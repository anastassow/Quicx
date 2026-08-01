#ifndef PLATFORM_H
#define PLATFORM_H

/*
must be the first #include in any .c file that needs it — feature-test
macros only take effect if defined before the first system header pulls
in the C library's own feature-test logic. currently needed to unlock
endian.h's be64toh/htobe64 (glibc/Darwin libc hide them by default).
*/
#if defined(__linux__)
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include <stdint.h>

/*
64-bit network/host byte-order conversion — ntohl/htonl only cover 32
bits. macOS provides ntohll/htonll natively (arpa/inet.h); glibc does
not, so on Linux they're built from endian.h's be64toh/htobe64.
*/
#if defined(__linux__)
#include <endian.h>
#ifndef ntohll
#define ntohll(x) be64toh(x)
#endif
#ifndef htonll
#define htonll(x) htobe64(x)
#endif
#elif defined(__APPLE__)
#include <arpa/inet.h>
#endif

#endif
