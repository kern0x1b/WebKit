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

package Hasher;

use strict;
use integer;

# Performance: 'use integer' gives native integer arithmetic. The narrow
# 32-bit mixer never needs more than 32 bits per operand, so all products
# below are built from 16-bit halves and stay well inside an IV.

my $mask32 = 0xFFFFFFFF;
my $narrowSecretA = 0x53c5ca59;
my $narrowSecretB = 0x74743c1b;

sub maskTop8BitsAndAvoidZero($) {
    my ($value) = @_;

    $value &= $mask32;

    # Save 8 bits for StringImpl to use as flags.
    $value &= 0xffffff;

    # This avoids ever returning a hash code of 0, since that is used to
    # signal "hash not computed yet". Setting the high bit maintains
    # reasonable fidelity to a hash code of 0 because it is likely to yield
    # exactly 0 when hash lookup masks out the high bits.
    $value = (0x80000000 >> 8) if ($value == 0);

    return $value;
}

# 32-bit multiply as four 16-bit partial products: a direct 32x32 multiply
# overflows Perl's integer and silently degrades to a double.
sub _mul32($$) {
    my ($a, $b) = @_;

    my $al = $a & 0xFFFF;
    my $ah = ($a >> 16) & 0xFFFF;
    my $bl = $b & 0xFFFF;
    my $bh = ($b >> 16) & 0xFFFF;

    my $ll = $al * $bl;
    my $lh = $al * $bh;
    my $hl = $ah * $bl;
    my $hh = $ah * $bh;

    my $mid = ($ll >> 16) + ($lh & 0xFFFF) + ($hl & 0xFFFF);
    my $lo = ((($mid & 0xFFFF) << 16) | ($ll & 0xFFFF)) & $mask32;
    my $hi = ($hh + ($lh >> 16) + ($hl >> 16) + ($mid >> 16)) & $mask32;

    return ($lo, $hi);
}

sub _narrowMix($$) {
    my ($a, $b) = @_;
    return _mul32(($a ^ $narrowSecretA) & $mask32, ($b ^ $narrowSecretB) & $mask32);
}

# Read 4 bytes from string at index $i as a little-endian 32-bit value.
sub _read32($$) {
    my ($str, $i) = @_;
    return (ord(substr($str, $i, 1))
        | (ord(substr($str, $i + 1, 1)) << 8)
        | (ord(substr($str, $i + 2, 1)) << 16)
        | (ord(substr($str, $i + 3, 1)) << 24)) & $mask32;
}

# Read 1-3 bytes from string at index $i (length $k) into a 32-bit value.
sub _readSmall($$$) {
    my ($str, $i, $k) = @_;
    return ((ord(substr($str, $i, 1)) << 16)
        | (ord(substr($str, $i + ($k >> 1), 1)) << 8)
        | ord(substr($str, $i + $k - 1, 1))) & $mask32;
}

sub GenerateHashValue($) {
    my ($string) = @_;

    # https://github.com/Nicoshev/rapidhash
    # 32-bit narrow variant (RapidHash::narrowHash), raw ASCII bytes.
    my $len = length($string);

    my $seed = 0;
    my $see1 = $len & $mask32;
    ($seed, $see1) = _narrowMix($seed, $see1);

    my $remaining = $len;
    my $offset = 0;
    while ($remaining > 8) {
        $seed ^= _read32($string, $offset);
        $see1 ^= _read32($string, $offset + 4);
        ($seed, $see1) = _narrowMix($seed, $see1);
        $offset += 8;
        $remaining -= 8;
    }

    if ($remaining >= 4) {
        $seed ^= _read32($string, $offset);
        $see1 ^= _read32($string, $offset + $remaining - 4);
    } elsif ($remaining) {
        $seed ^= _readSmall($string, $offset, $remaining);
    }

    ($seed, $see1) = _narrowMix($seed, $see1);
    $seed ^= $see1;
    ($seed, $see1) = _narrowMix($seed, $see1);

    return maskTop8BitsAndAvoidZero(($seed ^ $see1) & $mask32);
}

1;
