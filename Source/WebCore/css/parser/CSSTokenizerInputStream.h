// Copyright 2014 The Chromium Authors. All rights reserved.
// Copyright (C) 2016 Apple Inc. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//    * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//    * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//    * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#pragma once

#include <wtf/text/StringView.h>

namespace WebCore {

constexpr Latin1Character kEndOfFileMarker = 0;

DECLARE_ALLOCATOR_WITH_HEAP_IDENTIFIER(CSSTokenizerInputStream);
class CSSTokenizerInputStream {
    WTF_MAKE_NONCOPYABLE(CSSTokenizerInputStream);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED_WITH_HEAP_IDENTIFIER(CSSTokenizerInputStream, CSSTokenizerInputStream);
public:
    explicit CSSTokenizerInputStream(const String& input);

    // Gets the char in the stream. Will return (NUL) kEndOfFileMarker when at the
    // end of the stream.
    char16_t nextInputChar() const
    {
        if (m_offset >= m_stringLength) [[unlikely]]
            return kEndOfFileMarker;
        return characterAt(m_offset);
    }

    // Gets the char at lookaheadOffset from the current stream position. Will
    // return NUL (kEndOfFileMarker) if the stream position is at the end.
    char16_t peek(unsigned lookaheadOffset) const
    {
        size_t index = m_offset + lookaheadOffset;
        if (index >= m_stringLength) [[unlikely]]
            return kEndOfFileMarker;
        return characterAt(index);
    }

    void advance(unsigned offset = 1) { m_offset += offset; }
    void pushBack(char16_t cc)
    {
        --m_offset;
        ASSERT_UNUSED(cc, nextInputChar() == cc);
    }

    double getDouble(unsigned start, unsigned end) const;

    // Advances offset past every character satisfying the predicate and returns the new offset.
    // The end bound and the buffer pointer are hoisted out of the loop, and the predicate is
    // instantiated on the concrete character type, so the Latin-1 path is one indexed byte load
    // per character with no is8Bit test and no widening to char16_t.
    template<typename Predicate>
    unsigned skipWhile(unsigned offset, Predicate predicate) const
    {
        size_t start = m_offset + offset;
        if (start >= m_stringLength) [[unlikely]]
            return offset;
        size_t index = start;
        size_t length = m_stringLength;
        if (m_is8Bit) {
            const Latin1Character* characters = m_characters8;
            while (index < length && predicate(characters[index]))
                ++index;
        } else {
            const char16_t* characters = m_characters16;
            while (index < length && predicate(characters[index]))
                ++index;
        }
        return offset + static_cast<unsigned>(index - start);
    }

    // Number of characters from the current position for which the predicate holds.
    template<typename Predicate>
    unsigned countWhile(Predicate predicate) const
    {
        return skipWhile(0, predicate);
    }

    void advanceUntilNonWhitespace();
    void advanceUntilNewlineOrNonWhitespace();

    unsigned length() const { return m_stringLength; }
    unsigned offset() const { return std::min(m_offset, m_stringLength); }

    StringView rangeAt(unsigned start, unsigned length) const
    {
        ASSERT(start + length <= m_stringLength);
        if (m_is8Bit)
            return StringView(static_cast<const void*>(m_characters8 + start), length, true);
        return StringView(static_cast<const void*>(m_characters16 + start), length, false);
    }

private:
    char16_t characterAt(size_t index) const
    {
        return m_is8Bit ? m_characters8[index] : m_characters16[index];
    }

    size_t m_offset;
    const size_t m_stringLength;
    // The buffer pointer is hoisted out of the StringImpl once so that every character
    // access is a single indexed load instead of re-testing the is8Bit flag.
    const Latin1Character* m_characters8 { nullptr };
    const char16_t* m_characters16 { nullptr };
    bool m_is8Bit { false };
    RefPtr<StringImpl> m_string;
};

} // namespace WebCore
