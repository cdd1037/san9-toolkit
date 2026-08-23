#pragma once

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace san9::toolkit_config {

struct Settings {
    std::wstring gameExecutable;
    std::wstring documentsRoot;
    UINT cursorLockVirtualKey = VK_F12;
    bool scaleInitialWindowForSystemDpi = true;
    std::wstring windowTitle = L"三國志ⅨPK";
    bool borderlessWindow = false;
    DWORD gameSpeedMultiplier = 2;
    DWORD messageSpeed = 1;
    DWORD gameReport = 1;
    bool playBgm = true;
    bool playSound = true;
    bool playMovie = true;
};

inline std::wstring ReadString(const std::filesystem::path& path, const wchar_t* section,
                               const wchar_t* key, const wchar_t* fallback) {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetPrivateProfileStringW(section, key, fallback, buffer.data(),
                                                   static_cast<DWORD>(buffer.size()), path.c_str());
    buffer.resize(length);
    return buffer;
}

inline DWORD ReadDword(const std::filesystem::path& path, const wchar_t* section,
                       const wchar_t* key, DWORD fallback, DWORD minimum, DWORD maximum) {
    const std::wstring fallbackText = std::to_wstring(fallback);
    const std::wstring text = ReadString(path, section, key, fallbackText.c_str());
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(text.c_str(), &end, 10);
    if (!end || *end != L'\0' || value < minimum || value > maximum) {
        return fallback;
    }
    return static_cast<DWORD>(value);
}

inline Settings Load(const std::filesystem::path& path) {
    Settings result;
    result.gameExecutable = ReadString(path, L"Toolkit", L"GameExecutable", L"");
    result.documentsRoot = ReadString(path, L"Toolkit", L"DocumentsRoot", L"");
    result.cursorLockVirtualKey = ReadDword(path, L"Toolkit", L"CursorLockVirtualKey", VK_F12, 1, 255);
    result.scaleInitialWindowForSystemDpi =
        ReadDword(path, L"Toolkit", L"ScaleInitialWindowForSystemDpi", 1, 0, 1) != 0;
    result.windowTitle = ReadString(path, L"Toolkit", L"WindowTitle", L"三國志ⅨPK");
    if (result.windowTitle.empty()) {
        result.windowTitle = L"三國志ⅨPK";
    }
    result.borderlessWindow = ReadDword(path, L"Toolkit", L"BorderlessWindow", 0, 0, 1) != 0;
    result.gameSpeedMultiplier = ReadDword(path, L"Toolkit", L"GameSpeedMultiplier", 2, 1, 4);
    result.messageSpeed = ReadDword(path, L"Configs", L"MessageSpeed", 1, 0, 2);
    result.gameReport = ReadDword(path, L"Configs", L"GameReport", 1, 0, 2);
    result.playBgm = ReadDword(path, L"Configs", L"PlayBGM", 1, 0, 1) != 0;
    result.playSound = ReadDword(path, L"Configs", L"PlaySound", 1, 0, 1) != 0;
    result.playMovie = ReadDword(path, L"Configs", L"PlayMovie", 1, 0, 1) != 0;
    return result;
}

inline bool WriteValue(const std::filesystem::path& path, const wchar_t* section,
                       const wchar_t* key, const std::wstring& value) {
    return WritePrivateProfileStringW(section, key, value.c_str(), path.c_str()) != FALSE;
}

inline bool Save(const std::filesystem::path& path, const Settings& value) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    const auto number = [](DWORD item) { return std::to_wstring(item); };
    const bool saved =
        WriteValue(path, L"Toolkit", L"GameExecutable", value.gameExecutable) &&
        WriteValue(path, L"Toolkit", L"DocumentsRoot", value.documentsRoot) &&
        WriteValue(path, L"Toolkit", L"CursorLockVirtualKey", number(value.cursorLockVirtualKey)) &&
        WriteValue(path, L"Toolkit", L"ScaleInitialWindowForSystemDpi", number(value.scaleInitialWindowForSystemDpi)) &&
        WriteValue(path, L"Toolkit", L"WindowTitle", value.windowTitle.empty() ? L"三國志ⅨPK" : value.windowTitle) &&
        WriteValue(path, L"Toolkit", L"BorderlessWindow", number(value.borderlessWindow)) &&
        WriteValue(path, L"Toolkit", L"GameSpeedMultiplier", number(value.gameSpeedMultiplier)) &&
        WriteValue(path, L"Configs", L"MessageSpeed", number(value.messageSpeed)) &&
        WriteValue(path, L"Configs", L"GameReport", number(value.gameReport)) &&
        WriteValue(path, L"Configs", L"PlayBGM", number(value.playBgm)) &&
        WriteValue(path, L"Configs", L"PlaySound", number(value.playSound)) &&
        WriteValue(path, L"Configs", L"PlayMovie", number(value.playMovie)) &&
        WriteValue(path, L"Configs", L"FullScreen", L"0");
    if (!saved) {
        return false;
    }

    // The documented cache-flush form returns zero even when the flush occurs.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return true;
}

} // namespace san9::toolkit_config
