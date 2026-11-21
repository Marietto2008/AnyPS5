#include "prx/libc/include/exceptions/Unwind.hpp"
#include "prx/libc/include/exceptions/Runtime.hpp"
#if defined(__FreeBSD__)
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>
#endif

extern "C" _Unwind_Reason_Code APS5_VABI __gxx_personality_v0_nid_postfix(
    int version, _Unwind_Action actions, std::uint64_t exceptionClass,
    _Unwind_Exception* exception, _Unwind_Context* context
) {
    using namespace LibcUnwind;
    using namespace LibcException;
    if (version != 1) return _URC_FATAL_PHASE1_ERROR;
    if (!context || !context->lsda) return _URC_CONTINUE_UNWIND;
    const Byte* p = reinterpret_cast<const Byte*>(context->lsda);
    Byte landingEncoding = *p++;
    Word landingBase = landingEncoding == 255 ? context->region : Encoded(p, landingEncoding, context->dataBase, context->region, context->textBase);
    Byte typeEncoding = *p++;
    const Byte* typeTable = nullptr;
    if (typeEncoding != 255) { Word offset = Uleb(p); typeTable = p + offset; }
    Byte callEncoding = *p++;
    Word length = Uleb(p);
    const Byte* callEnd = p + length;
    const Byte* actionsBegin = callEnd;
    Word ip = context->registers[16] - !context->signalFrame;
    Header* header = Native(exceptionClass) ? FromUnwind(exception) : nullptr;
    while (p < callEnd) {
        Word start = Encoded(p, callEncoding);
        Word size = Encoded(p, callEncoding);
        Word landing = Encoded(p, callEncoding);
        Word action = Uleb(p);
        if (ip < context->region + start || ip - context->region - start >= size) continue;
        if (!landing) return _URC_CONTINUE_UNWIND;
        bool cleanup = !action, matched = false;
        std::intptr_t selector = 0;
        void* adjusted = header ? static_cast<void*>(Primary(header) + 1) : nullptr;
        if (header && std::strcmp(Kind(Primary(header)->type), "N10__cxxabiv119__pointer_type_infoE") == 0)
            std::memcpy(&adjusted, adjusted, sizeof(adjusted));
        if (action) {
            const Byte* record = actionsBegin + action - 1;
            for (unsigned steps = 0; steps < 4096; ++steps) {
                auto filter = Sleb(record);
                const Byte* nextField = record;
                auto next = Sleb(record);
                if (filter == 0) cleanup = true;
                else if (filter > 0 && !(actions & _UA_FORCE_UNWIND) && typeTable) {
                    const Byte* entry = typeTable - filter * EncodingSize(typeEncoding);
                    auto* caught = reinterpret_cast<const std::type_info*>(Encoded(entry, typeEncoding, context->dataBase, context->region, context->textBase));
                    void* candidate = adjusted;
                    if (!caught || (header && Match(caught, Primary(header)->type, candidate))) {
                        matched = true; selector = filter; adjusted = candidate; break;
                    }
                } else if (filter < 0) return (actions & _UA_SEARCH_PHASE) ? _URC_FATAL_PHASE1_ERROR : _URC_FATAL_PHASE2_ERROR;
                if (!next) break;
                record = nextField + next;
            }
        }
        if (actions & _UA_SEARCH_PHASE) {
            if (!matched) return _URC_CONTINUE_UNWIND;
            if (header) { header->adjusted = adjusted; header->selector = static_cast<int>(selector); }
            return _URC_HANDLER_FOUND;
        }
        if (actions & _UA_HANDLER_FRAME) {
            if (!matched) return _URC_FATAL_PHASE2_ERROR;
            if (header) header->adjusted = adjusted;
        } else {
            if (!cleanup) return _URC_CONTINUE_UNWIND;
            selector = 0;
        }
        context->registers[0] = reinterpret_cast<Word>(exception);
        context->registers[1] = selector;
        context->registers[16] = landingBase + landing;
        return _URC_INSTALL_CONTEXT;
    }
    return _URC_CONTINUE_UNWIND;
}

extern "C" _Unwind_Reason_Code APS5_VABI __gcc_personality_v0_nid_postfix(
    int version, _Unwind_Action actions, std::uint64_t,
    _Unwind_Exception* exception, _Unwind_Context* context
) {
    using namespace LibcUnwind;
    if (version != 1) return _URC_FATAL_PHASE1_ERROR;
    if (!context || !(actions & _UA_CLEANUP_PHASE) || !context->lsda) return _URC_CONTINUE_UNWIND;
    const Byte* p = reinterpret_cast<const Byte*>(context->lsda);
    Byte landingEncoding = *p++;
    Word landingBase = landingEncoding == 255 ? context->region : Encoded(p, landingEncoding, context->dataBase, context->region, context->textBase);
    if (*p++ != 255) Uleb(p);
    Byte callEncoding = *p++;
    Word length = Uleb(p);
    const Byte* callEnd = p + length;
    Word ip = context->registers[16] - !context->signalFrame;
    while (p < callEnd) {
        Word start = Encoded(p, callEncoding);
        Word size = Encoded(p, callEncoding);
        Word landing = Encoded(p, callEncoding);
        Uleb(p);
        if (ip < context->region + start || ip - context->region - start >= size) continue;
        if (!landing) return _URC_CONTINUE_UNWIND;
        context->registers[0] = reinterpret_cast<Word>(exception);
        context->registers[1] = 0;
        context->registers[16] = landingBase + landing;
        return _URC_INSTALL_CONTEXT;
    }
    return _URC_CONTINUE_UNWIND;
}

#ifndef __APPLE__
#if defined(__FreeBSD__)
namespace {

using HostPersonality = _Unwind_Reason_Code (*)(int, _Unwind_Action, std::uint64_t, _Unwind_Exception*, _Unwind_Context*);

// libthr's pthread_exit unwinds with the host's own unwinder (it opens libgcc_s itself), whose contexts are not this
// runtime's: those frames go to the host personality the unwinder belongs with.
bool ForeignContext(const _Unwind_Context* context) {
    return context != nullptr && context->cursor != UnwindCursorSlots_nid_no_patch();
}

HostPersonality HostPersonalityIn(const char* library, const char* name) {
    void* const handle = dlopen(library, RTLD_LAZY | RTLD_NOLOAD);
    void* const symbol = handle != nullptr ? dlsym(handle, name) : nullptr;
    if (symbol == nullptr) {
        static constexpr char message[] = "[libc] a host unwinder reached the guest personality and the host one is not loaded\n";
        static_cast<void>(!write(STDERR_FILENO, message, sizeof(message) - 1));
        std::abort();
    }
    return reinterpret_cast<HostPersonality>(symbol);
}

}
#endif

extern "C" _Unwind_Reason_Code __gxx_personality_v0(
    int version, _Unwind_Action actions, std::uint64_t exceptionClass,
    _Unwind_Exception* exception, _Unwind_Context* context
) {
#if defined(__FreeBSD__)
    if (ForeignContext(context)) {
        static const HostPersonality host = HostPersonalityIn("libcxxrt.so.1", "__gxx_personality_v0");
        return host(version, actions, exceptionClass, exception, context);
    }
#endif
    return __gxx_personality_v0_nid_postfix(version, actions, exceptionClass, exception, context);
}

extern "C" _Unwind_Reason_Code __gcc_personality_v0(
    int version, _Unwind_Action actions, std::uint64_t exceptionClass,
    _Unwind_Exception* exception, _Unwind_Context* context
) {
#if defined(__FreeBSD__)
    if (ForeignContext(context)) {
        static const HostPersonality host = HostPersonalityIn("libgcc_s.so.1", "__gcc_personality_v0");
        return host(version, actions, exceptionClass, exception, context);
    }
#endif
    return __gcc_personality_v0_nid_postfix(version, actions, exceptionClass, exception, context);
}
#endif
