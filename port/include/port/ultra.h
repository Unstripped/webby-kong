#ifndef PORT_ULTRA_H
#define PORT_ULTRA_H
/* libultra types for port TUs (which use musl, not the IDO libc headers). */
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#define _SIZE_T_DEF
/* os_libc.h prototypes for these clash with musl's strings.h */
#define bcopy __ultra_bcopy
#define bzero __ultra_bzero
#define bcmp __ultra_bcmp
#define sprintf __ultra_sprintf
#include <ultra64.h>
#undef bcopy
#undef bzero
#undef bcmp
#undef sprintf

#include "port/port.h"
#endif
