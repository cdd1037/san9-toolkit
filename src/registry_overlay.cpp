#include "registry_overlay.h"

#include "import_hook.h"

#include <windows.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace san9::registry_overlay {
namespace {

constexpr std::array<const char*, 2> kProfileRoots{
    "software\\koei\\san9 tc",
    "software\\koei\\san9pk tc",
};
constexpr char kInstallInfo[] = "installinfo";
constexpr char kFullScreen[] = "fullscreen";
constexpr char kAppPositionX[] = "apppositionx";
constexpr char kAppPositionY[] = "apppositiony";
constexpr wchar_t kConfigEnvironmentVariable[] = L"SAN9_TOOLKIT_CONFIG";
constexpr wchar_t kConfigSection[] = L"Configs";

using RegOpenKeyExAFunction = LSTATUS(WINAPI*)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
using RegCloseKeyFunction = LSTATUS(WINAPI*)(HKEY);
using RegCreateKeyExAFunction = LSTATUS(WINAPI*)(HKEY, LPCSTR, DWORD, LPSTR, DWORD,
                                                 REGSAM, LPSECURITY_ATTRIBUTES,
                                                 PHKEY, LPDWORD);
using RegSetValueExAFunction = LSTATUS(WINAPI*)(HKEY, LPCSTR, DWORD, DWORD,
                                                const BYTE*, DWORD);
using RegDeleteValueAFunction = LSTATUS(WINAPI*)(HKEY, LPCSTR);
using RegQueryValueExAFunction = LSTATUS(WINAPI*)(HKEY, LPCSTR, LPDWORD, LPDWORD,
                                                  LPBYTE, LPDWORD);
using RegEnumValueAFunction = LSTATUS(WINAPI*)(HKEY, DWORD, LPSTR, LPDWORD, LPDWORD,
                                               LPDWORD, LPBYTE, LPDWORD);
using RegEnumKeyExAFunction = LSTATUS(WINAPI*)(HKEY, DWORD, LPSTR, LPDWORD, LPDWORD,
                                               LPSTR, LPDWORD, PFILETIME);

struct StoredValue {
    DWORD type{};
    std::vector<BYTE> data;
};

struct VirtualKey {
    std::string path;
    HKEY backing{};
};

RegOpenKeyExAFunction g_originalRegOpenKeyExA = nullptr;
RegCloseKeyFunction g_originalRegCloseKey = nullptr;
RegCreateKeyExAFunction g_originalRegCreateKeyExA = nullptr;
RegSetValueExAFunction g_originalRegSetValueExA = nullptr;
RegDeleteValueAFunction g_originalRegDeleteValueA = nullptr;
RegQueryValueExAFunction g_originalRegQueryValueExA = nullptr;
RegEnumValueAFunction g_originalRegEnumValueA = nullptr;
RegEnumKeyExAFunction g_originalRegEnumKeyExA = nullptr;

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_set<VirtualKey*> g_keys;
std::unordered_map<std::string, StoredValue> g_values;
std::wstring g_configPath;
std::string g_installDirectory;

std::string Canonicalize(const char* value) {
    std::string result;
    if (!value) {
        return result;
    }
    bool lastWasSeparator = false;
    for (; *value; ++value) {
        const unsigned char byte = static_cast<unsigned char>(*value);
        if (byte == '\\' || byte == '/') {
            if (!result.empty() && !lastWasSeparator) {
                result.push_back('\\');
            }
            lastWasSeparator = true;
        } else {
            result.push_back(static_cast<char>(std::tolower(byte)));
            lastWasSeparator = false;
        }
    }
    if (!result.empty() && result.back() == '\\') {
        result.pop_back();
    }
    return result;
}

std::string JoinPath(const std::string& base, const char* child) {
    const std::string normalizedChild = Canonicalize(child);
    if (base.empty()) {
        return normalizedChild;
    }
    if (normalizedChild.empty()) {
        return base;
    }
    return base + '\\' + normalizedChild;
}

bool IsSameOrDescendant(const std::string& path, const char* root) {
    const std::size_t rootLength = std::strlen(root);
    return path == root ||
           (path.size() > rootLength && path.compare(0, rootLength, root) == 0 &&
            path[rootLength] == '\\');
}

bool IsManagedRelation(const std::string& path) {
    for (const char* profileRoot : kProfileRoots) {
        const std::string root(profileRoot);
        if (IsSameOrDescendant(path, profileRoot) ||
            (path.size() < root.size() && root.compare(0, path.size(), path) == 0 &&
             root[path.size()] == '\\')) {
            return true;
        }
    }
    return false;
}

bool IsProfilePath(const std::string& path) {
    for (const char* profileRoot : kProfileRoots) {
        if (IsSameOrDescendant(path, profileRoot)) {
            return true;
        }
    }
    return false;
}

bool IsProfileSection(const std::string& path, const char* section) {
    for (const char* profileRoot : kProfileRoots) {
        if (path == std::string(profileRoot) + '\\' + section) {
            return true;
        }
    }
    return false;
}

bool IsMainWindowPosition(const std::string& path, LPCSTR valueName) {
    if (!IsProfileSection(path, "configs")) {
        return false;
    }
    const std::string name = Canonicalize(valueName);
    return name == kAppPositionX || name == kAppPositionY;
}

VirtualKey* FindVirtualKey(HKEY key) {
    auto* candidate = reinterpret_cast<VirtualKey*>(key);
    return g_keys.contains(candidate) ? candidate : nullptr;
}

std::string ValueId(const std::string& path, LPCSTR name) {
    return path + '\n' + Canonicalize(name);
}

LSTATUS CopyValue(DWORD type, const BYTE* source, DWORD sourceSize,
                  LPDWORD outputType, LPBYTE output, LPDWORD outputSize) {
    if (!outputSize) {
        return ERROR_INVALID_PARAMETER;
    }
    if (outputType) {
        *outputType = type;
    }
    const DWORD capacity = *outputSize;
    *outputSize = sourceSize;
    if (!output) {
        return ERROR_SUCCESS;
    }
    if (capacity < sourceSize) {
        return ERROR_MORE_DATA;
    }
    if (sourceSize != 0) {
        std::memcpy(output, source, sourceSize);
    }
    return ERROR_SUCCESS;
}

std::wstring ToWide(LPCSTR value) {
    if (!value || !*value) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
    if (length <= 1) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_ACP, 0, value, -1, result.data(), length);
    result.resize(static_cast<std::size_t>(length - 1));
    return result;
}

bool ReadConfiguredDword(LPCSTR valueName, DWORD& value) {
    if (g_configPath.empty() || !valueName) {
        return false;
    }
    const std::wstring wideName = ToWide(valueName);
    if (wideName.empty()) {
        return false;
    }
    constexpr wchar_t missing[] = L"{missing}";
    std::array<wchar_t, 64> buffer{};
    GetPrivateProfileStringW(kConfigSection, wideName.c_str(), missing, buffer.data(),
                             static_cast<DWORD>(buffer.size()), g_configPath.c_str());
    if (std::wcscmp(buffer.data(), missing) == 0) {
        return false;
    }
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(buffer.data(), &end, 0);
    if (end == buffer.data() || *end != L'\0') {
        return false;
    }
    value = static_cast<DWORD>(parsed);
    return true;
}

void PersistConfiguredDword(LPCSTR valueName, DWORD value) {
    if (g_configPath.empty() || !valueName) {
        return;
    }
    const std::wstring wideName = ToWide(valueName);
    if (wideName.empty()) {
        return;
    }
    const std::wstring text = std::to_wstring(value);
    if (!WritePrivateProfileStringW(kConfigSection, wideName.c_str(), text.c_str(),
                                    g_configPath.c_str())) {
        OutputDebugStringW(L"San9Toolkit: failed to persist a virtual registry value.\n");
    }
}

void DeleteConfiguredValue(LPCSTR valueName) {
    if (g_configPath.empty() || !valueName) {
        return;
    }
    const std::wstring wideName = ToWide(valueName);
    if (!wideName.empty()) {
        WritePrivateProfileStringW(kConfigSection, wideName.c_str(), nullptr,
                                   g_configPath.c_str());
    }
}

LSTATUS CreateVirtualKey(const std::string& path, REGSAM, PHKEY result,
                         LPDWORD disposition) {
    HKEY backing = nullptr;
    g_originalRegOpenKeyExA(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &backing);
    auto* key = new (std::nothrow) VirtualKey{path, backing};
    if (!key) {
        if (backing) {
            g_originalRegCloseKey(backing);
        }
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    g_keys.insert(key);
    *result = reinterpret_cast<HKEY>(key);
    if (disposition) {
        *disposition = backing ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
    }
    return ERROR_SUCCESS;
}

LSTATUS WINAPI OverlayRegOpenKeyExA(HKEY key, LPCSTR subKey, DWORD options,
                                    REGSAM access, PHKEY result) {
    if (!result) {
        return ERROR_INVALID_PARAMETER;
    }
    AcquireSRWLockExclusive(&g_lock);
    VirtualKey* parent = FindVirtualKey(key);
    const bool currentUser = key == HKEY_CURRENT_USER;
    if (!parent && !currentUser) {
        ReleaseSRWLockExclusive(&g_lock);
        return g_originalRegOpenKeyExA(key, subKey, options, access, result);
    }
    const std::string path = JoinPath(parent ? parent->path : std::string{}, subKey);
    if (!IsManagedRelation(path)) {
        ReleaseSRWLockExclusive(&g_lock);
        return g_originalRegOpenKeyExA(HKEY_CURRENT_USER, path.c_str(), options, access, result);
    }
    const LSTATUS status = CreateVirtualKey(path, access, result, nullptr);
    ReleaseSRWLockExclusive(&g_lock);
    return status;
}

LSTATUS WINAPI OverlayRegCreateKeyExA(HKEY key, LPCSTR subKey, DWORD reserved,
                                      LPSTR className, DWORD options, REGSAM access,
                                      LPSECURITY_ATTRIBUTES securityAttributes,
                                      PHKEY result, LPDWORD disposition) {
    if (!result) {
        return ERROR_INVALID_PARAMETER;
    }
    AcquireSRWLockExclusive(&g_lock);
    VirtualKey* parent = FindVirtualKey(key);
    const bool currentUser = key == HKEY_CURRENT_USER;
    if (!parent && !currentUser) {
        ReleaseSRWLockExclusive(&g_lock);
        return g_originalRegCreateKeyExA(key, subKey, reserved, className, options, access,
                                         securityAttributes, result, disposition);
    }
    const std::string path = JoinPath(parent ? parent->path : std::string{}, subKey);
    if (!IsManagedRelation(path)) {
        ReleaseSRWLockExclusive(&g_lock);
        return g_originalRegCreateKeyExA(HKEY_CURRENT_USER, path.c_str(), reserved, className,
                                         options, access, securityAttributes, result, disposition);
    }
    const LSTATUS status = CreateVirtualKey(path, access, result, disposition);
    ReleaseSRWLockExclusive(&g_lock);
    return status;
}

LSTATUS WINAPI OverlayRegCloseKey(HKEY key) {
    AcquireSRWLockExclusive(&g_lock);
    VirtualKey* virtualKey = FindVirtualKey(key);
    if (!virtualKey) {
        ReleaseSRWLockExclusive(&g_lock);
        return g_originalRegCloseKey(key);
    }
    g_keys.erase(virtualKey);
    if (virtualKey->backing) {
        g_originalRegCloseKey(virtualKey->backing);
    }
    delete virtualKey;
    ReleaseSRWLockExclusive(&g_lock);
    return ERROR_SUCCESS;
}

LSTATUS WINAPI OverlayRegQueryValueExA(HKEY key, LPCSTR valueName, LPDWORD reserved,
                                       LPDWORD type, LPBYTE data, LPDWORD dataSize) {
    AcquireSRWLockExclusive(&g_lock);
    VirtualKey* virtualKey = FindVirtualKey(key);
    if (!virtualKey) {
        ReleaseSRWLockExclusive(&g_lock);
        return g_originalRegQueryValueExA(key, valueName, reserved, type, data, dataSize);
    }
    if (!IsProfilePath(virtualKey->path)) {
        const HKEY backing = virtualKey->backing;
        const LSTATUS status = backing
                                   ? g_originalRegQueryValueExA(backing, valueName, reserved,
                                                                type, data, dataSize)
                                   : ERROR_FILE_NOT_FOUND;
        ReleaseSRWLockExclusive(&g_lock);
        return status;
    }
    if (IsMainWindowPosition(virtualKey->path, valueName)) {
        ReleaseSRWLockExclusive(&g_lock);
        return ERROR_FILE_NOT_FOUND;
    }

    const auto stored = g_values.find(ValueId(virtualKey->path, valueName));
    if (stored != g_values.end()) {
        const LSTATUS status = CopyValue(stored->second.type, stored->second.data.data(),
                                         static_cast<DWORD>(stored->second.data.size()),
                                         type, data, dataSize);
        ReleaseSRWLockExclusive(&g_lock);
        return status;
    }
    if (IsProfileSection(virtualKey->path, "install") &&
        Canonicalize(valueName) == kInstallInfo) {
        const LSTATUS status = CopyValue(REG_SZ,
                                         reinterpret_cast<const BYTE*>(g_installDirectory.c_str()),
                                         static_cast<DWORD>(g_installDirectory.size() + 1),
                                         type, data, dataSize);
        ReleaseSRWLockExclusive(&g_lock);
        return status;
    }
    if (IsProfileSection(virtualKey->path, "configs")) {
        DWORD configured = 0;
        const std::string canonicalName = Canonicalize(valueName);
        if (ReadConfiguredDword(valueName, configured) || canonicalName == kFullScreen) {
            const LSTATUS status = CopyValue(REG_DWORD,
                                             reinterpret_cast<const BYTE*>(&configured),
                                             sizeof(configured), type, data, dataSize);
            ReleaseSRWLockExclusive(&g_lock);
            return status;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ERROR_FILE_NOT_FOUND;
}

LSTATUS WINAPI OverlayRegSetValueExA(HKEY key, LPCSTR valueName, DWORD reserved,
                                     DWORD type, const BYTE* data, DWORD dataSize) {
    AcquireSRWLockExclusive(&g_lock);
    VirtualKey* virtualKey = FindVirtualKey(key);
    if (!virtualKey) {
        ReleaseSRWLockExclusive(&g_lock);
        return g_originalRegSetValueExA(key, valueName, reserved, type, data, dataSize);
    }
    if (!IsProfilePath(virtualKey->path)) {
        const HKEY backing = virtualKey->backing;
        const LSTATUS status = backing
                                   ? g_originalRegSetValueExA(backing, valueName, reserved, type,
                                                              data, dataSize)
                                   : ERROR_FILE_NOT_FOUND;
        ReleaseSRWLockExclusive(&g_lock);
        return status;
    }
    if (!valueName || (!data && dataSize != 0)) {
        ReleaseSRWLockExclusive(&g_lock);
        return ERROR_INVALID_PARAMETER;
    }
    if (IsMainWindowPosition(virtualKey->path, valueName)) {
        ReleaseSRWLockExclusive(&g_lock);
        return ERROR_SUCCESS;
    }
    StoredValue stored;
    stored.type = type;
    if (dataSize != 0) {
        stored.data.assign(data, data + dataSize);
    }
    g_values[ValueId(virtualKey->path, valueName)] = std::move(stored);
    if (IsProfileSection(virtualKey->path, "configs") && type == REG_DWORD &&
        dataSize == sizeof(DWORD)) {
        DWORD value = 0;
        std::memcpy(&value, data, sizeof(value));
        PersistConfiguredDword(valueName, value);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ERROR_SUCCESS;
}

LSTATUS WINAPI OverlayRegDeleteValueA(HKEY key, LPCSTR valueName) {
    AcquireSRWLockExclusive(&g_lock);
    VirtualKey* virtualKey = FindVirtualKey(key);
    if (!virtualKey) {
        ReleaseSRWLockExclusive(&g_lock);
        return g_originalRegDeleteValueA(key, valueName);
    }
    if (!IsProfilePath(virtualKey->path)) {
        const HKEY backing = virtualKey->backing;
        const LSTATUS status = backing ? g_originalRegDeleteValueA(backing, valueName)
                                       : ERROR_FILE_NOT_FOUND;
        ReleaseSRWLockExclusive(&g_lock);
        return status;
    }
    g_values.erase(ValueId(virtualKey->path, valueName));
    if (IsProfileSection(virtualKey->path, "configs")) {
        DeleteConfiguredValue(valueName);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ERROR_SUCCESS;
}

LSTATUS WINAPI OverlayRegEnumValueA(HKEY key, DWORD index, LPSTR valueName,
                                    LPDWORD valueNameSize, LPDWORD reserved, LPDWORD type,
                                    LPBYTE data, LPDWORD dataSize) {
    AcquireSRWLockExclusive(&g_lock);
    VirtualKey* virtualKey = FindVirtualKey(key);
    if (!virtualKey) {
        ReleaseSRWLockExclusive(&g_lock);
        return g_originalRegEnumValueA(key, index, valueName, valueNameSize, reserved, type,
                                       data, dataSize);
    }
    const HKEY backing = virtualKey->backing;
    const bool profile = IsProfilePath(virtualKey->path);
    const LSTATUS status = !profile && backing
                               ? g_originalRegEnumValueA(backing, index, valueName, valueNameSize,
                                                        reserved, type, data, dataSize)
                               : ERROR_NO_MORE_ITEMS;
    ReleaseSRWLockExclusive(&g_lock);
    return status;
}

LSTATUS WINAPI OverlayRegEnumKeyExA(HKEY key, DWORD index, LPSTR name, LPDWORD nameSize,
                                    LPDWORD reserved, LPSTR className, LPDWORD classSize,
                                    PFILETIME lastWriteTime) {
    AcquireSRWLockExclusive(&g_lock);
    VirtualKey* virtualKey = FindVirtualKey(key);
    if (!virtualKey) {
        ReleaseSRWLockExclusive(&g_lock);
        return g_originalRegEnumKeyExA(key, index, name, nameSize, reserved, className,
                                       classSize, lastWriteTime);
    }
    const HKEY backing = virtualKey->backing;
    const bool profile = IsProfilePath(virtualKey->path);
    const LSTATUS status = !profile && backing
                               ? g_originalRegEnumKeyExA(backing, index, name, nameSize, reserved,
                                                        className, classSize, lastWriteTime)
                               : ERROR_NO_MORE_ITEMS;
    ReleaseSRWLockExclusive(&g_lock);
    return status;
}

bool InitializePaths() {
    std::array<wchar_t, 32768> config{};
    const DWORD configLength = GetEnvironmentVariableW(
        kConfigEnvironmentVariable, config.data(), static_cast<DWORD>(config.size()));
    if (configLength != 0 && configLength < config.size()) {
        g_configPath.assign(config.data(), configLength);
    }

    std::array<char, MAX_PATH> executable{};
    const DWORD length = GetModuleFileNameA(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        return false;
    }
    g_installDirectory.assign(executable.data(), length);
    const std::size_t separator = g_installDirectory.find_last_of("\\/");
    if (separator == std::string::npos) {
        return false;
    }
    g_installDirectory.resize(separator + 1);
    return true;
}

} // namespace

bool Install() {
    return InitializePaths() &&
           import_hook::Install("advapi32.dll", "RegOpenKeyExA", &OverlayRegOpenKeyExA,
                                g_originalRegOpenKeyExA) &&
           import_hook::Install("advapi32.dll", "RegCloseKey", &OverlayRegCloseKey,
                                g_originalRegCloseKey) &&
           import_hook::Install("advapi32.dll", "RegCreateKeyExA", &OverlayRegCreateKeyExA,
                                g_originalRegCreateKeyExA) &&
           import_hook::Install("advapi32.dll", "RegSetValueExA", &OverlayRegSetValueExA,
                                g_originalRegSetValueExA) &&
           import_hook::Install("advapi32.dll", "RegDeleteValueA", &OverlayRegDeleteValueA,
                                g_originalRegDeleteValueA) &&
           import_hook::Install("advapi32.dll", "RegQueryValueExA", &OverlayRegQueryValueExA,
                                g_originalRegQueryValueExA) &&
           import_hook::Install("advapi32.dll", "RegEnumValueA", &OverlayRegEnumValueA,
                                g_originalRegEnumValueA) &&
           import_hook::Install("advapi32.dll", "RegEnumKeyExA", &OverlayRegEnumKeyExA,
                                g_originalRegEnumKeyExA);
}

void Shutdown() {
    AcquireSRWLockExclusive(&g_lock);
    for (VirtualKey* key : g_keys) {
        if (key->backing && g_originalRegCloseKey) {
            g_originalRegCloseKey(key->backing);
        }
        delete key;
    }
    g_keys.clear();
    g_values.clear();
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace san9::registry_overlay
