/*
 * Copyright (C) 2022-2024 Apple Inc. All rights reserved.
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
#include "JSCBytecodeCacheVersion.h"

#include <wtf/DataLog.h>
#include <wtf/HexNumber.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/SuperFastHash.h>

#if OS(UNIX)
#include <dlfcn.h>
#include <sys/stat.h>
#if OS(DARWIN)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <uuid/uuid.h>
#include <wtf/spi/darwin/dyldSPI.h>
#elif OS(QNX)
#include <sys/link.h>
#else
#include <link.h>
#endif
#endif

namespace JSC {

namespace JSCBytecodeCacheVersionInternal {
static constexpr bool verbose = false;
}

#if OS(DARWIN)
static bool readMachOUUID(const void* imageBase, std::span<uint8_t, 16> result)
{
    if (!imageBase)
        return false;

    const auto* header = static_cast<const mach_header*>(imageBase);
    uint32_t commandCount = 0;
    const uint8_t* cursor = nullptr;

    if (header->magic == MH_MAGIC_64 || header->magic == MH_CIGAM_64) {
        const auto* header64 = static_cast<const mach_header_64*>(imageBase);
        commandCount = header64->ncmds;
        cursor = std::bit_cast<const uint8_t*>(header64 + 1);
    } else if (header->magic == MH_MAGIC || header->magic == MH_CIGAM) {
        commandCount = header->ncmds;
        cursor = std::bit_cast<const uint8_t*>(header + 1);
    } else
        return false;

    for (uint32_t i = 0; i < commandCount; ++i) {
        const auto* command = std::bit_cast<const load_command*>(cursor);
        if (command->cmdsize < sizeof(load_command))
            return false;
        if (command->cmd == LC_UUID) {
            if (command->cmdsize < sizeof(uuid_command))
                return false;
            const auto* uuidCommand = std::bit_cast<const uuid_command*>(command);
            for (size_t byte = 0; byte < 16; ++byte)
                result[byte] = uuidCommand->uuid[byte];
            return true;
        }
        cursor += command->cmdsize;
    }
    return false;
}
#endif

uint64_t computeJSCBinaryIdentity()
{
    static LazyNeverDestroyed<uint64_t> identity;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        uint64_t value = 0;
#if OS(UNIX)
        Dl_info info { };
        if (dladdr(std::bit_cast<void*>(&computeJSCBinaryIdentity), &info)) {
#if OS(DARWIN)
            std::array<uint8_t, 16> uuid { };
            if (readMachOUUID(info.dli_fbase, std::span<uint8_t, 16> { uuid })) {
                uint64_t high = 0;
                uint64_t low = 0;
                for (size_t i = 0; i < 8; ++i) {
                    high = (high << 8) | uuid[i];
                    low = (low << 8) | uuid[i + 8];
                }
                value = high ^ (low * 0x9e3779b97f4a7c15ull);
                dataLogLnIf(JSCBytecodeCacheVersionInternal::verbose, "JavaScriptCore LC_UUID identity: ", value);
            }
#endif
            if (!value && info.dli_fname) {
                struct stat statBuffer { };
                if (!stat(info.dli_fname, &statBuffer)) {
                    value = static_cast<uint64_t>(statBuffer.st_size);
                    value = (value * 0x100000001b3ull) ^ static_cast<uint64_t>(statBuffer.st_mtime);
                    value = (value * 0x100000001b3ull) ^ static_cast<uint64_t>(statBuffer.st_ino);
                    dataLogLnIf(JSCBytecodeCacheVersionInternal::verbose, "JavaScriptCore stat identity: ", value);
                }
            }
        }
#endif
        if (!value) {
            static constexpr uint32_t stamp = SuperFastHash::computeHash(__DATE__ " " __TIME__);
            value = (static_cast<uint64_t>(stamp) << 32) | stamp | 1;
        }
        identity.construct(value);
    });
    return identity.get();
}

uint32_t computeJSCBytecodeCacheVersion()
{
    UNUSED_VARIABLE(JSCBytecodeCacheVersionInternal::verbose);
    static LazyNeverDestroyed<uint32_t> cacheVersion;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        void* jsFunctionAddr = std::bit_cast<void*>(&computeJSCBytecodeCacheVersion);
#if OS(DARWIN)
        uuid_t uuid;
        if (const mach_header* header = dyld_image_header_containing_address(jsFunctionAddr); header && _dyld_get_image_uuid(header, uuid)) {
            uuid_string_t uuidString = { };
            uuid_unparse(uuid, uuidString);
            cacheVersion.construct(SuperFastHash::computeHash(uuidString));
            dataLogLnIf(JSCBytecodeCacheVersionInternal::verbose, "UUID of JavaScriptCore.framework:", uuidString);
            return;
        }
        {
            uint64_t identity = computeJSCBinaryIdentity();
            cacheVersion.construct(static_cast<uint32_t>(identity ^ (identity >> 32)));
        }
        dataLogLnIf(JSCBytecodeCacheVersionInternal::verbose, "Failed to get UUID for JavaScriptCore framework, using binary identity");
#elif OS(UNIX) && !PLATFORM(PLAYSTATION) && !OS(HAIKU) && !OS(QNX)
        auto result = ([&] -> std::optional<uint32_t> {
            Dl_info info { };
            if (!dladdr(jsFunctionAddr, &info))
                return std::nullopt;

            if (!info.dli_fbase)
                return std::nullopt;

            struct DLParam {
                void* start { nullptr };
                std::span<const uint8_t> description;
            };

            DLParam param { };
            param.start = info.dli_fbase;
            if (!dl_iterate_phdr(static_cast<int(*)(struct dl_phdr_info*, size_t, void*)>(
                [](struct dl_phdr_info* info, size_t, void* priv) -> int {
                    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN // Unix port
                    auto* data = static_cast<DLParam*>(priv);
                    void* start = nullptr;
                    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
                        if (info->dlpi_phdr[i].p_type == PT_LOAD) {
                            start = std::bit_cast<void*>(static_cast<uintptr_t>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr));
                            break;
                        }
                    }

                    if (start != data->start)
                        return 0;

                    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
                        if (info->dlpi_phdr[i].p_type != PT_NOTE)
                            continue;

                        // https://refspecs.linuxbase.org/elf/gabi4+/ch5.pheader.html#note_section
                        using NoteHeader = ElfW(Nhdr);

                        auto* payload = std::bit_cast<uint8_t*>(static_cast<uintptr_t>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr));
                        size_t length = info->dlpi_phdr[i].p_filesz;
                        for (size_t index = 0; index < length;) {
                            auto* cursor  = payload + index;
                            if ((index + sizeof(NoteHeader)) > length)
                                return 0;

                            auto* note = std::bit_cast<NoteHeader*>(cursor);
                            size_t size = sizeof(NoteHeader) + roundUpToMultipleOf<4>(note->n_namesz) + roundUpToMultipleOf<4>(note->n_descsz);
                            if ((index + size) > length)
                                return 0;

                            auto* name = cursor + sizeof(NoteHeader);
                            auto* description = cursor + sizeof(NoteHeader) + roundUpToMultipleOf<4>(note->n_namesz);

                            if (note->n_type == NT_GNU_BUILD_ID && note->n_descsz != 0 && note->n_namesz == 4 && memcmp(name, "GNU", 4) == 0) {
                                // Found build-id note.
                                data->description = std::span { description, note->n_descsz };
                                return 1;
                            }

                            index += size;
                        }
                    }
                    return 0;
                    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
                }), &param))
                    return std::nullopt;

                if (param.description.empty())
                    return std::nullopt;

                if constexpr (JSCBytecodeCacheVersionInternal::verbose) {
                    for (uint8_t value : param.description)
                        dataLog(hex(value));
                    dataLogLn("");
                }

                return SuperFastHash::computeHash(param.description);
        }());
        if (result) {
            cacheVersion.construct(result.value());
            return;
        }
        {
            uint64_t identity = computeJSCBinaryIdentity();
            cacheVersion.construct(static_cast<uint32_t>(identity ^ (identity >> 32)));
        }
        dataLogLnIf(JSCBytecodeCacheVersionInternal::verbose, "Failed to get UUID for JavaScriptCore framework, using binary identity");
#else
        UNUSED_VARIABLE(jsFunctionAddr);
        static constexpr uint32_t precomputedCacheVersion = SuperFastHash::computeHash(__TIMESTAMP__);
        cacheVersion.construct(precomputedCacheVersion);
#endif
    });
    return cacheVersion.get();
}

} // namespace JSC
