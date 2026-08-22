#include "code_hook.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <limits>

namespace san9::code_hook {
namespace {

bool WriteRelativeJump(unsigned char* source, const void* target) {
    const auto delta = reinterpret_cast<std::intptr_t>(target) -
                       reinterpret_cast<std::intptr_t>(source + 5);
    if (delta < std::numeric_limits<std::int32_t>::min() ||
        delta > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    source[0] = 0xE9;
    const auto displacement = static_cast<std::int32_t>(delta);
    std::memcpy(source + 1, &displacement, sizeof(displacement));
    return true;
}

} // namespace

bool Install(void* entryValue, void* replacement, std::span<const unsigned char> expected,
             void** trampolineValue) {
    if (!entryValue || !replacement || !trampolineValue || expected.size() < 5) {
        return false;
    }
    auto* entry = static_cast<unsigned char*>(entryValue);
    if (std::memcmp(entry, expected.data(), expected.size()) != 0) {
        return false;
    }

    const std::size_t trampolineSize = expected.size() + 5;
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, trampolineSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        return false;
    }
    std::memcpy(trampoline, entry, expected.size());
    if (!WriteRelativeJump(trampoline + expected.size(), entry + expected.size())) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(entry, expected.size(), PAGE_EXECUTE_READWRITE, &oldProtection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    const bool jumpWritten = WriteRelativeJump(entry, replacement);
    if (jumpWritten) {
        std::memset(entry + 5, 0x90, expected.size() - 5);
    }
    DWORD ignored = 0;
    const BOOL restored = VirtualProtect(entry, expected.size(), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), entry, expected.size());
    if (!jumpWritten || !restored) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    *trampolineValue = trampoline;
    return true;
}

} // namespace san9::code_hook
