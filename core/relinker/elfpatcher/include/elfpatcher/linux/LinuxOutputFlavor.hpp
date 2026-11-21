#ifndef ELFPATCHER_LINUX_LINUXOUTPUTFLAVOR_HPP
#define ELFPATCHER_LINUX_LINUXOUTPUTFLAVOR_HPP

namespace Elfpatcher::Linux {

// --freebsd: the Linux ELF writers produce the same image for a FreeBSD host instead. Three differences, all in the
// container: EI_OSABI is ELFOSABI_FREEBSD (the PS5 executable's own value) instead of 0, PT_INTERP is FreeBSD's
// /libexec/ld-elf.so.1, and guest modules do not depend on ld-linux-x86-64.so.2 for __tls_get_addr (FreeBSD's
// run-time linker exports it itself). Set once by the command line before any conversion; false by default.
inline bool& FreeBsdOutput() {
    static bool freebsd = false;
    return freebsd;
}

inline constexpr unsigned char ElfOsAbiFreeBsd = 9;

}

#endif
