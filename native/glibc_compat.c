/*
 * glibc version compatibility shim for Linux/x86-64.
 *
 * Building on a host with glibc >= 2.33 makes the .so require GLIBC_2.33,
 * which is absent on older distributions.  Each wrapper here satisfies the
 * newer versioned symbol internally by delegating to the older equivalent,
 * so the .so loads on any glibc >= 2.17.
 *
 * To list which newer GLIBC symbols a built .so requires:
 *   nm -D thumbnailer.so | grep -E 'GLIBC_2\.(3[3-9]|[4-9][0-9])'
 * Add a wrapper below for every symbol reported there.
 */
#if defined(__linux__) && defined(__x86_64__)
#include <stddef.h>

/* malloc_usable_size was rebumped to GLIBC_2.33 on x86-64 in glibc 2.33. */
extern size_t __compat_mus_old(void *) __asm__("malloc_usable_size@GLIBC_2.2.5");
__asm__(".symver __compat_mus_new, malloc_usable_size@GLIBC_2.33");
size_t __compat_mus_new(void *ptr) { return __compat_mus_old(ptr); }

#endif /* linux x86-64 */
