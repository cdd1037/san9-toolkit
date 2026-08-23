#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

namespace san9::documents_path {

constexpr char kLongestGameSuffix[] = "\\Koei\\San9PK Tc";

inline bool EncodeRoot(const std::wstring& value, std::string& encoded) {
    if (value.empty() || !std::filesystem::path(value).is_absolute()) {
        return false;
    }

    BOOL usedDefaultCharacter = FALSE;
    const int byteCount = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, value.c_str(), -1,
                                               nullptr, 0, nullptr, &usedDefaultCharacter);
    if (byteCount <= 1 || usedDefaultCharacter) {
        return false;
    }
    encoded.resize(static_cast<std::size_t>(byteCount));
    usedDefaultCharacter = FALSE;
    if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, value.c_str(), -1,
                            encoded.data(), byteCount, nullptr, &usedDefaultCharacter) != byteCount ||
        usedDefaultCharacter) {
        return false;
    }
    encoded.resize(static_cast<std::size_t>(byteCount - 1));

    while (encoded.size() > 3 && (encoded.back() == '\\' || encoded.back() == '/')) {
        encoded.pop_back();
    }
    return encoded.size() + sizeof(kLongestGameSuffix) <= MAX_PATH;
}

} // namespace san9::documents_path
