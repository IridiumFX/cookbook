#ifndef APENNINES_T2_ADDR_H
#define APENNINES_T2_ADDR_H

#include "apennines/export.h"
#include "apennines/types.h"

typedef struct {
    u8 octets[4];
} ipv4_addr;

typedef struct {
    u8 octets[16];
} ipv6_addr;

typedef struct {
    int family;  /* 4 or 6 */
    union {
        ipv4_addr v4;
        ipv6_addr v6;
    } addr;
    u16 port;
} net_sock_addr;

APENNINES_API unsigned long addr_ipv4_parse(ipv4_addr *out, const char *str);
APENNINES_API unsigned long addr_ipv4_format(char *out, u64 out_cap, const ipv4_addr *addr);
APENNINES_API unsigned long addr_ipv6_parse(ipv6_addr *out, const char *str);
APENNINES_API unsigned long addr_ipv6_format(char *out, u64 out_cap, const ipv6_addr *addr);
APENNINES_API unsigned long addr_sockaddr_create(net_sock_addr *out, const char *host, u16 port);

#endif /* APENNINES_T2_ADDR_H */
