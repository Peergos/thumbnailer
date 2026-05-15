/*
 * glibc version compatibility shim for Linux/x86-64.
 *
 * When built on glibc >= 2.33 the .so gets VERNEED entries for symbols like
 * stat64@GLIBC_2.33, pthread_create@GLIBC_2.34, hypot@GLIBC_2.35 that don't
 * exist on older distributions.
 *
 * Fix: export those newer-versioned symbols from this .so, implemented by
 * delegating to old-versioned equivalents that exist on glibc >= 2.2.5.
 *
 * Key trick: use top-level .symver GAS directives to map plain alias names to
 * specific old-versioned symbols.  Calling through the plain alias generates
 * "alias@PLT" in the assembly (valid GAS), whereas inline asm operands with
 * versioned names produce the rejected "sym@VER@PLT" syntax.
 *
 * stat64/lstat64/fstat64: the GLIBC_2.2.5 interface uses __xstat64/__lxstat64/
 * __fxstat64 (the xstat family).  Direct stat64@GLIBC_2.2.5 does not exist in
 * current libc; use __xstat64 with _STAT_VER instead.
 *
 * To verify no newer glibc required:
 *   nm -D thumbnailer.so | grep -E 'GLIBC_2\.(3[3-9]|[4-9][0-9])'
 * should show only T (provided) entries, no U (undefined).
 */
#if defined(__linux__) && defined(__x86_64__)

#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <sys/stat.h>
#include <pthread.h>
#include <math.h>

/* _STAT_VER is the version tag passed to __xstat64/__lxstat64/__fxstat64.
 * On Linux/x86-64 it is always 1; glibc 2.33+ no longer exposes it publicly
 * (the xstat family is deprecated) so we define it ourselves. */
#ifndef _STAT_VER
# define _STAT_VER 1
#endif

/* stat family: delegate to the xstat64 interface from GLIBC_2.2.5.
 * stat64@GLIBC_2.2.5 does not exist in glibc >= 2.33; __xstat64@GLIBC_2.2.5
 * is the equivalent old entry point. */
__asm__(".symver __compat_xstat64,  __xstat64@GLIBC_2.2.5");
__asm__(".symver __compat_lxstat64, __lxstat64@GLIBC_2.2.5");
__asm__(".symver __compat_fxstat64, __fxstat64@GLIBC_2.2.5");

extern int __compat_xstat64 (int ver, const char *path, struct stat64 *buf);
extern int __compat_lxstat64(int ver, const char *path, struct stat64 *buf);
extern int __compat_fxstat64(int ver, int fd,           struct stat64 *buf);

/* pthread and math: old versions exist directly at GLIBC_2.2.5. */
__asm__(".symver __compat_pthread_create, pthread_create@GLIBC_2.2.5");
__asm__(".symver __compat_pthread_join,   pthread_join@GLIBC_2.2.5");
__asm__(".symver __compat_pthread_once,   pthread_once@GLIBC_2.2.5");
__asm__(".symver __compat_hypot,          hypot@GLIBC_2.2.5");

extern int    __compat_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
extern int    __compat_pthread_join  (pthread_t, void **);
extern int    __compat_pthread_once  (pthread_once_t *, void (*)(void));
extern double __compat_hypot         (double, double);

/* Export each newer-versioned symbol from this .so; delegate to old version. */
__asm__(".symver __new_stat64,         stat64@@GLIBC_2.33");
__asm__(".symver __new_fstat64,        fstat64@@GLIBC_2.33");
__asm__(".symver __new_lstat64,        lstat64@@GLIBC_2.33");
__asm__(".symver __new_pthread_create, pthread_create@@GLIBC_2.34");
__asm__(".symver __new_pthread_join,   pthread_join@@GLIBC_2.34");
__asm__(".symver __new_pthread_once,   pthread_once@@GLIBC_2.34");
__asm__(".symver __new_hypot,          hypot@@GLIBC_2.35");

__attribute__((visibility("default"))) int    __new_stat64        (const char *p, struct stat64 *s)                                      { return __compat_xstat64(_STAT_VER, p, s);             }
__attribute__((visibility("default"))) int    __new_fstat64       (int fd, struct stat64 *s)                                             { return __compat_fxstat64(_STAT_VER, fd, s);           }
__attribute__((visibility("default"))) int    __new_lstat64       (const char *p, struct stat64 *s)                                      { return __compat_lxstat64(_STAT_VER, p, s);            }
__attribute__((visibility("default"))) double __new_hypot         (double x, double y)                                                   { return __compat_hypot(x, y);                          }
__attribute__((visibility("default"))) int    __new_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *arg) { return __compat_pthread_create(t, a, f, arg);         }
__attribute__((visibility("default"))) int    __new_pthread_join  (pthread_t t, void **r)                                                { return __compat_pthread_join(t, r);                   }
__attribute__((visibility("default"))) int    __new_pthread_once  (pthread_once_t *o, void (*f)(void))                                   { return __compat_pthread_once(o, f);                   }

#endif /* linux x86-64 */

/* dlsym and pthread_attr_setstacksize moved from libdl/libpthread into libc
 * at glibc 2.34; their ABI is unchanged but the VERNEED tag changed.
 * --wrap redirects all calls in linked objects through these wrappers, which
 * call the old-versioned symbol (2.2.5 on x86_64, 2.17 on aarch64). */
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#include <dlfcn.h>
#include <pthread.h>

# if defined(__x86_64__)
#  define _GLIBC_BASE "GLIBC_2.2.5"
# else
#  define _GLIBC_BASE "GLIBC_2.17"
# endif

__asm__(".symver __compat_dlsym,                    dlsym@" _GLIBC_BASE);
__asm__(".symver __compat_pthread_attr_setstacksize, pthread_attr_setstacksize@" _GLIBC_BASE);

extern void *__compat_dlsym(void *handle, const char *symbol);
extern int   __compat_pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize);

void *__wrap_dlsym                    (void *handle, const char *symbol)        { return __compat_dlsym(handle, symbol);                     }
int   __wrap_pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)  { return __compat_pthread_attr_setstacksize(attr, stacksize); }

#endif /* linux x86_64 + aarch64 */
