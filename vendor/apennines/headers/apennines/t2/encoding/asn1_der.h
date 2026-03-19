#ifndef APENNINES_T2_ENCODING_ASN1_DER_H
#define APENNINES_T2_ENCODING_ASN1_DER_H

#include "apennines/export.h"
#include "apennines/types.h"
#include "apennines/t1/buffer/buf.h"

typedef enum {
    ASN1_TAG_BOOLEAN           = 0x01,
    ASN1_TAG_INTEGER           = 0x02,
    ASN1_TAG_BIT_STRING        = 0x03,
    ASN1_TAG_OCTET_STRING      = 0x04,
    ASN1_TAG_NULL              = 0x05,
    ASN1_TAG_OID               = 0x06,
    ASN1_TAG_UTF8_STRING       = 0x0C,
    ASN1_TAG_PRINTABLE_STRING  = 0x13,
    ASN1_TAG_IA5_STRING        = 0x16,
    ASN1_TAG_UTC_TIME          = 0x17,
    ASN1_TAG_GENERALIZED_TIME  = 0x18,
    ASN1_TAG_SEQUENCE          = 0x30,
    ASN1_TAG_SET               = 0x31,
    ASN1_TAG_CONTEXT_0         = 0xA0,
    ASN1_TAG_CONTEXT_1         = 0xA1,
    ASN1_TAG_CONTEXT_2         = 0xA2,
    ASN1_TAG_CONTEXT_3         = 0xA3
} asn1_tag;

typedef struct {
    u8        tag;
    const u8 *value;
    u64       value_len;
    const u8 *raw;
    u64       raw_len;
} asn1_tlv;

APENNINES_API unsigned long asn1_der_encode(buf *out, u8 tag,
                                            const u8 *value, u64 value_len);

APENNINES_API unsigned long asn1_der_encode_integer(buf *out,
                                                    const u8 *value,
                                                    u64 value_len);

APENNINES_API unsigned long asn1_der_encode_boolean(buf *out,
                                                    unsigned long value);

APENNINES_API unsigned long asn1_der_encode_null(buf *out);

APENNINES_API unsigned long asn1_der_encode_oid(buf *out,
                                                const u32 *components,
                                                u64 count);

APENNINES_API unsigned long asn1_der_encode_sequence(buf *out,
                                                     const u8 *content,
                                                     u64 content_len);

APENNINES_API unsigned long asn1_der_decode(asn1_tlv *out,
                                            const u8 *data, u64 len);

APENNINES_API unsigned long asn1_der_decode_integer(i64 *out,
                                                    const asn1_tlv *tlv);

APENNINES_API unsigned long asn1_der_decode_boolean(unsigned long *out,
                                                    const asn1_tlv *tlv);

APENNINES_API unsigned long asn1_der_decode_oid(u32 *out, u64 *count,
                                                u64 max_count,
                                                const asn1_tlv *tlv);

APENNINES_API unsigned long asn1_der_sequence_next(asn1_tlv *out,
                                                   const u8 **cursor,
                                                   u64 *remaining);

APENNINES_API unsigned long asn1_der_get_tag(u8 *out,
                                             const asn1_tlv *tlv);

APENNINES_API unsigned long asn1_der_get_length(u64 *out,
                                                const asn1_tlv *tlv);

APENNINES_API unsigned long asn1_der_get_value(const u8 **out,
                                               u64 *out_len,
                                               const asn1_tlv *tlv);

#endif /* APENNINES_T2_ENCODING_ASN1_DER_H */
