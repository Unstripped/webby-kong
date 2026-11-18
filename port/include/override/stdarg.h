#ifndef PORT_OVERRIDE_STDARG_H
#define PORT_OVERRIDE_STDARG_H
/* Replaces IDO's include/compiler/ido/stdarg.h for clang. */
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_end(ap) __builtin_va_end(ap)
#endif
