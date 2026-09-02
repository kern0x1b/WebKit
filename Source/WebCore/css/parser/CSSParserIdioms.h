/*
 * Copyright (C) 2013 Google Inc. All rights reserved.
 * Copyright (C) 2016 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <WebCore/CSSParserContext.h>
#include <WebCore/CSSValueKeywords.h>
#include <WebCore/CSSWideKeyword.h>
#include <array>
#include <type_traits>
#include <wtf/ASCIICType.h>

namespace WebCore {

// One table lookup replaces the five-to-seven compare-and-branch chain these predicates used to
// expand to. Entries 0x80-0xFF carry the non-ASCII classification so the Latin-1 path needs no
// range test at all.
enum : uint8_t {
    CSSCharacterClassNameStart = 1 << 0,
    CSSCharacterClassName = 1 << 1,
    CSSCharacterClassNewline = 1 << 2,
    CSSCharacterClassWhitespace = 1 << 3,
    CSSCharacterClassDigit = 1 << 4,
    CSSCharacterClassSpace = 1 << 5,
    CSSCharacterClassTabOrSpace = 1 << 6,
    CSSCharacterClassNonASCII = CSSCharacterClassNameStart | CSSCharacterClassName,
};

inline constexpr std::array<uint8_t, 256> cssCharacterClassTable = [] {
    std::array<uint8_t, 256> table { };
    for (unsigned i = 0; i < 128; ++i) {
        uint8_t flags = 0;
        if ((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') || i == '_')
            flags |= CSSCharacterClassNameStart | CSSCharacterClassName;
        if (i >= '0' && i <= '9')
            flags |= CSSCharacterClassName | CSSCharacterClassDigit;
        if (i == '-')
            flags |= CSSCharacterClassName;
        if (i == '\n' || i == '\r' || i == '\f')
            flags |= CSSCharacterClassNewline | CSSCharacterClassWhitespace;
        if (i == ' ' || i == '\t')
            flags |= CSSCharacterClassWhitespace | CSSCharacterClassTabOrSpace;
        if (i == ' ' || i == '\t' || i == '\n')
            flags |= CSSCharacterClassSpace;
        table[i] = flags;
    }
    for (unsigned i = 128; i < 256; ++i)
        table[i] = CSSCharacterClassNonASCII;
    return table;
}();

template<typename CharacterType>
inline uint8_t cssCharacterClass(CharacterType c)
{
    if constexpr (sizeof(CharacterType) == 1)
        return cssCharacterClassTable[static_cast<uint8_t>(c)];
    else {
        auto value = static_cast<std::make_unsigned_t<CharacterType>>(c);
        return value < 256 ? cssCharacterClassTable[static_cast<size_t>(value)] : static_cast<uint8_t>(CSSCharacterClassNonASCII);
    }
}

// Space characters as defined by the CSS specification.
// http://www.w3.org/TR/css3-syntax/#whitespace

template<typename CharacterType>
inline bool isCSSSpace(CharacterType c)
{
    return cssCharacterClass(c) & CSSCharacterClassSpace;
}

// https://drafts.csswg.org/css-syntax-3/#newline
template<typename CharacterType>
inline bool isCSSNewline(CharacterType c)
{
    return cssCharacterClass(c) & CSSCharacterClassNewline;
}

// http://dev.w3.org/csswg/css-syntax/#name-start-code-point
template <typename CharacterType>
bool isNameStartCodePoint(CharacterType c)
{
    return cssCharacterClass(c) & CSSCharacterClassNameStart;
}

// http://dev.w3.org/csswg/css-syntax/#name-code-point
template <typename CharacterType>
bool isNameCodePoint(CharacterType c)
{
    return cssCharacterClass(c) & CSSCharacterClassName;
}

inline bool isValidCustomIdentifier(CSSValueID valueID)
{
    // "default" is obsolete as a CSS-wide keyword but is still not allowed as a custom identifier.
    return !isCSSWideKeyword(valueID) && valueID != CSSValueDefault;
}

// Unlike CSSPropertyParserHelpers::genericFontFamily, this does not access
// NeverDestroyed objects that require main-thread access, making it
// safe to call from OffscreenCanvas workers.
inline bool isGenericFontFamilyKeyword(CSSValueID valueID)
{
    switch (valueID) {
    case CSSValueSerif:
    case CSSValueSansSerif:
    case CSSValueCursive:
    case CSSValueFantasy:
    case CSSValueMonospace:
    case CSSValueSystemUi:
    case CSSValueWebkitPictograph:
    case CSSValueMath:
        return true;
    default:
        return false;
    }
}

// https://drafts.csswg.org/css-conditional-5/#propdef-container-name
inline bool isValidContainerNameIdentifier(CSSValueID valueID)
{
    switch (valueID) {
    case CSSValueNone:
    case CSSValueAnd:
    case CSSValueOr:
    case CSSValueNot:
        return false;
    default:
        return isValidCustomIdentifier(valueID);
    }
}

} // namespace WebCore
