#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_ITANIUM_CXXABI_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_ITANIUM_CXXABI_HPP

// AnyPS5 FreeBSD libcxxrt adapter v1.3
#if defined(__FreeBSD__)
// FreeBSD: libcxxrt's <cxxabi.h> includes the compiler <unwind.h>, which clashes with the guest
// unwind ABI of UnwindAbi.hpp. Declare only what this library uses from the host C++ ABI.
#include <cstddef>
namespace __cxxabiv1 {
extern "C" char* __cxa_demangle(const char* mangled_name, char* buf, std::size_t* n, int* status);
extern "C" int __cxa_thread_atexit(void (*)(void*), void*, void*) noexcept;
}
namespace abi = __cxxabiv1;
#else
#include <cxxabi.h>
#endif

#ifdef __EMSCRIPTEN__
namespace __cxxabiv1 {
extern "C" int __cxa_thread_atexit(void (*)(void*), void*, void*) noexcept;
}
#endif

// libcxxrt (FreeBSD) keeps these type_info classes private, like libc++abi.
#if defined(_LIBCPPABI_VERSION) || defined(__FreeBSD__)
#include <typeinfo>

namespace __cxxabiv1 {
class __class_type_info : public std::type_info {
public:
    enum __sub_kind { __unknown = 0 };
    struct __upcast_result;
    struct __dyncast_result;
};

class __si_class_type_info : public __class_type_info {
public:
    const __class_type_info* __base_type;
};

struct __base_class_type_info {
    const __class_type_info* __base_type;
    long __offset_flags;
};

class __vmi_class_type_info : public __class_type_info {
public:
    unsigned int __flags;
    unsigned int __base_count;
    __base_class_type_info __base_info[1];
};

class __pbase_type_info : public std::type_info {
public:
    unsigned int __flags;
    const std::type_info* __pointee;
};

class __pointer_type_info : public __pbase_type_info {};

class __function_type_info : public std::type_info {};

class __fundamental_type_info : public std::type_info {};

class __pointer_to_member_type_info : public __pbase_type_info {
public:
    const __class_type_info* __context;
};

#ifdef __APPLE__
extern "C" int __cxa_thread_atexit(void (*)(void*), void*, void*) noexcept;
#endif
}
#endif

#endif
