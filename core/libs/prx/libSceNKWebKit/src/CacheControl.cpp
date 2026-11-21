/*
 * Copyright (C) 2014-2017 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "prx/libSceNKWebKit/include/CacheControlExports.hpp"
#include "prx/libSceNKWebKit/include/HttpHeaderMapExports.hpp"
#include "prx/libSceNKWebKit/include/InventoryExports.hpp"
#include "prx/libSceNKWebKit/include/ResourceResponseExports.hpp"
#include "prx/libSceNKWebKit/include/WtfExports.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <ios>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using NKWebKit::HttpAbi::CacheControlDirectives;
using NKWebKit::WtfAbi::String;

// Latin-1 strings are sequences of unsigned char. libc++ 19 removed the generic std::char_traits template (only
// char, wchar_t, char8_t, char16_t and char32_t remain), so they get explicit traits; char16_t keeps the standard ones.
struct LatinTraits {
    using char_type = unsigned char;
    using int_type = int;
    using off_type = std::streamoff;
    using pos_type = std::streampos;
    using state_type = std::mbstate_t;
    static constexpr void assign(char_type& to, const char_type& from) noexcept { to = from; }
    static constexpr bool eq(char_type a, char_type b) noexcept { return a == b; }
    static constexpr bool lt(char_type a, char_type b) noexcept { return a < b; }
    static constexpr int compare(const char_type* a, const char_type* b, std::size_t count) noexcept {
        for (std::size_t i = 0; i < count; ++i)
            if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
        return 0;
    }
    static constexpr std::size_t length(const char_type* text) noexcept {
        std::size_t count = 0;
        while (text[count] != 0) ++count;
        return count;
    }
    static constexpr const char_type* find(const char_type* text, std::size_t count, const char_type& value) noexcept {
        for (std::size_t i = 0; i < count; ++i)
            if (text[i] == value) return text + i;
        return nullptr;
    }
    static char_type* move(char_type* to, const char_type* from, std::size_t count) noexcept {
        return count == 0 ? to : static_cast<char_type*>(std::memmove(to, from, count));
    }
    static char_type* copy(char_type* to, const char_type* from, std::size_t count) noexcept {
        return count == 0 ? to : static_cast<char_type*>(std::memcpy(to, from, count));
    }
    static char_type* assign(char_type* to, std::size_t count, char_type value) noexcept {
        return count == 0 ? to : static_cast<char_type*>(std::memset(to, value, count));
    }
    static constexpr int_type not_eof(int_type value) noexcept { return value == eof() ? 0 : value; }
    static constexpr char_type to_char_type(int_type value) noexcept { return static_cast<char_type>(value); }
    static constexpr int_type to_int_type(char_type value) noexcept { return value; }
    static constexpr bool eq_int_type(int_type a, int_type b) noexcept { return a == b; }
    static constexpr int_type eof() noexcept { return -1; }
};

template<class TCharacter> struct CharacterTraits { using Type = std::char_traits<TCharacter>; };
template<> struct CharacterTraits<unsigned char> { using Type = LatinTraits; };
template<class TCharacter> using HostString = std::basic_string<TCharacter, typename CharacterTraits<TCharacter>::Type>;
template<class TCharacter> using HostStringView = std::basic_string_view<TCharacter, typename CharacterTraits<TCharacter>::Type>;

template<class TCharacter>
bool IsSeparator(TCharacter c) {
    return std::u16string_view(u"()<>@,;:\\\"/[]?={} \t").find(c) != std::u16string_view::npos;
}

template<class TCharacter>
HostStringView<TCharacter> Trim(HostStringView<TCharacter> value) {
    std::size_t end = 0;
    while (end < value.size() && !IsSeparator(value[end])) ++end;
    return value.substr(0, end);
}

template<class TCharacter>
bool Equal(HostStringView<TCharacter> value, std::string_view literal) {
    if (value.size() != literal.size()) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        auto c = value[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != literal[i]) return false;
    }
    return true;
}

template<class TCharacter>
std::vector<std::pair<HostStringView<TCharacter>, HostStringView<TCharacter>>> Parse(HostStringView<TCharacter> header) {
    std::vector<std::pair<HostStringView<TCharacter>, HostStringView<TCharacter>>> result;
    const auto missing = header.npos;
    std::size_t pos = 0;
    while (pos < header.size()) {
        auto comma = header.find(',', pos);
        auto equal = header.find('=', pos);
        if (equal == missing && comma == missing) {
            result.emplace_back(Trim(header.substr(pos)), header.substr(0, 0));
            return result;
        }
        if (comma != missing && (comma < equal || equal == missing)) {
            result.emplace_back(Trim(header.substr(pos, comma - pos)), header.substr(0, 0));
            pos = comma + 1;
            continue;
        }
        auto directive = Trim(header.substr(pos, equal - pos));
        pos = equal + 1;
        auto value = header.substr(pos);
        if (value.starts_with(static_cast<TCharacter>('"'))) {
            auto quote = value.find('"', 1);
            if (quote == missing) {
                result.emplace_back(directive, Trim(value.substr(1)));
                return result;
            }
            result.emplace_back(directive, value.substr(1, quote - 1));
            pos += quote + 1;
            comma = header.find(',', pos);
            if (comma == missing) return result;
            pos = comma + 1;
            continue;
        }
        comma = value.find(',');
        if (comma == missing) {
            result.emplace_back(directive, Trim(value));
            return result;
        }
        result.emplace_back(directive, Trim(value.substr(0, comma)));
        pos += comma + 1;
    }
    return result;
}

template<class TCharacter>
void Apply(const String& input, CacheControlDirectives& result) {
    HostString<TCharacter> safe;
    auto* characters = static_cast<const TCharacter*>(input.impl->data);
    for (unsigned i = 0; i < input.impl->length; ++i)
        if (characters[i] > ' ' && characters[i] != 127) safe.push_back(characters[i]);
    for (auto [name, value] : Parse(HostStringView<TCharacter>(safe))) {
        if (Equal(name, "no-cache") && value.empty()) result.noCache = true;
        else if (Equal(name, "no-store")) result.noStore = true;
        else if (Equal(name, "must-revalidate")) result.mustRevalidate = true;
        else if (Equal(name, "immutable")) result.immutable = true;
        else {
            double* destination = nullptr;
            if (Equal(name, "max-age")) destination = &result.maxAge;
            else if (Equal(name, "max-stale")) destination = &result.maxStale;
            else if (Equal(name, "stale-while-revalidate")) destination = &result.staleWhileRevalidate;
            if (!destination || !std::isnan(*destination)) continue;
            if (Equal(name, "max-stale") && value.empty()) {
                *destination = std::numeric_limits<double>::infinity();
                continue;
            }
            bool ok;
            double number;
            if constexpr (sizeof(TCharacter) == 1)
                number = _ZN3WTF18charactersToDoubleEPKhmPb(value.data(), value.size(), &ok);
            else
                number = _ZN3WTF18charactersToDoubleEPKDsmPb(value.data(), value.size(), &ok);
            if (ok) *destination = number;
        }
    }
}

template<class TCharacter>
bool PragmaNoCache(const String& input) {
    HostStringView<TCharacter> value(static_cast<const TCharacter*>(input.impl->data), input.impl->length);
    for (std::size_t i = 0; i + 8 <= value.size(); ++i)
        if (Equal(value.substr(i, 8), "no-cache")) return true;
    return false;
}

}

extern "C" NKWebKit::HttpAbi::CacheControlDirectives APS5_VABI _ZN7WebCore27parseCacheControlDirectivesERKNS_13HTTPHeaderMapE(const NKWebKit::HttpAbi::HTTPHeaderMap& headers) {
    CacheControlDirectives result;
    auto control = _ZNK7WebCore13HTTPHeaderMap3getENS_14HTTPHeaderNameE(&headers, NKWebKit::HttpAbi::HTTPHeaderName::CacheControl);
    if (control.impl && control.impl->length) {
        if (control.impl->hashAndFlags & 4) Apply<unsigned char>(control, result);
        else Apply<char16_t>(control, result);
    }
    if (!result.noCache) {
        auto pragma = _ZNK7WebCore13HTTPHeaderMap3getENS_14HTTPHeaderNameE(&headers, NKWebKit::HttpAbi::HTTPHeaderName::Pragma);
        if (pragma.impl && pragma.impl->length)
            result.noCache = pragma.impl->hashAndFlags & 4 ? PragmaNoCache<unsigned char>(pragma) : PragmaNoCache<char16_t>(pragma);
    }
    return result;
}

extern "C" WTF::Seconds APS5_VABI _ZN7WebCore17computeCurrentAgeERKNS_16ResourceResponseEN3WTF8WallTimeE(const NKWebKit::WebCoreAbi::ResourceResponseBase& response, WTF::WallTime responseTime) {
    const auto dateValue = _ZNK7WebCore20ResourceResponseBase4dateEv(&response);
    const double apparentAge = dateValue.hasValue ? std::max(0.0, responseTime.m_value - dateValue.seconds) : 0.0;
    const auto ageValue = _ZNK7WebCore20ResourceResponseBase3ageEv(&response);
    const double correctedInitialAge = std::max(apparentAge, ageValue.hasValue ? ageValue.seconds : 0.0);
    const double residentTime = _ZN3WTF8WallTime3nowEv().m_value - responseTime.m_value;
    return WTF::Seconds{correctedInitialAge + residentTime};
}
