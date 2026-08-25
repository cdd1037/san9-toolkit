#pragma once

#include <filesystem>
#include <string_view>

namespace san9::movie_trace {

void Initialize(const std::filesystem::path& configPath);
void BeginPlayback(std::wstring_view movieName);
void Record(std::wstring_view event, std::wstring_view details = {});
void EndPlayback(std::wstring_view details);

} // namespace san9::movie_trace
