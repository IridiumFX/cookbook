#ifndef COOKBOOK_ED25519_H
#define COOKBOOK_ED25519_H

#include "cookbook.h"
#include <stddef.h>
#include <stdint.h>

/* Generate an Ed25519 key pair.
   pk: 32-byte public key (output).
   sk: 64-byte secret key = seed[32] || pk[32] (output).
   Returns 0 on success, -1 on CSPRNG failure. */
COOKBOOK_API int cookbook_ed25519_keygen(unsigned char pk[32],
                                        unsigned char sk[64]);

/* Sign a message (RFC 8032 Ed25519, deterministic).
   sig: 64-byte signature (output).
   msg/msg_len: message to sign.
   sk: 64-byte secret key (seed || pk).
   Returns 0 on success. */
COOKBOOK_API int cookbook_ed25519_sign(unsigned char sig[64],
                                      const void *msg, size_t msg_len,
                                      const unsigned char sk[64]);

/* Verify a signature (RFC 8032 Ed25519).
   Returns 0 if valid, -1 if invalid. */
COOKBOOK_API int cookbook_ed25519_verify(const unsigned char sig[64],
                                        const void *msg, size_t msg_len,
                                        const unsigned char pk[32]);

#endif /* COOKBOOK_ED25519_H */
