#ifndef PRX_LIBC_INCLUDE_SPECIFICS_LINUX_GUESTFAULTCOPY_HPP
#define PRX_LIBC_INCLUDE_SPECIFICS_LINUX_GUESTFAULTCOPY_HPP

#include <csignal>
#include <cstddef>

namespace GuestFaultCopy {

bool Ready();
bool Copy(void* destination, const void* source, std::size_t bytes);
// FreeBSD: whether a store to the byte would succeed, by a store that changes nothing (lock or $0).
bool ProbeWrite(void* address);
bool Resume(const siginfo_t* info, void* context);
void Reinstall();

}

#endif
