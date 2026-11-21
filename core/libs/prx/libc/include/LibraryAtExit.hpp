#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_LIBRARYATEXIT_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_LIBRARYATEXIT_HPP

#include <cstdlib>

#if defined(__FreeBSD__)
extern "C" int __cxa_atexit(void (*function)(void*), void* argument, void* dso);
extern "C" __attribute__((visibility("hidden"))) void* __dso_handle;
#endif

// An exit handler bound to the library that registers it, which is what atexit already is on glibc: the
// handler runs when the library is finalized, in LIFO order with its static destructors. FreeBSD's atexit is
// process-wide, and rtld finalizes the libraries (destroying their statics) before such a handler runs.
// static: each library must pass its own __dso_handle, so no copy of this function may be shared.
[[maybe_unused]] static int LibraryAtExit(void (*handler)()) {
#if defined(__FreeBSD__)
    return __cxa_atexit([](void* function) { reinterpret_cast<void (*)()>(function)(); }, reinterpret_cast<void*>(handler), &__dso_handle);
#else
    return std::atexit(handler);
#endif
}

#endif
