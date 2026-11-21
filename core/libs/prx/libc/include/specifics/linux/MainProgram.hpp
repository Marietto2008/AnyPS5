#ifndef PRX_LIBC_INCLUDE_SPECIFICS_LINUX_MAINPROGRAM_HPP
#define PRX_LIBC_INCLUDE_SPECIFICS_LINUX_MAINPROGRAM_HPP

#include "prx/libc/include/specifics/linux/ElfTypes.hpp"
#if defined(__FreeBSD__)
#include <dlfcn.h>
#endif

// Is this loaded object the main program (the relinked title)? glibc reports it with an empty dlpi_name and
// l_name, and the macOS image list does the same. FreeBSD's rtld gives its path instead, so there it is
// recognised by what it is: the head of the link map list, and the object whose dynamic section is that map's
// l_ld. (elf_aux_info does not hand out AT_PHDR on FreeBSD.)

inline bool MainProgramImage(const dl_phdr_info& image) {
#if defined(__FreeBSD__)
    static const void* const mainDynamic = [] {
        void* self = ::dlopen(nullptr, RTLD_LAZY);
        link_map* map = nullptr;
        const bool known = self != nullptr && ::dlinfo(self, RTLD_DI_LINKMAP, &map) == 0 && map != nullptr;
        if (self != nullptr) ::dlclose(self);
        return known ? static_cast<const void*>(map->l_ld) : nullptr;
    }();
    if (mainDynamic == nullptr) return false;
    for (std::uint16_t index = 0; index < image.dlpi_phnum; ++index) {
        const auto& header = image.dlpi_phdr[index];
        if (header.p_type == PT_DYNAMIC) return reinterpret_cast<const void*>(image.dlpi_addr + header.p_vaddr) == mainDynamic;
    }
    return false;
#else
    return image.dlpi_name == nullptr || image.dlpi_name[0] == '\0';
#endif
}

#if defined(__linux__) || defined(__FreeBSD__) // the hosts with a real <link.h>
inline bool MainProgramMap(const link_map& map) {
#if defined(__FreeBSD__)
    return map.l_prev == nullptr;
#else
    return map.l_name == nullptr || map.l_name[0] == '\0';
#endif
}
#endif

#endif
