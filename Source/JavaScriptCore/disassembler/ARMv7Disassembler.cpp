/*
 * Copyright (C) 2013 Apple Inc. All rights reserved.
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
#include "AssemblyComments.h"
#include "Disassembler.h"

#if ENABLE(ARMV7_DISASSEMBLER)

#include "ARMv7DOpcode.h"
#include "MacroAssemblerCodeRef.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

bool tryToDisassemble(const CodePtr<DisassemblyPtrTag>& codePtr, size_t size, void* codeStart, void*, const char* prefix, PrintStream& out)
{
    ARMv7Disassembler::ARMv7DOpcode armOpcode;

    uint16_t* currentPC = reinterpret_cast<uint16_t*>(std::bit_cast<uintptr_t>(codePtr.untaggedPtr()) & ~1);
    uint16_t* endPC = currentPC + (size / sizeof(uint16_t));
    uint16_t* armCodeStart = reinterpret_cast<uint16_t*>(std::bit_cast<uintptr_t>(codeStart) & ~1);

    unsigned pcOffset = codeStart ? static_cast<unsigned>((currentPC - armCodeStart) * sizeof(uint16_t)) : 0;

    while (currentPC < endPC) {
        uint16_t* instructionPC = currentPC;
        char pcInfo[25];
        if (codeStart)
            snprintf(pcInfo, sizeof(pcInfo) - 1, "<%u> %#llx", pcOffset, static_cast<unsigned long long>(std::bit_cast<uintptr_t>(instructionPC)));
        else
            snprintf(pcInfo, sizeof(pcInfo) - 1, "%#llx", static_cast<unsigned long long>(std::bit_cast<uintptr_t>(instructionPC)));
        const char* text = armOpcode.disassemble(currentPC);
        out.printf("%s%24s: %s", prefix, pcInfo, text);
        if (auto str = AssemblyCommentRegistry::singleton().comment(instructionPC))
            out.printf("; %s\n", str->ascii().data());
        else
            out.printf("\n");
        pcOffset += static_cast<unsigned>((currentPC - instructionPC) * sizeof(uint16_t));
    }

    return true;
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(ARMV7_DISASSEMBLER)
