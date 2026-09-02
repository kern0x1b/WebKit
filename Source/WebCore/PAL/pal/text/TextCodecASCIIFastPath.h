/*
 * Copyright (C) 2011 Apple Inc. All rights reserved.
 * Copyright (C) 2011 Nokia Corporation and/or its subsidiary(-ies).
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/StdLibExtras.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/ASCIIFastPath.h>

#if HAVE(ARM_NEON_INTRINSICS)
#include <arm_neon.h>
#endif

namespace PAL {

template<size_t size> struct ASCIIFastPathByteFiller;
template<> struct ASCIIFastPathByteFiller<4> {
    static void copy(std::span<Latin1Character> destination, std::span<const uint8_t> source)
    {
        memcpySpan(destination, source.first(4));
    }
    
    static void copy(std::span<char16_t> destination, std::span<const uint8_t> source)
    {
        destination[0] = source[0];
        destination[1] = source[1];
        destination[2] = source[2];
        destination[3] = source[3];
    }
};
template<> struct ASCIIFastPathByteFiller<8> {
    static void copy(std::span<Latin1Character> destination, std::span<const uint8_t> source)
    {
        memcpySpan(destination, source.first(8));
    }

    static void copy(std::span<char16_t> destination, std::span<const uint8_t> source)
    {
        destination[0] = source[0];
        destination[1] = source[1];
        destination[2] = source[2];
        destination[3] = source[3];
        destination[4] = source[4];
        destination[5] = source[5];
        destination[6] = source[6];
        destination[7] = source[7];
    }
};

inline void copyASCIIMachineWord(std::span<Latin1Character> destination, std::span<const uint8_t> source)
{
    ASCIIFastPathByteFiller<sizeof(WTF::MachineWord)>::copy(destination, source);
}

inline void copyASCIIMachineWord(std::span<char16_t> destination, std::span<const uint8_t> source)
{
    ASCIIFastPathByteFiller<sizeof(WTF::MachineWord)>::copy(destination, source);
}

#if HAVE(ARM_NEON_INTRINSICS)

ALWAYS_INLINE bool vectorContainsOnlyASCII(uint8x16_t value)
{
    uint8x8_t folded = vorr_u8(vget_low_u8(value), vget_high_u8(value));
    return !(vget_lane_u64(vreinterpret_u64_u8(folded), 0) & 0x8080808080808080ULL);
}

ALWAYS_INLINE void storeASCIIVector(Latin1Character* destination, uint8x16_t value)
{
    vst1q_u8(destination, value);
}

ALWAYS_INLINE void storeASCIIVector(char16_t* destination, uint8x16_t value)
{
    auto* output = reinterpret_cast<uint16_t*>(destination);
    vst1q_u16(output, vmovl_u8(vget_low_u8(value)));
    vst1q_u16(output + 8, vmovl_u8(vget_high_u8(value)));
}

#elif CPU(ARM_THUMB2) && CPU(LITTLE_ENDIAN)

ALWAYS_INLINE void storeASCIIWord(Latin1Character* destination, uint32_t word)
{
    WTF::unalignedStore<uint32_t>(destination, word);
}

ALWAYS_INLINE void storeASCIIWord(char16_t* destination, uint32_t word)
{
    uint32_t low = word & 0x0000FFFFU;
    uint32_t high = word >> 16;
    WTF::unalignedStore<uint32_t>(destination, (low | (low << 8)) & 0x00FF00FFU);
    WTF::unalignedStore<uint32_t>(destination + 2, (high | (high << 8)) & 0x00FF00FFU);
}

#endif

// Copies the longest prefix of `source` that contains only bytes below 0x80 into `destination`,
// widening to 16 bits when that is what the destination holds, and returns the length of that
// prefix in bytes. The first non-ASCII byte and everything after it is left untouched, so the
// caller resumes its slow path on exactly the byte the scan stopped at and never re-reads a byte
// this function already accepted.
template<typename CharacterType>
ALWAYS_INLINE size_t copyLeadingASCII(std::span<CharacterType> destination, std::span<const uint8_t> source)
{
    static_assert(sizeof(CharacterType) == 1 || sizeof(CharacterType) == 2);
    ASSERT(destination.size() >= source.size());

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    const uint8_t* input = source.data();
    CharacterType* output = destination.data();
    const size_t size = source.size();
    size_t index = 0;

#if HAVE(ARM_NEON_INTRINSICS)
    // Four vectors are tested with a single fold and a single transfer out of the vector unit,
    // which is the expensive part of the test on this core, and the widening store below is one
    // instruction per half vector instead of eight scalar stores.
    while (size - index >= 64) {
        uint8x16_t first = vld1q_u8(input + index);
        uint8x16_t second = vld1q_u8(input + index + 16);
        uint8x16_t third = vld1q_u8(input + index + 32);
        uint8x16_t fourth = vld1q_u8(input + index + 48);
        if (!vectorContainsOnlyASCII(vorrq_u8(vorrq_u8(first, second), vorrq_u8(third, fourth))))
            break;
        storeASCIIVector(output + index, first);
        storeASCIIVector(output + index + 16, second);
        storeASCIIVector(output + index + 32, third);
        storeASCIIVector(output + index + 48, fourth);
        index += 64;
    }
    while (size - index >= 16) {
        uint8x16_t value = vld1q_u8(input + index);
        if (!vectorContainsOnlyASCII(value))
            break;
        storeASCIIVector(output + index, value);
        index += 16;
    }
#elif CPU(ARM_THUMB2) && CPU(LITTLE_ENDIAN)
    // No alignment prologue: this core takes word loads and stores at any address, so a run that
    // starts just after a multi-byte sequence goes straight into the word loop instead of
    // stepping bytes until the pointer happens to be aligned again.
    while (size - index >= 16) {
        uint32_t first = WTF::unalignedLoad<uint32_t>(input + index);
        uint32_t second = WTF::unalignedLoad<uint32_t>(input + index + 4);
        uint32_t third = WTF::unalignedLoad<uint32_t>(input + index + 8);
        uint32_t fourth = WTF::unalignedLoad<uint32_t>(input + index + 12);
        if ((first | second | third | fourth) & 0x80808080U)
            break;
        storeASCIIWord(output + index, first);
        storeASCIIWord(output + index + 4, second);
        storeASCIIWord(output + index + 8, third);
        storeASCIIWord(output + index + 12, fourth);
        index += 16;
    }
    while (size - index >= 4) {
        uint32_t word = WTF::unalignedLoad<uint32_t>(input + index);
        if (word & 0x80808080U)
            break;
        storeASCIIWord(output + index, word);
        index += 4;
    }
#else
    // Portable: reach a machine-word boundary with a byte loop, then load whole words. Nothing
    // here assumes an unaligned load is legal.
    while (index < size && !WTF::isAlignedToMachineWord(input + index)) {
        if (!isASCII(input[index]))
            return index;
        output[index] = static_cast<CharacterType>(input[index]);
        ++index;
    }
    while (size - index >= sizeof(WTF::MachineWord)) {
        if (!WTF::containsOnlyASCII<Latin1Character>(*reinterpret_cast<const WTF::MachineWord*>(input + index)))
            break;
        for (size_t offset = 0; offset < sizeof(WTF::MachineWord); ++offset)
            output[index + offset] = static_cast<CharacterType>(input[index + offset]);
        index += sizeof(WTF::MachineWord);
    }
#endif

    while (index < size && isASCII(input[index])) {
        output[index] = static_cast<CharacterType>(input[index]);
        ++index;
    }
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

    return index;
}

} // namespace PAL
