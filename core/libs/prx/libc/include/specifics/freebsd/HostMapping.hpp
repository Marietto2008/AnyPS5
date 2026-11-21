#ifndef PRX_LIBC_INCLUDE_SPECIFICS_FREEBSD_HOSTMAPPING_HPP
#define PRX_LIBC_INCLUDE_SPECIFICS_FREEBSD_HOSTMAPPING_HPP

// AnyPS5 FreeBSD port: HostMapping (specifics/linux/HostMapping.hpp) answered from the kern.proc.vmmap records,
// because FreeBSD has neither /proc/self/maps nor PROCMAP_QUERY. Same interface; included by the Linux header.
#if defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace HostMapping {

struct Mapping {
    std::uint64_t start;
    std::uint64_t end;
    std::string path;
    bool readable = false;
    bool writable = false;
    bool executable = false;
    bool shared = false;
    std::uint64_t inode = 0;
};

// No PROCMAP_QUERY descriptor on FreeBSD: callers that would use one take their other path.
inline int QueryFd() { return -1; }

// What callers read from the Linux fields:
// - inode == 0 && !shared means private anonymous memory;
// - anything else is backed by a file, a shared memory object or a device.
// Here anonymous means a default/swap object (or none yet) that is neither POSIX nor SysV shared memory. Private
// mappings of files and shared memory objects are always copy-on-write, so "shared" is a backed mapping without COW.
// FreeBSD gives no inode for a POSIX shared memory object; 1 stands for "backed" there.
inline Mapping FromEntry(const struct kinfo_vmentry& entry) {
    const bool sharedMemory = (entry.kve_flags & (KVME_FLAG_POSIXSHM | KVME_FLAG_SYSVSHM)) != 0;
    const bool anonymous = !sharedMemory && (entry.kve_type == KVME_TYPE_DEFAULT || entry.kve_type == KVME_TYPE_SWAP ||
        entry.kve_type == KVME_TYPE_NONE || entry.kve_type == KVME_TYPE_GUARD);
    Mapping mapping{static_cast<std::uint64_t>(entry.kve_start), static_cast<std::uint64_t>(entry.kve_end), entry.kve_path};
    mapping.readable = (entry.kve_protection & KVME_PROT_READ) != 0;
    mapping.writable = (entry.kve_protection & KVME_PROT_WRITE) != 0;
    mapping.executable = (entry.kve_protection & KVME_PROT_EXEC) != 0;
    mapping.shared = !anonymous && (entry.kve_flags & KVME_FLAG_COW) == 0;
    mapping.inode = anonymous ? 0 : entry.kve_type == KVME_TYPE_VNODE && entry.kve_vn_fileid != 0 ? static_cast<std::uint64_t>(entry.kve_vn_fileid) : 1;
    return mapping;
}

// The buffer of this thread's previous query. Every kern.proc.vmmap call makes the kernel count the resident pages of
// every mapping, one radix lookup per 4 KiB page wherever there is no superpage (milliseconds for a process with a few
// hundred MiB resident), so the size probe is made only when the previous buffer is too short.
inline std::vector<char>& EntryBuffer() {
    thread_local std::vector<char> buffer;
    return buffer;
}

// Calls visit(entry) for every mapping in address order until it returns false.
template <typename Visit>
void ForEachEntry(const char* caller, Visit&& visit) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, static_cast<int>(getpid())};
    struct Borrowed {
        std::vector<char> buffer = std::move(EntryBuffer());
        ~Borrowed() { EntryBuffer() = std::move(buffer); }
    } borrowed;   // a visit that queries again gets a buffer of its own
    auto& buffer = borrowed.buffer;
    std::size_t length = 0;
    for (int attempt = 0;; ++attempt) {
        if (!buffer.empty()) {
            length = buffer.size();
            if (sysctl(mib, 4, buffer.data(), &length, nullptr, 0) == 0) break;
            if (errno != ENOMEM || attempt == 8) throw std::runtime_error(std::string(caller) + ": kern.proc.vmmap failed");
        }
        if (sysctl(mib, 4, nullptr, &length, nullptr, 0) != 0) throw std::runtime_error(std::string(caller) + ": kern.proc.vmmap failed");
        length += length / 3;   // the map may grow between the two calls
        buffer.resize(length);
    }
    for (std::size_t offset = 0; offset + sizeof(int) <= length;) {
        const auto* entry = reinterpret_cast<const struct kinfo_vmentry*>(buffer.data() + offset);
        if (entry->kve_structsize <= 0) break;
        offset += static_cast<std::size_t>(entry->kve_structsize);
        if (!visit(*entry)) return;
    }
}

// The mapping that holds address or, failing that, the first one above it.
inline std::optional<Mapping> CoveringOrNext(std::uint64_t address, const char* caller) {
    std::optional<Mapping> found;
    ForEachEntry(caller, [&](const struct kinfo_vmentry& entry) {
        if (static_cast<std::uint64_t>(entry.kve_end) <= address) return true;
        found = FromEntry(entry);
        return false;
    });
    return found;
}

inline std::optional<Mapping> Find(std::uint64_t address, const char* caller) {
    auto mapping = CoveringOrNext(address, caller);
    if (mapping && mapping->start > address) return std::nullopt;
    return mapping;
}

template <typename Visit>
void Walk(std::uint64_t begin, std::uint64_t end, const char* caller, Visit&& visit) {
    if (begin >= end) return;
    ForEachEntry(caller, [&](const struct kinfo_vmentry& entry) {
        if (static_cast<std::uint64_t>(entry.kve_end) <= begin) return true;
        if (static_cast<std::uint64_t>(entry.kve_start) >= end) return false;
        visit(FromEntry(entry));
        return true;
    });
}

}
#endif

#endif
