#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_SPECIFICS_FREEBSD_COMPAT_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_SPECIFICS_FREEBSD_COMPAT_HPP

// AnyPS5 FreeBSD port: Linux-isms used by the core, mapped to their FreeBSD equivalents.
// Included explicitly by the core sources that need it (not forced into 3rdparty code, which may
// define its own gettid on FreeBSD). Empty on every other platform.
#if defined(__FreeBSD__)
#include <pthread.h>
#include <pthread_np.h>
#include <sys/types.h>

// Linux gettid(): the kernel id of the calling thread. FreeBSD's is pthread_getthreadid_np(), the
// lwpid (same value as thr_self()). Unlike Linux, the main thread's id is NOT the process id:
// "am I the main thread?" must use pthread_main_np(), not gettid() == getpid().
inline pid_t gettid() noexcept {
    return static_cast<pid_t>(pthread_getthreadid_np());
}

// glibc pthread_getattr_np(): FreeBSD's pthread_attr_get_np() fills an attribute object that must already
// be initialised. Same contract as glibc: on success the caller owns attr and destroys it.
inline int pthread_getattr_np(pthread_t thread, pthread_attr_t* attr) noexcept {
    if (const int error = pthread_attr_init(attr)) return error;
    if (const int error = pthread_attr_get_np(thread, attr)) {
        pthread_attr_destroy(attr);
        return error;
    }
    return 0;
}

// Linux CLOCK_MONOTONIC_RAW does not exist on FreeBSD. Vulkan drivers there never report the
// MONOTONIC_RAW time domain (Mesa falls back to CLOCK_MONOTONIC when the macro is missing), so code
// selecting it only needs to compile.
#include <time.h>
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

#include <dlfcn.h>
#include <link.h>
#ifndef RTLD_DL_LINKMAP
#define RTLD_DL_LINKMAP 2
#endif

// glibc dladdr1(..., RTLD_DL_LINKMAP): dladdr() plus the link_map of the object containing the address.
// FreeBSD rtld fills both dli_fbase (dladdr) and link_map::l_base (linkmap_add) with the object's mapbase,
// so the object is the entry of the r_debug chain whose l_base is dli_fbase. Only RTLD_DL_LINKMAP is
// supported (the only flag the core uses).
inline int dladdr1(const void* address, Dl_info* info, void** extra, int flags) noexcept {
    if (::dladdr(address, info) == 0) return 0;
    if (flags != RTLD_DL_LINKMAP || extra == nullptr) return 1;
    *extra = nullptr;
    void* const program = ::dlopen(nullptr, RTLD_LAZY);
    link_map* map = nullptr;
    const bool known = program != nullptr && ::dlinfo(program, RTLD_DI_LINKMAP, &map) == 0 && map != nullptr;
    if (program != nullptr) ::dlclose(program);
    if (!known) return 0;
    while (map->l_prev != nullptr) map = map->l_prev;
    for (; map != nullptr; map = map->l_next) {
        if (map->l_base == static_cast<caddr_t>(info->dli_fbase)) {
            *extra = map;
            return 1;
        }
    }
    return 0;
}
#endif

#endif
