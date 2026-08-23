#pragma once

#include <windows.h>

#include <cstring>

namespace san9::import_hook {

template <typename Function>
bool Install(const char* importedModule, const char* importedFunction,
             Function replacement, Function& original) {
    auto* module = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!module) {
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return false;
    }
    const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) {
        return false;
    }

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(module + imports.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* moduleName = reinterpret_cast<const char*>(module + descriptor->Name);
        if (_stricmp(moduleName, importedModule) != 0) {
            continue;
        }
        if (!descriptor->OriginalFirstThunk) {
            return false;
        }
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(module + descriptor->OriginalFirstThunk);
        auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA32*>(module + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) {
                continue;
            }
            const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                module + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name), importedFunction) != 0) {
                continue;
            }
            DWORD oldProtection = 0;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function),
                                PAGE_READWRITE, &oldProtection)) {
                return false;
            }
            original = reinterpret_cast<Function>(addresses->u1.Function);
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&addresses->u1.Function),
                                reinterpret_cast<LONG>(replacement));
            DWORD ignored = 0;
            VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function),
                           oldProtection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), &addresses->u1.Function,
                                  sizeof(addresses->u1.Function));
            return true;
        }
    }
    return false;
}

} // namespace san9::import_hook
