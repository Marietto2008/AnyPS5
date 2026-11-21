#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_SPECIFICS_FREEBSD_LINUXMMAPFLAGS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_SPECIFICS_FREEBSD_LINUXMMAPFLAGS_HPP

// AnyPS5 FreeBSD port, for test programs only: the two Linux mmap flags the GPU driver tests pass, given their
// FreeBSD meaning. Library code uses FreeBsdHostMemory::MapNoReplace instead, which also reports EEXIST.
#if defined(__FreeBSD__)
#include <sys/mman.h>

// FreeBSD has no MAP_NORESERVE: a private mapping is never charged against swap up front unless vm.overcommit asks.
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

// Same placement rule (fixed address, fail rather than replace); an occupied range fails with EINVAL, not EEXIST.
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE (MAP_FIXED | MAP_EXCL)
#endif
#endif

#endif
