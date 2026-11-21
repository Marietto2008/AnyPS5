#include <cstdint>
#include <cstdlib>

// macOS and FreeBSD have their own __stack_chk_guard (FreeBSD: long[8], filled by libc at startup); a plain-named
// guest one would replace it for the whole process wherever the libraries are loaded unpatched.
#if defined(__APPLE__) || defined(__FreeBSD__)
extern "C" std::uintptr_t __stack_chk_guard_nid_postfix = 0xDEADBEEFCAFEBABEull;
#else
std::uintptr_t __stack_chk_guard = 0xDEADBEEFCAFEBABEull;
#endif
