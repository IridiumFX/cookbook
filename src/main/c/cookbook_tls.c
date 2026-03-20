/*
 * cookbook_tls.c — TLS 1.3 client (RFC 8446)
 *
 * Minimal implementation: ClientHello → ServerHello → key derivation →
 * decrypt EncryptedExtensions/Certificate/CertificateVerify/Finished →
 * send client Finished → application data.
 *
 * Cipher suite: TLS_AES_128_GCM_SHA256 (0x1301)
 * Key exchange: X25519 (0x001D)
 * Signature algorithms: RSA-PSS-RSAE-SHA256, ECDSA-SECP256R1-SHA256, Ed25519
 */

#include "cookbook_tls.h"
#include "cookbook_socket.h"

/* apennines modules */
#include <apennines/t2/crypto/cipher.h>
#include <apennines/t2/crypto/hash.h>
#include <apennines/t2/crypto/ec.h>
#include <apennines/t2/crypto/ct.h>
#include <apennines/t2/crypto/x509.h>
#include <apennines/t2/crypto/rsa.h>
#include <apennines/t2/crypto/ecdsa.h>
#include <apennines/t3/crypto/pki.h>
#include <apennines/t1/random/entropy.h>

/* cookbook's own Ed25519 for Ed25519 cert verify */
#include "cookbook_ed25519.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- TLS 1.3 constants ---- */

#define TLS_RECORD_MAX       16384
#define TLS_HANDSHAKE        22
#define TLS_APPLICATION_DATA 23
#define TLS_CHANGE_CIPHER    20
#define TLS_ALERT            21

#define HS_CLIENT_HELLO      1
#define HS_SERVER_HELLO      2
#define HS_ENC_EXTENSIONS    8
#define HS_CERTIFICATE       11
#define HS_CERT_VERIFY       15
#define HS_FINISHED          20

#define TLS_AES_128_GCM_SHA256 0x1301
#define TLS_X25519              0x001D

/* TLS 1.3 uses legacy version 0x0303 in record layer */
#define TLS_LEGACY_VERSION   0x0303
#define TLS_VERSION_13       0x0304

#define HKDF_HASH_LEN 32  /* SHA-256 output length */

/* global CA trust store (set once at startup, read-only after) */
static pki_store *g_ca_store = NULL;

void cookbook_tls_set_ca_store(void *store) {
    g_ca_store = (pki_store *)store;
}

/* ---- TLS connection state ---- */

struct cookbook_tls {
    cookbook_sock_t sock;

    /* traffic keys */
    aes_ctx     send_aes;
    aes_ctx     recv_aes;
    u8          send_iv[12];
    u8          recv_iv[12];
    u64         send_seq;
    u64         recv_seq;

    /* receive buffer for partial records */
    u8          recv_buf[TLS_RECORD_MAX + 256];
    size_t      recv_buf_len;
    size_t      recv_buf_pos;

    /* handshake transcript hash */
    sha256_ctx  transcript;
};

/* ---- Helpers ---- */

static void put_u16(u8 *p, uint16_t v) {
    p[0] = (u8)(v >> 8);
    p[1] = (u8)(v & 0xFF);
}

static void put_u24(u8 *p, uint32_t v) {
    p[0] = (u8)(v >> 16);
    p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)(v & 0xFF);
}

static uint16_t get_u16(const u8 *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get_u24(const u8 *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/* Build the nonce for AEAD: XOR the IV with the sequence number */
static void build_nonce(u8 *nonce, const u8 *iv, u64 seq) {
    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; i++)
        nonce[11 - i] ^= (u8)(seq >> (8 * i));
}

/* ---- TLS record layer ---- */

/* Send a TLS record. For handshake records before encryption,
   content_type is TLS_HANDSHAKE. After key derivation, wraps in
   TLS_APPLICATION_DATA with AEAD. */
static int tls_send_record(cookbook_tls *tls, u8 content_type,
                            const u8 *data, size_t len, int encrypted) {
    u8 header[5];

    if (!encrypted) {
        header[0] = content_type;
        put_u16(header + 1, TLS_LEGACY_VERSION);
        put_u16(header + 3, (uint16_t)len);
        if (cookbook_sock_send(tls->sock, header, 5) != 0) return -1;
        if (len > 0 && cookbook_sock_send(tls->sock, data, len) != 0)
            return -1;
        return 0;
    }

    /* encrypted record: plaintext + content_type byte, then AES-GCM */
    size_t plain_len = len + 1; /* data + inner content type */
    u8 *plain = malloc(plain_len);
    if (!plain) return -1;
    memcpy(plain, data, len);
    plain[len] = content_type; /* inner content type */

    size_t cipher_len = plain_len + 16; /* + GCM tag */
    u8 *ciphertext = malloc(cipher_len);
    if (!ciphertext) { free(plain); return -1; }

    /* build AAD: record header with ciphertext length */
    u8 aad[5];
    aad[0] = TLS_APPLICATION_DATA;
    put_u16(aad + 1, TLS_LEGACY_VERSION);
    put_u16(aad + 3, (uint16_t)cipher_len);

    u8 nonce[12], tag[16];
    build_nonce(nonce, tls->send_iv, tls->send_seq);
    tls->send_seq++;

    aes128_gcm_encrypt(ciphertext, tag, &tls->send_aes,
                        nonce, aad, 5, plain, (u64)plain_len);
    /* append tag to ciphertext */
    memcpy(ciphertext + plain_len, tag, 16);

    free(plain);

    /* send record header + ciphertext */
    if (cookbook_sock_send(tls->sock, aad, 5) != 0 ||
        cookbook_sock_send(tls->sock, ciphertext, cipher_len) != 0) {
        free(ciphertext);
        return -1;
    }

    free(ciphertext);
    return 0;
}

/* Read a TLS record. Returns content type, fills *out (malloc'd), *out_len.
   If encrypted, decrypts in place. */
static int tls_recv_record(cookbook_tls *tls, u8 *out_type,
                            u8 **out, size_t *out_len, int encrypted) {
    u8 header[5];
    if (cookbook_sock_recv(tls->sock, header, 5) != 0) return -1;

    *out_type = header[0];
    size_t rec_len = get_u16(header + 3);
    if (rec_len > TLS_RECORD_MAX + 256) return -1;

    u8 *rec = malloc(rec_len);
    if (!rec) return -1;
    if (cookbook_sock_recv(tls->sock, rec, rec_len) != 0) {
        free(rec);
        return -1;
    }

    if (!encrypted || header[0] != TLS_APPLICATION_DATA) {
        *out = rec;
        *out_len = rec_len;
        return 0;
    }

    /* decrypt: last 16 bytes are GCM tag */
    if (rec_len < 17) { free(rec); return -1; }
    size_t cipher_body = rec_len - 16;
    u8 *tag = rec + cipher_body;

    u8 nonce[12];
    build_nonce(nonce, tls->recv_iv, tls->recv_seq);
    tls->recv_seq++;

    /* AAD is the record header as received */
    u8 *plain = malloc(cipher_body);
    if (!plain) { free(rec); return -1; }

    unsigned long rc = aes128_gcm_decrypt(plain, &tls->recv_aes,
                                           nonce, header, 5,
                                           rec, (u64)cipher_body, tag);
    free(rec);
    if (rc != 0) { free(plain); return -1; }

    /* strip inner content type (last byte of plaintext) */
    if (cipher_body == 0) { free(plain); return -1; }

    /* skip padding zeros from the end */
    size_t pt_len = cipher_body;
    while (pt_len > 0 && plain[pt_len - 1] == 0) pt_len--;
    if (pt_len == 0) { free(plain); return -1; }

    *out_type = plain[pt_len - 1]; /* inner content type */
    *out_len = pt_len - 1;
    /* shift data if needed — it's already in place */
    *out = plain;
    return 0;
}

/* ---- TLS 1.3 Key Schedule (RFC 8446 §7.1) ---- */

/* Derive-Secret: HKDF-Expand-Label(Secret, Label, Hash, Length) */
static void tls13_derive_secret(u8 *out, size_t out_len,
                                 const u8 *secret,
                                 const char *label, size_t label_len,
                                 const u8 *hash, size_t hash_len) {
    /* HkdfLabel structure:
       uint16 length
       opaque label<7..255> = "tls13 " + Label
       opaque context<0..255> = Hash */
    u8 info[512];
    size_t pos = 0;
    info[pos++] = (u8)(out_len >> 8);
    info[pos++] = (u8)(out_len & 0xFF);
    size_t full_label_len = 6 + label_len; /* "tls13 " prefix */
    info[pos++] = (u8)full_label_len;
    memcpy(info + pos, "tls13 ", 6); pos += 6;
    memcpy(info + pos, label, label_len); pos += label_len;
    info[pos++] = (u8)hash_len;
    if (hash_len > 0) { memcpy(info + pos, hash, hash_len); pos += hash_len; }

    hkdf_expand(out, (u64)out_len, HMAC_HASH_SHA256,
                 secret, HKDF_HASH_LEN, info, (u64)pos);
}

/* ---- ClientHello construction ---- */

static size_t build_client_hello(u8 *buf, size_t buf_sz,
                                  const char *hostname,
                                  const x25519_pubkey *key_share,
                                  const u8 *random32) {
    /* We build the handshake message body first, then wrap in record */
    u8 body[1024];
    size_t pos = 0;

    /* ProtocolVersion legacy_version = 0x0303 */
    put_u16(body + pos, TLS_LEGACY_VERSION); pos += 2;

    /* Random (32 bytes) */
    memcpy(body + pos, random32, 32); pos += 32;

    /* legacy_session_id: 32 bytes (for middlebox compat) */
    u8 session_id[32];
    entropy_get_system(session_id, 32);
    body[pos++] = 32;
    memcpy(body + pos, session_id, 32); pos += 32;

    /* cipher suites: TLS_AES_128_GCM_SHA256 only */
    put_u16(body + pos, 2); pos += 2; /* length */
    put_u16(body + pos, TLS_AES_128_GCM_SHA256); pos += 2;

    /* compression methods: null only */
    body[pos++] = 1;
    body[pos++] = 0;

    /* ---- extensions ---- */
    size_t ext_start = pos;
    pos += 2; /* placeholder for extensions length */

    /* supported_versions (0x002B): offer TLS 1.3 only */
    put_u16(body + pos, 0x002B); pos += 2;
    put_u16(body + pos, 3); pos += 2; /* ext data length */
    body[pos++] = 2; /* list length */
    put_u16(body + pos, TLS_VERSION_13); pos += 2;

    /* supported_groups (0x000A): X25519 */
    put_u16(body + pos, 0x000A); pos += 2;
    put_u16(body + pos, 4); pos += 2;
    put_u16(body + pos, 2); pos += 2;
    put_u16(body + pos, TLS_X25519); pos += 2;

    /* key_share (0x0033): X25519 public key */
    put_u16(body + pos, 0x0033); pos += 2;
    put_u16(body + pos, 36); pos += 2; /* ext data length */
    put_u16(body + pos, 34); pos += 2; /* client shares length */
    put_u16(body + pos, TLS_X25519); pos += 2;
    put_u16(body + pos, 32); pos += 2; /* key length */
    memcpy(body + pos, key_share->data, 32); pos += 32;

    /* signature_algorithms (0x000D) */
    put_u16(body + pos, 0x000D); pos += 2;
    put_u16(body + pos, 8); pos += 2; /* ext data length */
    put_u16(body + pos, 6); pos += 2; /* list length */
    put_u16(body + pos, 0x0804); pos += 2; /* rsa_pss_rsae_sha256 */
    put_u16(body + pos, 0x0403); pos += 2; /* ecdsa_secp256r1_sha256 */
    put_u16(body + pos, 0x0807); pos += 2; /* ed25519 */

    /* SNI (0x0000) */
    if (hostname && hostname[0]) {
        size_t hlen = strlen(hostname);
        put_u16(body + pos, 0x0000); pos += 2;
        put_u16(body + pos, (uint16_t)(hlen + 5)); pos += 2;
        put_u16(body + pos, (uint16_t)(hlen + 3)); pos += 2;
        body[pos++] = 0; /* host_name type */
        put_u16(body + pos, (uint16_t)hlen); pos += 2;
        memcpy(body + pos, hostname, hlen); pos += hlen;
    }

    /* fill in extensions length */
    put_u16(body + ext_start, (uint16_t)(pos - ext_start - 2));

    /* wrap in handshake message header */
    if (pos + 4 > buf_sz) return 0;
    size_t total = 0;
    buf[total++] = HS_CLIENT_HELLO;
    put_u24(buf + total, (uint32_t)pos); total += 3;
    memcpy(buf + total, body, pos); total += pos;

    return total;
}

/* ---- Handshake ---- */

cookbook_tls *cookbook_tls_connect(cookbook_sock_t sock, const char *hostname) {
    cookbook_tls *tls = calloc(1, sizeof(*tls));
    if (!tls) return NULL;
    tls->sock = sock;

    /* initialize transcript hash */
    sha256_init(&tls->transcript);

    /* generate X25519 ephemeral key pair */
    x25519_keypair eph;
    if (x25519_keygen(&eph) != 0) { free(tls); return NULL; }

    /* generate client random */
    u8 client_random[32];
    entropy_get_system(client_random, 32);

    /* build and send ClientHello */
    u8 ch_buf[2048];
    size_t ch_len = build_client_hello(ch_buf, sizeof(ch_buf),
                                        hostname, &eph.pub, client_random);
    if (ch_len == 0) { free(tls); return NULL; }

    /* update transcript with ClientHello (handshake message, not record) */
    sha256_update(&tls->transcript, ch_buf, (u64)ch_len);

    /* send as TLS record */
    if (tls_send_record(tls, TLS_HANDSHAKE, ch_buf, ch_len, 0) != 0) {
        free(tls);
        return NULL;
    }

    /* ---- Receive ServerHello ---- */
    u8 rec_type;
    u8 *sh_rec = NULL;
    size_t sh_rec_len = 0;
    if (tls_recv_record(tls, &rec_type, &sh_rec, &sh_rec_len, 0) != 0 ||
        rec_type != TLS_HANDSHAKE || sh_rec_len < 4 ||
        sh_rec[0] != HS_SERVER_HELLO) {
        free(sh_rec); free(tls);
        return NULL;
    }

    /* update transcript with ServerHello */
    sha256_update(&tls->transcript, sh_rec, (u64)sh_rec_len);

    /* parse ServerHello: extract server's X25519 key share */
    size_t sh_body_len = get_u24(sh_rec + 1);
    const u8 *sh = sh_rec + 4;
    if (sh_body_len + 4 > sh_rec_len) { free(sh_rec); free(tls); return NULL; }

    /* skip: version(2) + random(32) + session_id_len(1) + session_id + cipher(2) + comp(1) */
    size_t sp = 2 + 32; /* version + random */
    if (sp >= sh_body_len) { free(sh_rec); free(tls); return NULL; }
    u8 sid_len = sh[sp++];
    sp += sid_len;
    sp += 2; /* cipher suite */
    sp += 1; /* compression */

    /* parse extensions to find key_share */
    u8 server_pub[32] = {0};
    int found_key_share = 0;
    if (sp + 2 <= sh_body_len) {
        size_t ext_total = get_u16(sh + sp); sp += 2;
        size_t ext_end = sp + ext_total;
        while (sp + 4 <= ext_end && sp + 4 <= sh_body_len) {
            uint16_t etype = get_u16(sh + sp); sp += 2;
            uint16_t elen  = get_u16(sh + sp); sp += 2;
            if (etype == 0x0033 && elen >= 36) {
                /* key_share: group(2) + key_len(2) + key(32) */
                uint16_t group = get_u16(sh + sp);
                uint16_t klen  = get_u16(sh + sp + 2);
                if (group == TLS_X25519 && klen == 32) {
                    memcpy(server_pub, sh + sp + 4, 32);
                    found_key_share = 1;
                }
            }
            sp += elen;
        }
    }
    free(sh_rec);

    if (!found_key_share) { free(tls); return NULL; }

    /* ---- Key Schedule ---- */

    /* shared secret via X25519 */
    u8 shared_secret[32];
    x25519_pubkey srv_pub;
    memcpy(srv_pub.data, server_pub, 32);
    if (x25519_dh(shared_secret, &eph.priv, &srv_pub) != 0) {
        free(tls);
        return NULL;
    }

    /* early_secret = HKDF-Extract(salt=0, ikm=0) */
    u8 zero_key[HKDF_HASH_LEN] = {0};
    u8 early_secret[HKDF_HASH_LEN];
    hkdf_extract(early_secret, HMAC_HASH_SHA256,
                  zero_key, HKDF_HASH_LEN, zero_key, HKDF_HASH_LEN);

    /* derive_secret(early, "derived", empty_hash) */
    u8 empty_hash[32];
    sha256_digest(empty_hash, NULL, 0);
    u8 derived[HKDF_HASH_LEN];
    tls13_derive_secret(derived, HKDF_HASH_LEN, early_secret,
                         "derived", 7, empty_hash, 32);

    /* handshake_secret = HKDF-Extract(salt=derived, ikm=shared_secret) */
    u8 hs_secret[HKDF_HASH_LEN];
    hkdf_extract(hs_secret, HMAC_HASH_SHA256,
                  derived, HKDF_HASH_LEN, shared_secret, 32);

    /* transcript hash up to ServerHello */
    u8 sh_hash[32];
    {
        sha256_ctx tmp = tls->transcript; /* snapshot */
        sha256_final(sh_hash, &tmp);
    }

    /* client/server handshake traffic secrets */
    u8 c_hs_secret[HKDF_HASH_LEN], s_hs_secret[HKDF_HASH_LEN];
    tls13_derive_secret(c_hs_secret, HKDF_HASH_LEN, hs_secret,
                         "c hs traffic", 12, sh_hash, 32);
    tls13_derive_secret(s_hs_secret, HKDF_HASH_LEN, hs_secret,
                         "s hs traffic", 12, sh_hash, 32);

    /* derive handshake keys */
    u8 s_hs_key[16], s_hs_iv[12];
    tls13_derive_secret(s_hs_key, 16, s_hs_secret, "key", 3, NULL, 0);
    tls13_derive_secret(s_hs_iv, 12, s_hs_secret, "iv", 2, NULL, 0);

    u8 c_hs_key[16], c_hs_iv[12];
    tls13_derive_secret(c_hs_key, 16, c_hs_secret, "key", 3, NULL, 0);
    tls13_derive_secret(c_hs_iv, 12, c_hs_secret, "iv", 2, NULL, 0);

    /* set up handshake decryption */
    aes_ctx hs_recv_aes, hs_send_aes;
    aes128_init(&hs_recv_aes, s_hs_key);
    aes128_init(&hs_send_aes, c_hs_key);
    memcpy(tls->recv_iv, s_hs_iv, 12);
    memcpy(tls->send_iv, c_hs_iv, 12);
    tls->recv_aes = hs_recv_aes;
    tls->send_aes = hs_send_aes;
    tls->recv_seq = 0;
    tls->send_seq = 0;

    /* ---- Receive encrypted handshake messages ---- */
    /* Skip CCS if present (middlebox compat) */
    /* Then: EncryptedExtensions, Certificate, CertificateVerify, Finished */

    /* state for cert verification across messages */
    x509_cert server_cert;
    int have_cert = 0;
    memset(&server_cert, 0, sizeof(server_cert));

    int got_finished = 0;
    while (!got_finished) {
        u8 *msg = NULL;
        size_t msg_len = 0;
        u8 mtype;

        if (tls_recv_record(tls, &mtype, &msg, &msg_len, 1) != 0) {
            /* might be CCS (unencrypted) */
            free(msg);
            /* try reading as unencrypted CCS */
            u8 *ccs = NULL;
            size_t ccs_len = 0;
            if (tls_recv_record(tls, &mtype, &ccs, &ccs_len, 0) != 0) {
                free(ccs); free(tls);
                return NULL;
            }
            if (mtype == TLS_CHANGE_CIPHER) {
                free(ccs);
                continue; /* skip CCS, read next */
            }
            free(ccs); free(tls);
            return NULL;
        }

        if (mtype == TLS_CHANGE_CIPHER) {
            free(msg);
            continue; /* skip CCS */
        }

        if (mtype != TLS_HANDSHAKE || msg_len < 4) {
            free(msg);
            if (mtype == TLS_ALERT) { free(tls); return NULL; }
            continue;
        }

        /* snapshot transcript BEFORE adding this message */
        sha256_ctx pre_msg_transcript = tls->transcript;

        /* update transcript with this handshake message */
        sha256_update(&tls->transcript, msg, (u64)msg_len);

        /* process handshake message */
        u8 hs_type = msg[0];
        size_t hs_len = get_u24(msg + 1);

        switch (hs_type) {
        case HS_ENC_EXTENSIONS:
            break;

        case HS_CERTIFICATE: {
            /* TLS 1.3 Certificate message (RFC 8446 §4.4.2):
               opaque certificate_request_context<0..255>;
               CertificateEntry certificate_list<0..2^24-1>;
               CertificateEntry = opaque cert_data<1..2^24-1>; extensions<0..2^16-1>; */
            if (hs_len < 5) break;
            const u8 *p = msg + 4;
            u8 ctx_len = *p++;
            p += ctx_len; /* skip certificate_request_context */

            /* certificate_list length (3 bytes) */
            if (p + 3 > msg + 4 + hs_len) break;
            /* uint32_t list_len = get_u24(p); */ p += 3;

            /* first certificate entry */
            if (p + 3 > msg + 4 + hs_len) break;
            uint32_t cert_len = get_u24(p); p += 3;
            if (p + cert_len > msg + 4 + hs_len) break;

            /* parse the X.509 certificate (DER) */
            if (x509_parse(&server_cert, p, (u64)cert_len) == 0) {
                have_cert = 1;

                /* check expiry */
                unsigned long expired = 0;
                x509_is_expired(&expired, &server_cert, (u64)time(NULL));
                if (expired) {
                    x509_destroy(&server_cert);
                    have_cert = 0;
                    free(msg);
                    free(tls);
                    return NULL;
                }

                /* verify certificate chain against CA store */
                if (g_ca_store) {
                    /* collect all certs from the message */
                    /* for now, verify just the leaf cert */
                    pki_cert leaf = { .data = (u8 *)p, .len = (u64)cert_len };
                    pki_verify_result vr;
                    if (pki_store_verify(&vr, g_ca_store, &leaf, 1,
                                          (u64)time(NULL)) == 0) {
                        if (!vr.valid) {
                            x509_destroy(&server_cert);
                            have_cert = 0;
                            free(msg);
                            free(tls);
                            return NULL;
                        }
                    }
                }
            }
            break;
        }

        case HS_CERT_VERIFY: {
            /* TLS 1.3 CertificateVerify (RFC 8446 §4.4.3):
               SignatureScheme algorithm (2 bytes);
               opaque signature<0..2^16-1>; */
            if (!have_cert || hs_len < 4) break;

            uint16_t sig_scheme = get_u16(msg + 4);
            uint16_t sig_len = get_u16(msg + 6);
            const u8 *sig = msg + 8;
            if (sig_len + 8 > msg_len) break;

            /* build the content to verify:
               64 spaces + "TLS 1.3, server CertificateVerify" + 0x00 + transcript_hash */
            u8 verify_content[130];
            memset(verify_content, 0x20, 64); /* 64 spaces */
            memcpy(verify_content + 64,
                   "TLS 1.3, server CertificateVerify", 33);
            verify_content[97] = 0x00;

            /* transcript hash up to but NOT including CertificateVerify */
            u8 cv_hash[32];
            {
                sha256_ctx tmp = pre_msg_transcript;
                sha256_final(cv_hash, &tmp);
            }
            memcpy(verify_content + 98, cv_hash, 32);

            int sig_ok = 0;

            if (sig_scheme == 0x0804) {
                /* rsa_pss_rsae_sha256 */
                const u8 *spki = NULL; u64 spki_len = 0;
                x509_get_pubkey(&spki, &spki_len, &server_cert);
                if (spki && spki_len > 0) {
                    /* extract RSA key from SubjectPublicKeyInfo */
                    rsa_pubkey rpk;
                    /* SPKI contains AlgorithmIdentifier + BIT STRING wrapping the key */
                    /* For RSA, the BIT STRING payload is the PKCS#1 RSAPublicKey */
                    /* Simple approach: scan for the inner SEQUENCE */
                    const u8 *key_der = spki;
                    u64 key_len = spki_len;
                    /* skip outer SEQUENCE tag+len, AlgId, BIT STRING tag+len+0x00 */
                    if (key_der[0] == 0x30 && spki_len > 24) {
                        /* find the BIT STRING (tag 0x03) */
                        size_t pos = 0;
                        pos++; /* skip SEQUENCE tag */
                        /* skip SEQUENCE length */
                        if (key_der[pos] & 0x80) pos += 1 + (key_der[pos] & 0x7F);
                        else pos++;
                        /* skip AlgorithmIdentifier (SEQUENCE) */
                        if (key_der[pos] == 0x30) {
                            pos++;
                            if (key_der[pos] & 0x80) pos += 1 + (key_der[pos] & 0x7F);
                            else { pos += 1 + key_der[pos]; }
                        }
                        /* now at BIT STRING */
                        if (pos < spki_len && key_der[pos] == 0x03) {
                            pos++;
                            size_t bs_len;
                            if (key_der[pos] & 0x80) {
                                int nb = key_der[pos] & 0x7F;
                                pos++;
                                bs_len = 0;
                                for (int bi = 0; bi < nb; bi++)
                                    bs_len = (bs_len << 8) | key_der[pos++];
                            } else {
                                bs_len = key_der[pos++];
                            }
                            pos++; /* skip unused bits byte (0x00) */
                            key_der = key_der + pos;
                            key_len = bs_len - 1;
                        }
                    }
                    if (rsa_pubkey_import_der(&rpk, key_der, key_len) == 0) {
                        unsigned long valid = 0;
                        rsa_verify_pss(&valid, &rpk,
                                        sig, (u64)sig_len,
                                        verify_content, 130);
                        if (valid) sig_ok = 1;
                        rsa_pubkey_destroy(&rpk);
                    }
                }
            } else if (sig_scheme == 0x0403) {
                /* ecdsa_secp256r1_sha256 */
                const u8 *spki = NULL; u64 spki_len = 0;
                x509_get_pubkey(&spki, &spki_len, &server_cert);
                if (spki && spki_len >= 65) {
                    /* extract uncompressed P-256 point from SPKI */
                    /* look for 0x04 marker (uncompressed point) */
                    const u8 *pt = NULL;
                    for (u64 i = 0; i + 65 <= spki_len; i++) {
                        if (spki[i] == 0x04) {
                            pt = spki + i;
                            break;
                        }
                    }
                    if (pt) {
                        ecdsa_pubkey epk;
                        memcpy(epk.data, pt, 65);
                        /* DER-decode the ECDSA signature (r,s from ASN.1) */
                        /* TLS sends DER-encoded, ecdsa_verify wants raw r||s */
                        if (sig_len > 6 && sig[0] == 0x30) {
                            ecdsa_sig esig;
                            /* parse DER: SEQUENCE { INTEGER r, INTEGER s } */
                            size_t sp2 = 2; /* skip 30 + len */
                            if (sig[1] & 0x80) sp2 += (sig[1] & 0x7F);
                            /* r */
                            if (sig[sp2] == 0x02) {
                                sp2++;
                                size_t rlen = sig[sp2++];
                                /* skip leading zero padding */
                                while (rlen > 32 && sig[sp2] == 0) { sp2++; rlen--; }
                                memset(esig.r, 0, 32);
                                memcpy(esig.r + 32 - rlen, sig + sp2, rlen);
                                sp2 += rlen;
                            }
                            /* s */
                            if (sig[sp2] == 0x02) {
                                sp2++;
                                size_t slen = sig[sp2++];
                                while (slen > 32 && sig[sp2] == 0) { sp2++; slen--; }
                                memset(esig.s, 0, 32);
                                memcpy(esig.s + 32 - slen, sig + sp2, slen);
                            }
                            u64 valid = 0;
                            ecdsa_verify(&valid, &epk,
                                          verify_content, 130, &esig);
                            if (valid) sig_ok = 1;
                        }
                    }
                }
            } else if (sig_scheme == 0x0807) {
                /* ed25519 */
                const u8 *spki = NULL; u64 spki_len = 0;
                x509_get_pubkey(&spki, &spki_len, &server_cert);
                if (spki && spki_len >= 32) {
                    /* find the 32-byte key in SPKI */
                    const u8 *pk = spki + spki_len - 32;
                    if (cookbook_ed25519_verify(sig,
                            (const char *)verify_content, 130, pk) == 0)
                        sig_ok = 1;
                }
            }

            if (!sig_ok) {
                /* signature verification failed */
                if (have_cert) x509_destroy(&server_cert);
                free(msg);
                free(tls);
                return NULL;
            }
            break;
        }

        case HS_FINISHED: {
            /* verify server Finished:
               finished_key = HKDF-Expand-Label(s_hs_secret, "finished", "", 32)
               verify_data = HMAC-SHA-256(finished_key, transcript_hash_before_finished) */
            u8 s_finished_key[HKDF_HASH_LEN];
            tls13_derive_secret(s_finished_key, HKDF_HASH_LEN,
                                 s_hs_secret, "finished", 8, NULL, 0);

            u8 pre_fin_hash[32];
            {
                sha256_ctx tmp = pre_msg_transcript;
                sha256_final(pre_fin_hash, &tmp);
            }

            u8 expected_verify[32];
            hmac_digest(expected_verify, HMAC_HASH_SHA256,
                         s_finished_key, HKDF_HASH_LEN,
                         pre_fin_hash, 32);

            /* compare with received verify_data (msg+4, 32 bytes) */
            unsigned long fin_match = 0;
            if (hs_len == 32)
                ct_compare(&fin_match, msg + 4, expected_verify, 32);
            if (hs_len != 32 || fin_match != 0) {
                if (have_cert) x509_destroy(&server_cert);
                free(msg);
                free(tls);
                return NULL;
            }

            got_finished = 1;
            break;
        }
        default:
            break;
        }
        free(msg);
    }

    /* clean up server cert */
    if (have_cert) x509_destroy(&server_cert);

    /* ---- Send client Finished ---- */

    /* finished_key = HKDF-Expand-Label(c_hs_secret, "finished", "", 32) */
    u8 c_finished_key[HKDF_HASH_LEN];
    tls13_derive_secret(c_finished_key, HKDF_HASH_LEN,
                         c_hs_secret, "finished", 8, NULL, 0);

    /* transcript hash at this point */
    u8 fin_hash[32];
    {
        sha256_ctx tmp = tls->transcript;
        sha256_final(fin_hash, &tmp);
    }

    /* verify_data = HMAC-SHA-256(finished_key, transcript_hash) */
    u8 verify_data[32];
    hmac_digest(verify_data, HMAC_HASH_SHA256,
                 c_finished_key, HKDF_HASH_LEN,
                 fin_hash, 32);

    /* build Finished handshake message */
    u8 fin_msg[36];
    fin_msg[0] = HS_FINISHED;
    put_u24(fin_msg + 1, 32);
    memcpy(fin_msg + 4, verify_data, 32);

    /* update transcript with client Finished */
    sha256_update(&tls->transcript, fin_msg, 36);

    /* send encrypted */
    if (tls_send_record(tls, TLS_HANDSHAKE, fin_msg, 36, 1) != 0) {
        free(tls);
        return NULL;
    }

    /* ---- Derive application traffic keys ---- */

    /* master_secret = HKDF-Extract(salt=derive_secret(hs_secret,"derived",empty_hash), ikm=0) */
    u8 derived2[HKDF_HASH_LEN];
    tls13_derive_secret(derived2, HKDF_HASH_LEN, hs_secret,
                         "derived", 7, empty_hash, 32);
    u8 master_secret[HKDF_HASH_LEN];
    hkdf_extract(master_secret, HMAC_HASH_SHA256,
                  derived2, HKDF_HASH_LEN, zero_key, HKDF_HASH_LEN);

    /* transcript hash including client Finished */
    u8 app_hash[32];
    {
        sha256_ctx tmp = tls->transcript;
        sha256_final(app_hash, &tmp);
    }

    /* application traffic secrets */
    u8 c_app_secret[HKDF_HASH_LEN], s_app_secret[HKDF_HASH_LEN];
    tls13_derive_secret(c_app_secret, HKDF_HASH_LEN, master_secret,
                         "c ap traffic", 12, app_hash, 32);
    tls13_derive_secret(s_app_secret, HKDF_HASH_LEN, master_secret,
                         "s ap traffic", 12, app_hash, 32);

    /* derive application keys */
    u8 s_app_key[16], s_app_iv[12];
    tls13_derive_secret(s_app_key, 16, s_app_secret, "key", 3, NULL, 0);
    tls13_derive_secret(s_app_iv, 12, s_app_secret, "iv", 2, NULL, 0);

    u8 c_app_key[16], c_app_iv[12];
    tls13_derive_secret(c_app_key, 16, c_app_secret, "key", 3, NULL, 0);
    tls13_derive_secret(c_app_iv, 12, c_app_secret, "iv", 2, NULL, 0);

    /* install application traffic keys */
    aes128_init(&tls->recv_aes, s_app_key);
    aes128_init(&tls->send_aes, c_app_key);
    memcpy(tls->recv_iv, s_app_iv, 12);
    memcpy(tls->send_iv, c_app_iv, 12);
    tls->recv_seq = 0;
    tls->send_seq = 0;

    /* zeroize key material */
    memset(shared_secret, 0, 32);
    memset(hs_secret, 0, HKDF_HASH_LEN);
    memset(master_secret, 0, HKDF_HASH_LEN);
    memset(&eph, 0, sizeof(eph));

    return tls;
}

/* ---- Application data ---- */

int cookbook_tls_send(cookbook_tls *tls, const void *data, size_t len) {
    if (!tls || !data || len == 0) return -1;
    return tls_send_record(tls, TLS_APPLICATION_DATA,
                            (const u8 *)data, len, 1);
}

int cookbook_tls_recv(cookbook_tls *tls, void *buf, size_t len) {
    if (!tls || !buf || len == 0) return -1;

    /* if we have buffered plaintext, return from that */
    if (tls->recv_buf_pos < tls->recv_buf_len) {
        size_t avail = tls->recv_buf_len - tls->recv_buf_pos;
        size_t n = avail < len ? avail : len;
        memcpy(buf, tls->recv_buf + tls->recv_buf_pos, n);
        tls->recv_buf_pos += n;
        return (int)n;
    }

    /* read a new record */
    u8 *msg = NULL;
    size_t msg_len = 0;
    u8 mtype;
    if (tls_recv_record(tls, &mtype, &msg, &msg_len, 1) != 0)
        return -1;

    if (mtype == TLS_ALERT) {
        free(msg);
        return 0; /* treat as EOF */
    }

    if (mtype != TLS_APPLICATION_DATA || msg_len == 0) {
        free(msg);
        return -1;
    }

    /* buffer the decrypted plaintext */
    size_t copy = msg_len < sizeof(tls->recv_buf) ? msg_len : sizeof(tls->recv_buf);
    memcpy(tls->recv_buf, msg, copy);
    tls->recv_buf_len = copy;
    tls->recv_buf_pos = 0;
    free(msg);

    /* return as much as requested */
    size_t n = copy < len ? copy : len;
    memcpy(buf, tls->recv_buf, n);
    tls->recv_buf_pos = n;
    return (int)n;
}

void cookbook_tls_close(cookbook_tls *tls) {
    if (!tls) return;

    /* send close_notify alert */
    u8 alert[2] = { 1, 0 }; /* warning, close_notify */
    tls_send_record(tls, TLS_ALERT, alert, 2, 1);

    /* zeroize keys */
    memset(&tls->send_aes, 0, sizeof(tls->send_aes));
    memset(&tls->recv_aes, 0, sizeof(tls->recv_aes));
    memset(tls->send_iv, 0, 12);
    memset(tls->recv_iv, 0, 12);

    free(tls);
}
