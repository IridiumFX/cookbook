/*
 * cookbook_ldap.c — Zero-dependency LDAP simple bind (RFC 4511)
 *
 * Implements just enough of the LDAP protocol to perform a simple bind
 * (password authentication) against any LDAPv3 server. Uses BER encoding
 * (X.690) for the wire format. No libldap or wldap32 — pure C11 over
 * a platform-abstracted socket.
 *
 * The socket layer uses the same pattern as cookbook_grid.c (raw TCP).
 * On Nova this will swap to the Nova networking API.
 */

#include "cookbook_ldap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Platform socket abstraction ---- */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define sock_close(s) closesocket(s)
#define sock_errno WSAGetLastError()
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
#define sock_close(s) close(s)
#define sock_errno errno
#endif

static sock_t tcp_connect(const char *host, int port) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return SOCK_INVALID;

    sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == SOCK_INVALID) { freeaddrinfo(res); return SOCK_INVALID; }

    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        sock_close(s);
        freeaddrinfo(res);
        return SOCK_INVALID;
    }

    freeaddrinfo(res);
    return s;
}

static int sock_send_all(sock_t s, const unsigned char *buf, size_t len) {
    while (len > 0) {
        int n = send(s, (const char *)buf, (int)len, 0);
        if (n <= 0) return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static int sock_recv_all(sock_t s, unsigned char *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        int n = recv(s, (char *)buf + got, (int)(len - got), 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

/* ---- BER encoding helpers (X.690, DER subset) ---- */

/* Write a BER length field. Returns bytes written. */
static size_t ber_write_length(unsigned char *buf, size_t len) {
    if (len < 0x80) {
        buf[0] = (unsigned char)len;
        return 1;
    } else if (len <= 0xFF) {
        buf[0] = 0x81;
        buf[1] = (unsigned char)len;
        return 2;
    } else if (len <= 0xFFFF) {
        buf[0] = 0x82;
        buf[1] = (unsigned char)(len >> 8);
        buf[2] = (unsigned char)(len & 0xFF);
        return 3;
    }
    /* lengths > 64k not needed for bind */
    buf[0] = 0x83;
    buf[1] = (unsigned char)(len >> 16);
    buf[2] = (unsigned char)((len >> 8) & 0xFF);
    buf[3] = (unsigned char)(len & 0xFF);
    return 4;
}

/* How many bytes a length field would take */
static size_t ber_length_size(size_t len) {
    if (len < 0x80)   return 1;
    if (len <= 0xFF)   return 2;
    if (len <= 0xFFFF) return 3;
    return 4;
}

/* Write a complete TLV: tag + length + value. Returns total bytes written. */
static size_t ber_write_tlv(unsigned char *buf, unsigned char tag,
                             const unsigned char *val, size_t vlen) {
    size_t off = 0;
    buf[off++] = tag;
    off += ber_write_length(buf + off, vlen);
    if (val && vlen > 0) {
        memcpy(buf + off, val, vlen);
        off += vlen;
    }
    return off;
}

/* Size of a complete TLV */
static size_t ber_tlv_size(size_t vlen) {
    return 1 + ber_length_size(vlen) + vlen;
}

/* BER tag constants */
#define BER_INTEGER    0x02
#define BER_OCTET_STR  0x04
#define BER_SEQUENCE   0x30
/* LDAP BindRequest: APPLICATION 0 CONSTRUCTED = 0x60 */
#define LDAP_BIND_REQ  0x60
/* LDAP BindResponse: APPLICATION 1 CONSTRUCTED = 0x61 */
#define LDAP_BIND_RESP 0x61
/* context-specific primitive 0 (simple auth) = 0x80 */
#define LDAP_AUTH_SIMPLE 0x80

/* Write a BER INTEGER with value v (must be >= 0, fits in 4 bytes) */
static size_t ber_write_int(unsigned char *buf, int v) {
    unsigned char vbuf[4];
    int vlen;
    if (v < 0x80) {
        vbuf[0] = (unsigned char)v;
        vlen = 1;
    } else if (v < 0x8000) {
        vbuf[0] = (unsigned char)(v >> 8);
        vbuf[1] = (unsigned char)(v & 0xFF);
        vlen = 2;
    } else {
        vbuf[0] = (unsigned char)(v >> 16);
        vbuf[1] = (unsigned char)((v >> 8) & 0xFF);
        vbuf[2] = (unsigned char)(v & 0xFF);
        vlen = 3;
    }
    return ber_write_tlv(buf, BER_INTEGER, vbuf, (size_t)vlen);
}

/* ---- BER decoding helpers ---- */

/* Read a BER length from buf. Advances *pos. Returns the length. */
static size_t ber_read_length(const unsigned char *buf, size_t buf_len,
                               size_t *pos) {
    if (*pos >= buf_len) return 0;
    unsigned char b = buf[(*pos)++];
    if (b < 0x80) return (size_t)b;

    int nbytes = b & 0x7F;
    if (nbytes > 3 || *pos + (size_t)nbytes > buf_len) return 0;

    size_t len = 0;
    for (int i = 0; i < nbytes; i++)
        len = (len << 8) | buf[(*pos)++];
    return len;
}

/* Read a BER INTEGER value. Returns the int value, advances *pos past TLV. */
static int ber_read_int(const unsigned char *buf, size_t buf_len,
                         size_t *pos) {
    if (*pos >= buf_len || buf[*pos] != BER_INTEGER) return -1;
    (*pos)++; /* skip tag */
    size_t vlen = ber_read_length(buf, buf_len, pos);
    if (vlen == 0 || *pos + vlen > buf_len) return -1;

    int val = 0;
    /* handle sign extension for first byte */
    if (buf[*pos] & 0x80) val = -1;
    for (size_t i = 0; i < vlen; i++)
        val = (val << 8) | buf[(*pos)++];
    return val;
}

/* ---- LDAP BindRequest/BindResponse ---- */

/*
 * LDAP BindRequest (RFC 4511 §4.2):
 *   BindRequest ::= [APPLICATION 0] SEQUENCE {
 *       version        INTEGER (3),
 *       name           LDAPDN (OCTET STRING),
 *       authentication AuthenticationChoice
 *   }
 *   AuthenticationChoice ::= CHOICE {
 *       simple [0] OCTET STRING,  -- password
 *   }
 *
 * Wrapped in an LDAPMessage:
 *   LDAPMessage ::= SEQUENCE {
 *       messageID  INTEGER,
 *       protocolOp BindRequest
 *   }
 */

static int build_bind_request(unsigned char *buf, size_t buf_sz,
                               size_t *out_len, int msg_id,
                               const char *dn, const char *password) {
    size_t dn_len = strlen(dn);
    size_t pw_len = strlen(password);

    /* calculate sizes bottom-up */
    size_t ver_size = ber_tlv_size(1); /* INTEGER 3 = 1 byte */
    size_t dn_size  = ber_tlv_size(dn_len);
    size_t pw_size  = ber_tlv_size(pw_len); /* [0] password */
    size_t bind_body = ver_size + dn_size + pw_size;
    size_t bind_size = ber_tlv_size(bind_body); /* [APPLICATION 0] */
    size_t msgid_size = ber_tlv_size(1); /* msg id, assume < 128 */
    size_t msg_body = msgid_size + bind_size;
    size_t total = ber_tlv_size(msg_body); /* SEQUENCE */

    if (total > buf_sz) return -1;

    size_t off = 0;

    /* LDAPMessage SEQUENCE */
    buf[off++] = BER_SEQUENCE;
    off += ber_write_length(buf + off, msg_body);

    /* messageID */
    off += ber_write_int(buf + off, msg_id);

    /* BindRequest [APPLICATION 0] */
    buf[off++] = LDAP_BIND_REQ;
    off += ber_write_length(buf + off, bind_body);

    /* version INTEGER (3) */
    off += ber_write_int(buf + off, 3);

    /* name OCTET STRING (DN) */
    off += ber_write_tlv(buf + off, BER_OCTET_STR,
                          (const unsigned char *)dn, dn_len);

    /* authentication [0] OCTET STRING (password) */
    off += ber_write_tlv(buf + off, LDAP_AUTH_SIMPLE,
                          (const unsigned char *)password, pw_len);

    *out_len = off;
    return 0;
}

/*
 * LDAP BindResponse (RFC 4511 §4.2.2):
 *   BindResponse ::= [APPLICATION 1] SEQUENCE {
 *       resultCode  ENUMERATED,
 *       matchedDN   LDAPDN,
 *       diagnosticMessage LDAPString,
 *       ...
 *   }
 *   resultCode 0 = success
 */

static int parse_bind_response(const unsigned char *buf, size_t buf_len) {
    size_t pos = 0;

    /* outer SEQUENCE */
    if (pos >= buf_len || buf[pos] != BER_SEQUENCE) return -1;
    pos++;
    size_t seq_len = ber_read_length(buf, buf_len, &pos);
    if (seq_len == 0) return -1;
    (void)seq_len;

    /* messageID (skip) */
    int mid = ber_read_int(buf, buf_len, &pos);
    (void)mid;

    /* BindResponse tag */
    if (pos >= buf_len || buf[pos] != LDAP_BIND_RESP) return -1;
    pos++;
    size_t resp_len = ber_read_length(buf, buf_len, &pos);
    (void)resp_len;

    /* resultCode is ENUMERATED (tag 0x0A), encoded like INTEGER */
    if (pos >= buf_len) return -1;
    unsigned char rc_tag = buf[pos++];
    if (rc_tag != 0x0A) return -1; /* ENUMERATED */
    size_t rc_len = ber_read_length(buf, buf_len, &pos);
    if (rc_len == 0 || pos + rc_len > buf_len) return -1;

    int result_code = 0;
    for (size_t i = 0; i < rc_len; i++)
        result_code = (result_code << 8) | buf[pos++];

    /* resultCode 0 = success */
    return (result_code == 0) ? 0 : -1;
}

/* ---- Public API ---- */

int cookbook_ldap_bind(const cookbook_ldap_config *cfg,
                       const char *subject,
                       const char *password,
                       char **groups_out) {
    if (!cfg || !cfg->url || !subject || !password) return -1;
    if (groups_out) *groups_out = NULL;

    /* parse URL: ldap://host:port */
    const char *host = cfg->url;
    int port = 389;

    if (strncmp(host, "ldaps://", 8) == 0) {
        host += 8;
        port = 636;
        /* Note: LDAPS (TLS) not yet supported — requires TLS handshake */
        return -1; /* TODO: TLS support */
    } else if (strncmp(host, "ldap://", 7) == 0) {
        host += 7;
    }

    char host_buf[256];
    snprintf(host_buf, sizeof(host_buf), "%s", host);
    char *colon = strchr(host_buf, ':');
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
    }
    /* strip trailing slash */
    size_t hlen = strlen(host_buf);
    if (hlen > 0 && host_buf[hlen - 1] == '/') host_buf[hlen - 1] = '\0';

    /* build user DN */
    const char *attr = cfg->user_attr ? cfg->user_attr : "uid";
    char user_dn[512];
    snprintf(user_dn, sizeof(user_dn), "%s=%s,%s",
             attr, subject, cfg->base_dn ? cfg->base_dn : "");

    /* connect */
    sock_t s = tcp_connect(host_buf, port);
    if (s == SOCK_INVALID) return -1;

    /* build and send BindRequest */
    unsigned char req[2048];
    size_t req_len = 0;
    if (build_bind_request(req, sizeof(req), &req_len, 1,
                            user_dn, password) != 0) {
        sock_close(s);
        return -1;
    }

    if (sock_send_all(s, req, req_len) != 0) {
        sock_close(s);
        return -1;
    }

    /* receive BindResponse — read header first to get length */
    unsigned char resp[4096];
    /* read first 2 bytes: tag + first length byte */
    if (sock_recv_all(s, resp, 2) != 0) {
        sock_close(s);
        return -1;
    }

    size_t total_len;
    size_t hdr_extra = 0;
    if (resp[1] < 0x80) {
        total_len = (size_t)resp[1];
    } else {
        hdr_extra = (size_t)(resp[1] & 0x7F);
        if (hdr_extra > 3 || hdr_extra == 0) { sock_close(s); return -1; }
        if (sock_recv_all(s, resp + 2, hdr_extra) != 0) {
            sock_close(s);
            return -1;
        }
        total_len = 0;
        for (size_t i = 0; i < hdr_extra; i++)
            total_len = (total_len << 8) | resp[2 + i];
    }

    size_t hdr_size = 2 + hdr_extra;
    if (total_len + hdr_size > sizeof(resp)) {
        sock_close(s);
        return -1;
    }

    /* read the rest of the message */
    if (total_len > 0 &&
        sock_recv_all(s, resp + hdr_size, total_len) != 0) {
        sock_close(s);
        return -1;
    }

    sock_close(s);

    /* parse response */
    int rc = parse_bind_response(resp, hdr_size + total_len);

    /* Note: group lookup via LDAP search is not yet implemented.
       For now, groups come from the cookbook credentials/policies table.
       A future enhancement can add SearchRequest after successful bind. */

    return rc;
}
