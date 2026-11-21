#if (defined(__linux__) || defined(__FreeBSD__)) && defined(__x86_64__)
#include "prx/libc/include/specifics/linux/GuestFaultCopy.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <cstdint>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <sys/syscall.h>
#include <unistd.h>
#include <iterator>
#include <ucontext.h>
#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif

extern "C" {
std::size_t GuestFaultCopyBytes(void* destination, const void* source, std::size_t bytes);
extern const char GuestFaultCopyAccess[];
extern const char GuestFaultCopyResume[];
int GuestFaultCopyProbeWrite(void* address);
extern const char GuestFaultCopyProbeAccess[];
extern const char GuestFaultCopyProbeResume[];
}

__asm__(
    ".pushsection .text\n"
    ".p2align 4\n"
    ".globl GuestFaultCopyBytes\n"
    ".hidden GuestFaultCopyBytes\n"
    ".type GuestFaultCopyBytes, @function\n"
    "GuestFaultCopyBytes:\n"
    ".cfi_startproc\n"
    "    movq %rdx, %rcx\n"
    ".globl GuestFaultCopyAccess\n"
    ".hidden GuestFaultCopyAccess\n"
    "GuestFaultCopyAccess:\n"
    "    rep movsb\n"
    ".globl GuestFaultCopyResume\n"
    ".hidden GuestFaultCopyResume\n"
    "GuestFaultCopyResume:\n"
    "    movq %rcx, %rax\n"
    "    ret\n"
    ".cfi_endproc\n"
    ".size GuestFaultCopyBytes, .-GuestFaultCopyBytes\n"
    // A write access that changes nothing: 0 when the byte is writable, 1 when the store faulted.
    ".p2align 4\n"
    ".globl GuestFaultCopyProbeWrite\n"
    ".hidden GuestFaultCopyProbeWrite\n"
    ".type GuestFaultCopyProbeWrite, @function\n"
    "GuestFaultCopyProbeWrite:\n"
    ".cfi_startproc\n"
    "    movl $1, %eax\n"
    ".globl GuestFaultCopyProbeAccess\n"
    ".hidden GuestFaultCopyProbeAccess\n"
    "GuestFaultCopyProbeAccess:\n"
    "    lock orb $0, (%rdi)\n"
    "    xorl %eax, %eax\n"
    ".globl GuestFaultCopyProbeResume\n"
    ".hidden GuestFaultCopyProbeResume\n"
    "GuestFaultCopyProbeResume:\n"
    "    ret\n"
    ".cfi_endproc\n"
    ".size GuestFaultCopyProbeWrite, .-GuestFaultCopyProbeWrite\n"
    ".popsection\n");

namespace {

constexpr int CopySignals[] = {SIGSEGV, SIGBUS};
constexpr unsigned SegvSlot = 1u << 0;
constexpr unsigned AllSlots = (1u << std::size(CopySignals)) - 1;

struct sigaction g_previous[2][std::size(CopySignals)]{};
std::atomic<int> g_active{0};
std::atomic<pid_t> g_installer{0};
std::atomic<unsigned> g_pending{0};
std::atomic<bool> g_ready{false};

// A signal another process (or raise) sent, as opposed to a fault: si_code <= 0 on Linux, SI_USER (0x10001) and
// above on FreeBSD, whose faults carry small positive codes as Linux does.
bool SentByProcess(const siginfo_t* info) {
#if defined(__FreeBSD__)
    return info->si_code <= 0 || info->si_code >= SI_USER;
#else
    return info->si_code <= 0;
#endif
}

void Chain(int signal, siginfo_t* info, void* context) {
    std::size_t slot = 0;
    while (slot < std::size(CopySignals) && CopySignals[slot] != signal) ++slot;
    if (slot == std::size(CopySignals)) return;
    const int active = g_active.load(std::memory_order_acquire);
    const struct sigaction previous = g_previous[active][slot];
    if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
        if ((previous.sa_flags & SA_RESETHAND) != 0) {
            g_previous[active][slot] = {};
            g_previous[active][slot].sa_handler = SIG_DFL;
        }
        if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
            previous.sa_sigaction(signal, info, context);
            return;
        }
        previous.sa_handler(signal);
        return;
    }
    if (info != nullptr && SentByProcess(info) && previous.sa_handler == SIG_IGN) return;
    struct sigaction fallback{};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    sigaction(signal, &fallback, nullptr);
    if (info != nullptr && SentByProcess(info)) raise(signal);
}

void OnCopyFault(int signal, siginfo_t* info, void* context) {
    if (GuestFaultCopy::Resume(info, context)) return;
    Chain(signal, info, context);
}

bool InstallOwned(unsigned slots) {
    const int next = 1 - g_active.load(std::memory_order_relaxed);
    struct sigaction current[std::size(CopySignals)]{};
    bool ours = true;
    for (std::size_t slot = 0; slot < std::size(CopySignals); ++slot) {
        if ((slots & (1u << slot)) == 0) continue;
        if (sigaction(CopySignals[slot], nullptr, &current[slot]) != 0) return false;
        ours = ours && (current[slot].sa_flags & SA_SIGINFO) != 0 && current[slot].sa_sigaction == OnCopyFault;
    }
    if (ours) return true;
    for (std::size_t slot = 0; slot < std::size(CopySignals); ++slot) {
        const bool installed = (slots & (1u << slot)) == 0 || ((current[slot].sa_flags & SA_SIGINFO) != 0 && current[slot].sa_sigaction == OnCopyFault);
        g_previous[next][slot] = installed ? g_previous[1 - next][slot] : current[slot];
    }
    g_active.store(next, std::memory_order_release);
    for (std::size_t slot = 0; slot < std::size(CopySignals); ++slot) {
        if ((slots & (1u << slot)) == 0) continue;
        struct sigaction action{};
        action.sa_sigaction = OnCopyFault;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER | (g_previous[next][slot].sa_flags & SA_RESTART);
        action.sa_mask = g_previous[next][slot].sa_mask;
        if (sigaction(CopySignals[slot], &action, nullptr) != 0) return false;
    }
    return true;
}

bool Install(unsigned slots) {
#if defined(__FreeBSD__)
    const auto self = static_cast<pid_t>(pthread_getthreadid_np());
#else
    const auto self = static_cast<pid_t>(syscall(SYS_gettid));
#endif
    for (pid_t expected = 0; !g_installer.compare_exchange_weak(expected, self, std::memory_order_acquire); expected = 0) {
        if (expected == self) {
            g_pending.fetch_or(slots, std::memory_order_release);
            return true;
        }
    }
    bool installed = false;
    do {
        installed = InstallOwned(slots);
        slots = g_pending.exchange(0, std::memory_order_acquire);
    } while (installed && slots != 0);
    g_installer.store(0, std::memory_order_release);
    return installed;
}

bool Enabled() {
    return std::getenv("APS5_GUEST_COPY_SYSCALL") == nullptr;
}

}

namespace GuestFaultCopy {

bool Ready() {
    static const bool ready = [] {
        const bool installed = Enabled() && Install(AllSlots);
        g_ready.store(installed, std::memory_order_release);
        return installed;
    }();
    return ready;
}

void Reinstall() {
    if (g_ready.load(std::memory_order_acquire)) static_cast<void>(Install(SegvSlot));
}

bool Copy(void* destination, const void* source, std::size_t bytes) {
    return GuestFaultCopyBytes(destination, source, bytes) == 0;
}

bool ProbeWrite(void* address) {
    return GuestFaultCopyProbeWrite(address) == 0;
}

bool Resume(const siginfo_t* info, void* context) {
    if (info == nullptr || context == nullptr || SentByProcess(info)) return false;
    if (info->si_signo != SIGSEGV && info->si_signo != SIGBUS) return false;
#if defined(__FreeBSD__)
    auto& rip = static_cast<ucontext_t*>(context)->uc_mcontext.mc_rip;
    if (rip == reinterpret_cast<__register_t>(GuestFaultCopyProbeAccess)) {
        // A page the write watch protects (or a read trap) is writable for the guest: once the watch has let go of
        // it the store is retried.
        if (!GuestWriteWatch::GuestWriteWatchHandleFault_nid_postfix(reinterpret_cast<std::uintptr_t>(info->si_addr)))
            rip = reinterpret_cast<__register_t>(GuestFaultCopyProbeResume);
        return true;
    }
    if (rip != reinterpret_cast<__register_t>(GuestFaultCopyAccess)) return false;
    rip = reinterpret_cast<__register_t>(GuestFaultCopyResume);
#else
    auto& registers = static_cast<ucontext_t*>(context)->uc_mcontext.gregs;
    if (registers[REG_RIP] != reinterpret_cast<greg_t>(GuestFaultCopyAccess)) return false;
    registers[REG_RIP] = reinterpret_cast<greg_t>(GuestFaultCopyResume);
#endif
    return true;
}

}
#endif
