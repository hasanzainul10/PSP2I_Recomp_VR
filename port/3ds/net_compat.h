/* What the psprecomp networking code needs from <netinet/in.h> and
 * <sys/ioctl.h> that libctru doesn't declare. Force-included into the
 * runtime's 3DS build only (port/3ds/CMakeLists.txt), so the runtime's
 * sources stay as they are for the other platforms.
 *
 * The 3DS's network stack (soc:U) is IPv4 only: these declarations let the
 * IPv6 paths of the ad hoc code compile, and at run time socket(AF_INET6)
 * fails, which that code already treats as "no IPv6". FIONREAD isn't
 * supported either: the ioctl fails and the count reads 0 ("nothing
 * pending"). Both matter for online play only (3DS_PLAN.md T8). */
#ifndef PSP2I_3DS_NET_COMPAT_H
#define PSP2I_3DS_NET_COMPAT_H

#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ioctl.h>

#ifndef IPPROTO_IPV6
struct in6_addr {
    uint8_t s6_addr[16];
};
struct sockaddr_in6 {
    sa_family_t     sin6_family;
    in_port_t       sin6_port;
    uint32_t        sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;
};
#define IPPROTO_IPV6 41
#define IPV6_V6ONLY  26
static const struct in6_addr in6addr_any = { { 0 } };
#endif

#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46
#endif

#ifndef FIONREAD
#define FIONREAD 0x4004667F          /* BSD's value; soc:U rejects it */
#endif

#endif
