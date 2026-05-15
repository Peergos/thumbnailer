/*
 * glibc version compatibility shim for Linux/x86-64.
 *
 * Building on a host with glibc >= 2.33 makes the .so require newer versioned
 * symbols that are absent on older distributions.  Each wrapper here satisfies
 * the newer versioned symbol internally by delegating to the older equivalent,
 * so the .so loads on any glibc >= 2.17.
 *
 * To check what a built .so requires:
 *   nm -D thumbnailer.so | grep -E 'GLIBC_2\.(3[3-9]|[4-9][0-9])'
 */
#if defined(__linux__) && defined(__x86_64__)

#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <sys/stat.h>
#include <pthread.h>
#include <math.h>

/* ── stat64 / fstat64 / lstat64 → GLIBC_2.33 ──────────────────────────── */

extern int __compat_stat64_old  (const char *, struct stat64 *) __asm__("stat64@GLIBC_2.2.5");
extern int __compat_fstat64_old (int,           struct stat64 *) __asm__("fstat64@GLIBC_2.2.5");
extern int __compat_lstat64_old (const char *, struct stat64 *) __asm__("lstat64@GLIBC_2.2.5");

__asm__(".symver __compat_stat64_new,  stat64@GLIBC_2.33");
__asm__(".symver __compat_fstat64_new, fstat64@GLIBC_2.33");
__asm__(".symver __compat_lstat64_new, lstat64@GLIBC_2.33");

int __compat_stat64_new  (const char *p, struct stat64 *s) { return __compat_stat64_old(p, s);  }
int __compat_fstat64_new (int fd,        struct stat64 *s) { return __compat_fstat64_old(fd, s); }
int __compat_lstat64_new (const char *p, struct stat64 *s) { return __compat_lstat64_old(p, s); }

/* ── pthread_create / pthread_join / pthread_once → GLIBC_2.34 ─────────── */
/* In glibc 2.34 these moved from libpthread.so into libc.so.6.              */

extern int __compat_pthread_create_old(pthread_t *, const pthread_attr_t *,
                                       void *(*)(void *), void *)
    __asm__("pthread_create@GLIBC_2.2.5");
extern int __compat_pthread_join_old(pthread_t, void **)
    __asm__("pthread_join@GLIBC_2.2.5");
extern int __compat_pthread_once_old(pthread_once_t *, void (*)(void))
    __asm__("pthread_once@GLIBC_2.2.5");

__asm__(".symver __compat_pthread_create_new, pthread_create@GLIBC_2.34");
__asm__(".symver __compat_pthread_join_new,   pthread_join@GLIBC_2.34");
__asm__(".symver __compat_pthread_once_new,   pthread_once@GLIBC_2.34");

int __compat_pthread_create_new(pthread_t *t, const pthread_attr_t *a,
                                void *(*f)(void *), void *arg)
    { return __compat_pthread_create_old(t, a, f, arg); }
int __compat_pthread_join_new(pthread_t t, void **r)
    { return __compat_pthread_join_old(t, r); }
int __compat_pthread_once_new(pthread_once_t *o, void (*f)(void))
    { return __compat_pthread_once_old(o, f); }

/* ── hypot → GLIBC_2.35 ────────────────────────────────────────────────── */

extern double __compat_hypot_old(double, double) __asm__("hypot@GLIBC_2.2.5");
__asm__(".symver __compat_hypot_new, hypot@GLIBC_2.35");
double __compat_hypot_new(double x, double y) { return __compat_hypot_old(x, y); }

#endif /* linux x86-64 */
