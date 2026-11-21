#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_SPECIFICS_FREEBSD_HOSTMEMORY_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_SPECIFICS_FREEBSD_HOSTMEMORY_HPP

// AnyPS5 FreeBSD port: the two address-space primitives the Linux code takes from /proc/self/maps and
// MAP_FIXED_NOREPLACE, done the FreeBSD way. Only for FreeBSD builds.
#if defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <utility>
#include <vector>

namespace FreeBsdHostMemory {

// The mappings of this process, sorted by address: sysctl kern.proc.vmmap (packed struct kinfo_vmentry
// records), the equivalent of reading /proc/self/maps, without procfs or libutil.
inline std::vector<std::pair<std::uintptr_t, std::uintptr_t>> Mappings() {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, static_cast<int>(getpid())};
    std::vector<char> buffer;
    std::size_t length = 0;
    for (int attempt = 0;; ++attempt) {
        if (sysctl(mib, 4, nullptr, &length, nullptr, 0) != 0) throw std::system_error(errno, std::generic_category(), "sysctl kern.proc.vmmap (size)");
        length += length / 3;   // the map may grow between the two calls
        buffer.resize(length);
        if (sysctl(mib, 4, buffer.data(), &length, nullptr, 0) == 0) break;
        if (errno != ENOMEM || attempt == 8) throw std::system_error(errno, std::generic_category(), "sysctl kern.proc.vmmap");
    }
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> mappings;
    for (std::size_t offset = 0; offset + sizeof(int) <= length;) {
        const auto* entry = reinterpret_cast<const struct kinfo_vmentry*>(buffer.data() + offset);
        if (entry->kve_structsize <= 0) break;
        if (entry->kve_end > entry->kve_start) mappings.emplace_back(static_cast<std::uintptr_t>(entry->kve_start), static_cast<std::uintptr_t>(entry->kve_end));
        offset += static_cast<std::size_t>(entry->kve_structsize);
    }
    return mappings;
}

// Linux mmap(MAP_FIXED_NOREPLACE): MAP_FIXED | MAP_EXCL. On a range already in use FreeBSD fails with
// EINVAL (mmap(2)); callers written for Linux retry or skip only on EEXIST, so EINVAL becomes EEXIST.
// With otherwise valid arguments EINVAL can only mean "in use" or "outside the user address space", and
// both make the range unusable, as EEXIST does. `flags` must not contain MAP_FIXED/MAP_EXCL.
inline void* MapNoReplace(void* address, std::size_t bytes, int protection, int flags, int fd = -1, off_t offset = 0) {
    void* const placed = mmap(address, bytes, protection, flags | MAP_FIXED | MAP_EXCL, fd, offset);
    if (placed == MAP_FAILED && errno == EINVAL) errno = EEXIST;
    return placed;
}

}
#endif

#endif
