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
#include "cookbook_socket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define BER_BOOLEAN    0x01
#define BER_INTEGER    0x02
#define BER_OCTET_STR  0x04
#define BER_ENUM       0x0A
#define BER_SEQUENCE   0x30
#define BER_SET        0x31
/* LDAP BindRequest: APPLICATION 0 CONSTRUCTED = 0x60 */
#define LDAP_BIND_REQ  0x60
/* LDAP BindResponse: APPLICATION 1 CONSTRUCTED = 0x61 */
#define LDAP_BIND_RESP 0x61
/* LDAP SearchRequest: APPLICATION 3 CONSTRUCTED = 0x63 */
#define LDAP_SEARCH_REQ  0x63
/* LDAP SearchResultEntry: APPLICATION 4 CONSTRUCTED = 0x64 */
#define LDAP_SEARCH_ENTRY 0x64
/* LDAP SearchResultDone: APPLICATION 5 CONSTRUCTED = 0x65 */
#define LDAP_SEARCH_DONE  0x65
/* context-specific primitive 0 (simple auth) = 0x80 */
#define LDAP_AUTH_SIMPLE 0x80
/* equalityMatch filter: context 3 constructed = 0xA3 */
#define LDAP_FILTER_EQUALITY 0xA3

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

/* ---- LDAP SearchRequest (RFC 4511 §4.5) ---- */

/*
 * SearchRequest ::= [APPLICATION 3] SEQUENCE {
 *     baseObject      LDAPDN,
 *     scope           ENUMERATED (wholeSubtree=2),
 *     derefAliases    ENUMERATED (neverDerefAliases=0),
 *     sizeLimit       INTEGER (0=no limit),
 *     timeLimit       INTEGER (10 seconds),
 *     typesOnly       BOOLEAN (FALSE),
 *     filter          equalityMatch [3] { attr, value },
 *     attributes      SEQUENCE OF attributeDescription
 * }
 */
static int build_search_request(unsigned char *buf, size_t buf_sz,
                                  size_t *out_len, int msg_id,
                                  const char *base_dn,
                                  const char *attr, const char *value,
                                  const char *want_attr) {
    unsigned char body[1024];
    size_t pos = 0;

    /* baseObject */
    size_t base_len = strlen(base_dn);
    pos += ber_write_tlv(body + pos, BER_OCTET_STR,
                          (const unsigned char *)base_dn, base_len);

    /* scope: wholeSubtree (2) */
    unsigned char scope_val = 2;
    pos += ber_write_tlv(body + pos, BER_ENUM, &scope_val, 1);

    /* derefAliases: neverDerefAliases (0) */
    unsigned char deref_val = 0;
    pos += ber_write_tlv(body + pos, BER_ENUM, &deref_val, 1);

    /* sizeLimit: 1 (only need one entry) */
    pos += ber_write_int(body + pos, 1);

    /* timeLimit: 10 seconds */
    pos += ber_write_int(body + pos, 10);

    /* typesOnly: FALSE */
    unsigned char false_val = 0;
    pos += ber_write_tlv(body + pos, BER_BOOLEAN, &false_val, 1);

    /* filter: equalityMatch [3] { attributeDesc, assertionValue } */
    {
        size_t attr_len = strlen(attr);
        size_t val_len = strlen(value);
        size_t eq_body = ber_tlv_size(attr_len) + ber_tlv_size(val_len);

        body[pos++] = LDAP_FILTER_EQUALITY;
        pos += ber_write_length(body + pos, eq_body);
        pos += ber_write_tlv(body + pos, BER_OCTET_STR,
                              (const unsigned char *)attr, attr_len);
        pos += ber_write_tlv(body + pos, BER_OCTET_STR,
                              (const unsigned char *)value, val_len);
    }

    /* attributes: SEQUENCE { want_attr } */
    {
        size_t wa_len = strlen(want_attr);
        size_t inner = ber_tlv_size(wa_len);
        body[pos++] = BER_SEQUENCE;
        pos += ber_write_length(body + pos, inner);
        pos += ber_write_tlv(body + pos, BER_OCTET_STR,
                              (const unsigned char *)want_attr, wa_len);
    }

    /* wrap in SearchRequest [APPLICATION 3] */
    size_t search_body = pos;
    size_t msgid_size = ber_tlv_size(1);
    size_t search_size = ber_tlv_size(search_body);
    size_t msg_body = msgid_size + search_size;
    size_t total = ber_tlv_size(msg_body);
    if (total > buf_sz) return -1;

    size_t off = 0;
    buf[off++] = BER_SEQUENCE;
    off += ber_write_length(buf + off, msg_body);
    off += ber_write_int(buf + off, msg_id);
    buf[off++] = LDAP_SEARCH_REQ;
    off += ber_write_length(buf + off, search_body);
    memcpy(buf + off, body, search_body);
    off += search_body;

    *out_len = off;
    return 0;
}

/*
 * Parse SearchResultEntry to extract attribute values.
 * Looks for the specified attribute and concatenates values as comma-separated.
 * Returns malloc'd string or NULL.
 */
static char *parse_search_entry_attr(const unsigned char *buf, size_t buf_len,
                                       const char *want_attr) {
    size_t pos = 0;

    /* outer SEQUENCE */
    if (pos >= buf_len || buf[pos] != BER_SEQUENCE) return NULL;
    pos++;
    ber_read_length(buf, buf_len, &pos);

    /* messageID (skip) */
    ber_read_int(buf, buf_len, &pos);

    /* check for SearchResultEntry (0x64) */
    if (pos >= buf_len || buf[pos] != LDAP_SEARCH_ENTRY) return NULL;
    pos++;
    size_t entry_len = ber_read_length(buf, buf_len, &pos);
    size_t entry_end = pos + entry_len;
    if (entry_end > buf_len) return NULL;

    /* objectName (OCTET STRING — skip) */
    if (pos >= entry_end || buf[pos] != BER_OCTET_STR) return NULL;
    pos++;
    size_t dn_len = ber_read_length(buf, buf_len, &pos);
    pos += dn_len;

    /* attributes: SEQUENCE OF PartialAttribute */
    if (pos >= entry_end || buf[pos] != BER_SEQUENCE) return NULL;
    pos++;
    size_t attrs_len = ber_read_length(buf, buf_len, &pos);
    size_t attrs_end = pos + attrs_len;
    if (attrs_end > buf_len) attrs_end = buf_len;

    size_t want_len = strlen(want_attr);
    char *result = NULL;
    size_t result_len = 0;
    size_t result_cap = 0;

    /* iterate PartialAttribute entries */
    while (pos < attrs_end) {
        /* PartialAttribute ::= SEQUENCE { type, SET OF values } */
        if (buf[pos] != BER_SEQUENCE) break;
        pos++;
        size_t pa_len = ber_read_length(buf, buf_len, &pos);
        size_t pa_end = pos + pa_len;
        if (pa_end > attrs_end) break;

        /* type (OCTET STRING) */
        if (buf[pos] != BER_OCTET_STR) { pos = pa_end; continue; }
        pos++;
        size_t type_len = ber_read_length(buf, buf_len, &pos);
        const unsigned char *type_val = buf + pos;
        pos += type_len;

        /* check if this is the attribute we want */
        int match = (type_len == want_len &&
                      memcmp(type_val, want_attr, want_len) == 0);
        /* case-insensitive fallback */
        if (!match && type_len == want_len) {
            match = 1;
            for (size_t ci = 0; ci < want_len; ci++) {
                char a = (char)type_val[ci], b = want_attr[ci];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { match = 0; break; }
            }
        }

        if (!match) { pos = pa_end; continue; }

        /* SET OF values */
        if (pos >= pa_end || buf[pos] != BER_SET) { pos = pa_end; continue; }
        pos++;
        size_t set_len = ber_read_length(buf, buf_len, &pos);
        size_t set_end = pos + set_len;
        if (set_end > pa_end) set_end = pa_end;

        while (pos < set_end) {
            if (buf[pos] != BER_OCTET_STR) break;
            pos++;
            size_t vlen = ber_read_length(buf, buf_len, &pos);
            const char *val = (const char *)(buf + pos);
            pos += vlen;

            /* extract CN from DN-style values like "CN=group,OU=..." */
            const char *cn = val;
            size_t cn_len = vlen;
            if (vlen > 3 && (val[0] == 'C' || val[0] == 'c') &&
                (val[1] == 'N' || val[1] == 'n') && val[2] == '=') {
                cn = val + 3;
                cn_len = vlen - 3;
                const char *comma = memchr(cn, ',', cn_len);
                if (comma) cn_len = (size_t)(comma - cn);
            }

            /* append to result */
            size_t needed = result_len + cn_len + 2; /* +comma+null */
            if (needed > result_cap) {
                result_cap = needed + 256;
                char *tmp = realloc(result, result_cap);
                if (!tmp) { free(result); return NULL; }
                result = tmp;
            }
            if (result_len > 0) result[result_len++] = ',';
            memcpy(result + result_len, cn, cn_len);
            result_len += cn_len;
            result[result_len] = '\0';
        }
        pos = pa_end;
    }

    return result;
}

/* Read a complete LDAP message from the connection. Returns bytes read. */
static int ldap_recv_message(unsigned char *buf, size_t buf_sz,
                               int use_tls, cookbook_sock_t raw_s,
                               cookbook_tls_sock *tls_s) {
    #define LRECV(b, n) \
        (use_tls ? (cookbook_tls_sock_recv(tls_s, b, n) == (int)(n) ? 0 : -1) \
                 : cookbook_sock_recv(raw_s, b, n))

    if (LRECV(buf, 2) != 0) return -1;

    size_t total_len, hdr_extra = 0;
    if (buf[1] < 0x80) {
        total_len = (size_t)buf[1];
    } else {
        hdr_extra = (size_t)(buf[1] & 0x7F);
        if (hdr_extra > 3 || hdr_extra == 0) return -1;
        if (LRECV(buf + 2, hdr_extra) != 0) return -1;
        total_len = 0;
        for (size_t i = 0; i < hdr_extra; i++)
            total_len = (total_len << 8) | buf[2 + i];
    }

    size_t hdr_size = 2 + hdr_extra;
    if (total_len + hdr_size > buf_sz) return -1;
    if (total_len > 0 && LRECV(buf + hdr_size, total_len) != 0) return -1;

    #undef LRECV
    return (int)(hdr_size + total_len);
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

    int use_tls = 0;
    if (strncmp(host, "ldaps://", 8) == 0) {
        host += 8;
        port = 636;
        use_tls = 1;
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

    /* connect (plain or TLS) */
    cookbook_sock_t raw_s = COOKBOOK_SOCK_INVALID;
    cookbook_tls_sock *tls_s = NULL;

    if (use_tls) {
        tls_s = cookbook_sock_connect_tls(host_buf, port, 5);
        if (!tls_s) return -1;
    } else {
        raw_s = cookbook_sock_connect(host_buf, port, 5);
        if (raw_s == COOKBOOK_SOCK_INVALID) return -1;
    }

    /* helper macros for send/recv/close over plain or TLS */
    #define LDAP_SEND(data, len) \
        (use_tls ? cookbook_tls_sock_send(tls_s, data, len) \
                 : cookbook_sock_send(raw_s, data, len))
    #define LDAP_RECV(buf, len) \
        (use_tls ? (cookbook_tls_sock_recv(tls_s, buf, len) == (int)(len) ? 0 : -1) \
                 : cookbook_sock_recv(raw_s, buf, len))
    #define LDAP_CLOSE() do { \
        if (use_tls) cookbook_tls_sock_close(tls_s); \
        else cookbook_sock_close(raw_s); \
    } while(0)

    /* build and send BindRequest */
    unsigned char req[2048];
    size_t req_len = 0;
    if (build_bind_request(req, sizeof(req), &req_len, 1,
                            user_dn, password) != 0) {
        LDAP_CLOSE();
        return -1;
    }

    if (LDAP_SEND(req, req_len) != 0) {
        LDAP_CLOSE();
        return -1;
    }

    /* receive BindResponse — read header first to get length */
    unsigned char resp[4096];
    if (LDAP_RECV(resp, 2) != 0) {
        LDAP_CLOSE();
        return -1;
    }

    size_t total_len;
    size_t hdr_extra = 0;
    if (resp[1] < 0x80) {
        total_len = (size_t)resp[1];
    } else {
        hdr_extra = (size_t)(resp[1] & 0x7F);
        if (hdr_extra > 3 || hdr_extra == 0) { LDAP_CLOSE(); return -1; }
        if (LDAP_RECV(resp + 2, hdr_extra) != 0) {
            LDAP_CLOSE();
            return -1;
        }
        total_len = 0;
        for (size_t i = 0; i < hdr_extra; i++)
            total_len = (total_len << 8) | resp[2 + i];
    }

    size_t hdr_size = 2 + hdr_extra;
    if (total_len + hdr_size > sizeof(resp)) {
        LDAP_CLOSE();
        return -1;
    }

    /* read the rest of the message */
    if (total_len > 0 && LDAP_RECV(resp + hdr_size, total_len) != 0) {
        LDAP_CLOSE();
        return -1;
    }

    /* parse bind response */
    int rc = parse_bind_response(resp, hdr_size + total_len);

    if (rc != 0) {
        LDAP_CLOSE();
        rc = -1;
        goto ldap_cleanup;
    }
    rc = 0; /* bind succeeded */

    /* bind succeeded — search for group membership if requested */
    if (groups_out && cfg->group_attr) {
        const char *search_base = cfg->group_base ? cfg->group_base : cfg->base_dn;
        if (!search_base) search_base = "";

        unsigned char sreq[2048];
        size_t sreq_len = 0;
        if (build_search_request(sreq, sizeof(sreq), &sreq_len, 2,
                                   search_base, attr, subject,
                                   cfg->group_attr) == 0) {
            if (LDAP_SEND(sreq, sreq_len) == 0) {
                /* read SearchResultEntry (may be multiple, or SearchResultDone) */
                unsigned char sbuf[8192];
                int msg_sz = ldap_recv_message(sbuf, sizeof(sbuf),
                                                use_tls, raw_s, tls_s);
                if (msg_sz > 0) {
                    /* check if it's a SearchResultEntry (0x64) */
                    /* find the tag after messageID */
                    size_t sp2 = 0;
                    if (sbuf[sp2] == BER_SEQUENCE) {
                        sp2++; ber_read_length(sbuf, (size_t)msg_sz, &sp2);
                        ber_read_int(sbuf, (size_t)msg_sz, &sp2);
                        if (sp2 < (size_t)msg_sz && sbuf[sp2] == LDAP_SEARCH_ENTRY) {
                            *groups_out = parse_search_entry_attr(
                                sbuf, (size_t)msg_sz, cfg->group_attr);
                        }
                    }

                    /* consume remaining messages until SearchResultDone */
                    while (msg_sz > 0) {
                        sp2 = 0;
                        if (sbuf[sp2] == BER_SEQUENCE) {
                            sp2++; ber_read_length(sbuf, (size_t)msg_sz, &sp2);
                            ber_read_int(sbuf, (size_t)msg_sz, &sp2);
                            if (sp2 < (size_t)msg_sz &&
                                sbuf[sp2] == LDAP_SEARCH_DONE)
                                break;
                        }
                        msg_sz = ldap_recv_message(sbuf, sizeof(sbuf),
                                                    use_tls, raw_s, tls_s);
                    }
                }
            }
        }
    }

    LDAP_CLOSE();

ldap_cleanup:
    #undef LDAP_SEND
    #undef LDAP_RECV
    #undef LDAP_CLOSE

    return rc;
}
