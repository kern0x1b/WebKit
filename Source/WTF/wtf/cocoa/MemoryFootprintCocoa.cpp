/*
 * Copyright (C) 2017 Apple Inc. All rights reserved.
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

#include "config.h"
#include <wtf/MemoryFootprint.h>

#include <mach/mach.h>
#include <cstring>
#include <wtf/Assertions.h>
#include <cstdlib>
#include <mach/task_info.h>

namespace WTF {

size_t memoryFootprint()
{
#if defined(WEBKIT_IOS6)
    // This kernel answers TASK_VM_INFO at revision 0, which stops short of
    // phys_footprint — task_info writes nothing there and leaves whatever the
    // caller's stack held, so the modern query returns noise. Resident size is
    // also the number jetsam judges a process by on this release.
    //
    // But resident size counts every resident page the task maps, including the
    // shared cache and our own framework text - measured at 138.7 MB, flat, of a
    // ~220 MB total. So the collector's own bands, which are about how hard to
    // collect, were being crossed by system libraries being paged in rather than
    // by anything the collector can influence: collecting harder does not unmap
    // libobjc. Revision 0 does carry `internal`, the task's own anonymous
    // memory, which is what phys_footprint approximates upstream.
    //
    // WEBKIT_IOS6_FOOTPRINT=resident restores the old number.
    // Measured, not assumed: this returned resident_size to the megabyte on every
    // call - 228/228, 224/224, 229/229, 233/233 - so either TASK_VM_INFO is refused
    // at this revision and the fallback below runs, or internal+compressed is the
    // same quantity here. Either way the change did nothing, and the bands that were
    // recalibrated onto it were therefore recalibrated onto resident size while
    // being reasoned about as though they were not. Two deaths and seven code wipes
    // in one soak followed.
    //
    // Left in place, disabled, with the diagnostic that would settle which of the two
    // it is: WEBKIT_IOS6_FOOTPRINT=own turns it on and logs the return code and both
    // numbers once. Do not enable it again without reading that line first.
    static const int useInternal = [] -> int {
        const char* mode = getenv("WEBKIT_IOS6_FOOTPRINT");
        return mode && !strcmp(mode, "own");
    }();

    if (useInternal) {
        task_vm_info_data_t vmInfo;
        mach_msg_type_number_t vmCount = TASK_VM_INFO_REV0_COUNT;
        kern_return_t vmResult = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmInfo, &vmCount);
        static bool reported = false;
        if (!reported) {
            reported = true;
            WTFLogAlways("[footprint] TASK_VM_INFO kr=%d internal=%llu external=%llu compressed=%llu",
                (int)vmResult, (unsigned long long)vmInfo.internal,
                (unsigned long long)vmInfo.external, (unsigned long long)vmInfo.compressed);
        }
        if (vmResult == KERN_SUCCESS)
            return static_cast<size_t>(vmInfo.internal + vmInfo.compressed);
    }

    mach_task_basic_info_data_t taskInfo;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    kern_return_t result = task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t) &taskInfo, &count);
    if (result != KERN_SUCCESS)
        return 0;
    return static_cast<size_t>(taskInfo.resident_size);
#else
    task_vm_info_data_t vmInfo;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    kern_return_t result = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t) &vmInfo, &count);
    if (result != KERN_SUCCESS)
        return 0;
    return static_cast<size_t>(vmInfo.phys_footprint);
#endif
}

}
