/*
 * Copyright (C) 2012-2024 Apple Inc. All rights reserved.
 * Copyright (C) 2012 Patrick Gansterer <paroga@paroga.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <wtf/Forward.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/Latin1Character.h>

namespace WTF {

enum PositiveOrNegativeNumber { PositiveNumber, NegativeNumber };

template<typename> struct IntegerToStringConversionTrait;

// thresholds[k] is 10^k - 1, so "value > thresholds[k]" is "value has more than k digits".
// The last entry is saturated to the widest representable value, which makes the comparison
// always false and removes the bounds test on the one index that cannot need a correction.
inline constexpr uint32_t decimalDigitThresholds32[11] = {
    0, 9, 99, 999, 9999, 99999, 999999, 9999999, 99999999, 999999999, UINT32_MAX
};

inline constexpr uint64_t decimalDigitThresholds64[20] = {
    0, 9, 99, 999, 9999, 99999, 999999, 9999999, 99999999, 999999999,
    9999999999ULL, 99999999999ULL, 999999999999ULL, 9999999999999ULL, 99999999999999ULL,
    999999999999999ULL, 9999999999999999ULL, 99999999999999999ULL, 999999999999999999ULL,
    9999999999999999999ULL
};

// One count-leading-zeros plus one table compare, instead of one division per digit.
// 1233/4096 underestimates log10(2) by less than 5e-6, so over the 64 bit positions the
// estimate is never more than one digit short and the single compare below fixes it.
template<typename UnsignedIntegerType>
constexpr unsigned decimalDigitCount(UnsignedIntegerType number)
{
    if constexpr (sizeof(UnsignedIntegerType) <= sizeof(uint32_t)) {
        uint32_t value = static_cast<uint32_t>(number);
        unsigned significantBits = 32 - clz(static_cast<uint32_t>(value | 1));
        unsigned estimate = ((significantBits - 1) * 1233) >> 12;
        return estimate + 1 + (value > decimalDigitThresholds32[estimate + 1]);
    } else {
        uint64_t value = static_cast<uint64_t>(number);
        unsigned significantBits = 64 - clz(static_cast<uint64_t>(value | 1));
        unsigned estimate = ((significantBits - 1) * 1233) >> 12;
        return estimate + 1 + (value > decimalDigitThresholds64[estimate + 1]);
    }
}

inline constexpr Latin1Character decimalDigitPairs[200] = {
    '0','0','0','1','0','2','0','3','0','4','0','5','0','6','0','7','0','8','0','9',
    '1','0','1','1','1','2','1','3','1','4','1','5','1','6','1','7','1','8','1','9',
    '2','0','2','1','2','2','2','3','2','4','2','5','2','6','2','7','2','8','2','9',
    '3','0','3','1','3','2','3','3','3','4','3','5','3','6','3','7','3','8','3','9',
    '4','0','4','1','4','2','4','3','4','4','4','5','4','6','4','7','4','8','4','9',
    '5','0','5','1','5','2','5','3','5','4','5','5','5','6','5','7','5','8','5','9',
    '6','0','6','1','6','2','6','3','6','4','6','5','6','6','6','7','6','8','6','9',
    '7','0','7','1','7','2','7','3','7','4','7','5','7','6','7','7','7','8','7','9',
    '8','0','8','1','8','2','8','3','8','4','8','5','8','6','8','7','8','8','8','9',
    '9','0','9','1','9','2','9','3','9','4','9','5','9','6','9','7','9','8','9','9'
};

// Two digits per division by 100 instead of one per division by 10. A Cortex-A9 has no
// integer divide, so each of these is a multiply-high plus shift emitted by the compiler.
template<typename CharacterType>
constexpr size_t writeDecimalDigitsBackward32(uint32_t number, std::span<CharacterType> destination, size_t index)
{
    while (number >= 100) {
        unsigned pair = (number % 100) * 2;
        number /= 100;
        destination[--index] = static_cast<CharacterType>(decimalDigitPairs[pair + 1]);
        destination[--index] = static_cast<CharacterType>(decimalDigitPairs[pair]);
    }
    if (number >= 10) {
        unsigned pair = number * 2;
        destination[--index] = static_cast<CharacterType>(decimalDigitPairs[pair + 1]);
        destination[--index] = static_cast<CharacterType>(decimalDigitPairs[pair]);
        return index;
    }
    destination[--index] = static_cast<CharacterType>('0' + number);
    return index;
}

template<typename CharacterType>
constexpr size_t writeEightDecimalDigitsBackward(uint32_t number, std::span<CharacterType> destination, size_t index)
{
    for (unsigned i = 0; i < 4; ++i) {
        unsigned pair = (number % 100) * 2;
        number /= 100;
        destination[--index] = static_cast<CharacterType>(decimalDigitPairs[pair + 1]);
        destination[--index] = static_cast<CharacterType>(decimalDigitPairs[pair]);
    }
    return index;
}

template<typename CharacterType, typename UnsignedIntegerType>
constexpr size_t writeDecimalDigitsBackward(UnsignedIntegerType number, std::span<CharacterType> destination, size_t index)
{
    if constexpr (sizeof(UnsignedIntegerType) > sizeof(uint32_t)) {
        // At most two of these run: (2^64 - 1) / 10^16 is 1844, which fits in 32 bits.
        while (number > static_cast<UnsignedIntegerType>(UINT32_MAX)) {
            index = writeEightDecimalDigitsBackward(static_cast<uint32_t>(number % 100000000ULL), destination, index);
            number /= 100000000ULL;
        }
    }
    return writeDecimalDigitsBackward32(static_cast<uint32_t>(number), destination, index);
}

template<typename T, typename UnsignedIntegerType, PositiveOrNegativeNumber NumberType, typename AdditionalArgumentType>
static typename IntegerToStringConversionTrait<T>::ReturnType numberToStringImpl(UnsignedIntegerType number, AdditionalArgumentType additionalArgument)
{
    std::array<Latin1Character, sizeof(UnsignedIntegerType) * 3 + 1> buffer;
    auto index = writeDecimalDigitsBackward(number, std::span<Latin1Character> { buffer }, buffer.size());

    if (NumberType == NegativeNumber)
        buffer[--index] = '-';

    return IntegerToStringConversionTrait<T>::flush(std::span { buffer }.subspan(index), additionalArgument);
}

template<typename T, typename SignedIntegerType>
inline typename IntegerToStringConversionTrait<T>::ReturnType numberToStringSigned(SignedIntegerType number, typename IntegerToStringConversionTrait<T>::AdditionalArgumentType* additionalArgument = nullptr)
{
    if (number < 0)
        return numberToStringImpl<T, typename std::make_unsigned_t<SignedIntegerType>, NegativeNumber>(-unsignedCast(number), additionalArgument);
    return numberToStringImpl<T, typename std::make_unsigned_t<SignedIntegerType>, PositiveNumber>(number, additionalArgument);
}

template<typename T, typename UnsignedIntegerType>
inline typename IntegerToStringConversionTrait<T>::ReturnType numberToStringUnsigned(UnsignedIntegerType number, typename IntegerToStringConversionTrait<T>::AdditionalArgumentType* additionalArgument = nullptr)
{
    return numberToStringImpl<T, UnsignedIntegerType, PositiveNumber>(number, additionalArgument);
}

template<typename CharacterType, typename UnsignedIntegerType, PositiveOrNegativeNumber NumberType>
static void writeIntegerToBufferImpl(UnsignedIntegerType number, std::span<CharacterType> destination)
{
    static_assert(!std::is_same_v<bool, std::remove_cv_t<UnsignedIntegerType>>, "'bool' not supported");
    // The digit count is exact, so the digits can be filled in place from the back and the
    // scratch buffer plus its copy-out loop disappear.
    size_t index = decimalDigitCount(number);
    if (NumberType == NegativeNumber)
        ++index;

    index = writeDecimalDigitsBackward(number, destination, index);

    if (NumberType == NegativeNumber)
        destination[--index] = '-';
}

template<typename CharacterType, typename IntegerType>
inline void writeIntegerToBuffer(IntegerType integer, std::span<CharacterType> destination)
{
    static_assert(std::is_integral_v<IntegerType>);
    if constexpr (std::is_same_v<IntegerType, bool>)
        return writeIntegerToBufferImpl<CharacterType, uint8_t, PositiveNumber>(integer ? 1 : 0, destination);
    else if constexpr (std::is_signed_v<IntegerType>) {
        if (integer < 0)
            return writeIntegerToBufferImpl<CharacterType, typename std::make_unsigned_t<IntegerType>, NegativeNumber>(WTF::negate(integer), destination);
        return writeIntegerToBufferImpl<CharacterType, typename std::make_unsigned_t<IntegerType>, PositiveNumber>(unsignedCast(integer), destination);
    } else
        return writeIntegerToBufferImpl<CharacterType, IntegerType, PositiveNumber>(integer, destination);
}

template<typename UnsignedIntegerType, PositiveOrNegativeNumber NumberType>
constexpr unsigned lengthOfIntegerAsStringImpl(UnsignedIntegerType number)
{
    unsigned length = decimalDigitCount(number);

    if (NumberType == NegativeNumber)
        ++length;

    return length;
}

template<typename IntegerType>
constexpr unsigned lengthOfIntegerAsString(IntegerType integer)
{
    static_assert(std::is_integral_v<IntegerType>);
    if constexpr (std::is_same_v<IntegerType, bool>) {
        UNUSED_PARAM(integer);
        return 1;
    }
    else if constexpr (std::is_signed_v<IntegerType>) {
        if (integer < 0)
            return lengthOfIntegerAsStringImpl<typename std::make_unsigned_t<IntegerType>, NegativeNumber>(WTF::negate(integer));
        return lengthOfIntegerAsStringImpl<typename std::make_unsigned_t<IntegerType>, PositiveNumber>(unsignedCast(integer));
    } else
        return lengthOfIntegerAsStringImpl<IntegerType, PositiveNumber>(integer);
}

template<size_t N>
struct IntegerToStringConversionTrait<Vector<Latin1Character, N>> {
    using ReturnType = Vector<Latin1Character, N>;
    using AdditionalArgumentType = void;
    static ReturnType flush(std::span<const Latin1Character> characters, void*) { return characters; }
};

} // namespace WTF

using WTF::numberToStringSigned;
using WTF::numberToStringUnsigned;
using WTF::lengthOfIntegerAsString;
using WTF::writeIntegerToBuffer;
