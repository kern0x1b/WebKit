#!/usr/bin/env python3

# Copyright (C) 2005, 2006, 2007, 2008 Nikolas Zimmermann <zimmermann@kde.org>
# Copyright (C) 2006 Anders Carlsson <andersca@mac.com>
# Copyright (C) 2006, 2007 Samuel Weinig <sam@webkit.org>
# Copyright (C) 2006 Alexey Proskuryakov <ap@webkit.org>
# Copyright (C) 2006-2023 Apple Inc. All rights reserved.
# Copyright (C) 2009 Cameron McCormack <cam@mcc.id.au>
# Copyright (C) Research In Motion Limited 2010. All rights reserved.
# Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies)
# Copyright (C) 2011 Patrick Gansterer <paroga@webkit.org>
# Copyright (C) 2012 Ericsson AB. All rights reserved.
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Library General Public License
# along with this library; see the file COPYING.LIB.  If not, write to
# the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
# Boston, MA 02110-1301, USA.

mask64 = 2**64 - 1
mask32 = 2**32 - 1
narrowSecretA = 0x53c5ca59
narrowSecretB = 0x74743c1b


def stringHash(str):
    return narrowHash(str)


def maskTop8BitsAndAvoidZero(value):
    value &= mask32

    # Save 8 bits for StringImpl to use as flags.
    value &= 0xffffff

    # This avoids ever returning a hash code of 0, since that is used to
    # signal "hash not computed yet". Setting the high bit maintains
    # reasonable fidelity to a hash code of 0 because it is likely to yield
    # exactly 0 when hash lookup masks out the high bits.
    if not value:
        value = 0x800000
    return value


def narrowHash(string):
    # https://github.com/Nicoshev/rapidhash
    # 32-bit narrow variant (RapidHash::narrowHash), raw ASCII bytes.
    def narrowMix(a, b):
        product = ((a ^ narrowSecretA) * (b ^ narrowSecretB)) & mask64
        return (product & mask32, (product >> 32) & mask32)

    def read32(i):
        return (ord(string[i])
                | (ord(string[i + 1]) << 8)
                | (ord(string[i + 2]) << 16)
                | (ord(string[i + 3]) << 24))

    def readSmall(i, k):
        return ((ord(string[i]) << 16)
                | (ord(string[i + (k >> 1)]) << 8)
                | ord(string[i + k - 1]))

    length = len(string)
    seed = 0
    see1 = length & mask32
    (seed, see1) = narrowMix(seed, see1)

    remaining = length
    offset = 0
    while remaining > 8:
        seed ^= read32(offset)
        see1 ^= read32(offset + 4)
        (seed, see1) = narrowMix(seed, see1)
        offset += 8
        remaining -= 8

    if remaining >= 4:
        seed ^= read32(offset)
        see1 ^= read32(offset + remaining - 4)
    elif remaining:
        seed ^= readSmall(offset, remaining)

    (seed, see1) = narrowMix(seed, see1)
    seed ^= see1
    (seed, see1) = narrowMix(seed, see1)

    return maskTop8BitsAndAvoidZero((seed ^ see1) & mask32)


def ceilingToPowerOf2(v):
    v -= 1
    v |= v >> 1
    v |= v >> 2
    v |= v >> 4
    v |= v >> 8
    v |= v >> 16
    v += 1
    return v


# This is used to compute CompactHashIndex in JSDollarVM.cpp,
# where the indexMask in the corresponding HashTable should
# be numEntries - 1.
def createHashTable(keys, hashTableName):
    def createHashTableHelper(keys, hashTableName):
        table = {}
        links = {}
        compactSize = ceilingToPowerOf2(len(keys))
        maxDepth = 0
        collisions = 0
        numEntries = compactSize

        i = 0
        for key in keys:
            depth = 0
            hashValue = stringHash(key) % numEntries
            while hashValue in table:
                if hashValue in links:
                    hashValue = links[hashValue]
                    depth += 1
                else:
                    collisions += 1
                    links[hashValue] = compactSize
                    hashValue = compactSize
                    compactSize += 1
            table[hashValue] = i
            i += 1
            if depth > maxDepth:
                maxDepth = depth

        string = "static constinit const struct CompactHashIndex {}[{}] = {{\n".format(hashTableName, compactSize)
        for i in range(compactSize):
            T = -1
            if i in table:
                T = table[i]
            L = -1
            if i in links:
                L = links[i]
            string += '    {{ {}, {} }},\n'.format(T, L)
        string += '};\n'
        return string

    hashTableForRapidHash = createHashTableHelper(keys, hashTableName)
    print(hashTableForRapidHash)

