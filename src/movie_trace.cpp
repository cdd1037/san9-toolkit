#include "movie_trace.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <string>

namespace san9::movie_trace {
namespace {

constexpr std::uint64_t kMaximumTraceBytes = 1024 * 1024;

std::mutex g_mutex;
std::filesystem::path g_tracePath;
std::filesystem::path g_previousPath;
HANDLE g_file = INVALID_HANDLE_VALUE;
std::uint64_t g_bytesWritten = 0;
ULONGLONG g_startTick = 0;
bool g_limitReported = false;

std::string ToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), required,
                            nullptr, nullptr) != required) {
        return {};
    }
    return result;
}

std::string MakeLine(std::wstring_view event, std::wstring_view details) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    const ULONGLONG elapsed = g_startTick == 0 ? 0 : GetTickCount64() - g_startTick;
    std::array<char, 160> prefix{};
    const int length = sprintf_s(
        prefix.data(), prefix.size(),
        "%04u-%02u-%02uT%02u:%02u:%02u.%03u elapsed_ms=%llu pid=%lu tid=%lu event=",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
        time.wMilliseconds, static_cast<unsigned long long>(elapsed),
        GetCurrentProcessId(), GetCurrentThreadId());
    if (length <= 0) return {};
    std::string line(prefix.data(), static_cast<std::size_t>(length));
    line += ToUtf8(event);
    if (!details.empty()) {
        line.push_back(' ');
        line += ToUtf8(details);
    }
    line.push_back('\r');
    line.push_back('\n');
    return line;
}

bool WriteRaw(const std::string& line) {
    if (g_file == INVALID_HANDLE_VALUE || line.empty()) return false;
    DWORD written = 0;
    if (!WriteFile(g_file, line.data(), static_cast<DWORD>(line.size()), &written,
                   nullptr) || written != line.size()) {
        return false;
    }
    g_bytesWritten += written;
    FlushFileBuffers(g_file);
    return true;
}

void WriteLine(std::wstring_view event, std::wstring_view details) {
    std::string line = MakeLine(event, details);
    if (line.empty()) return;
    if (g_bytesWritten + line.size() <= kMaximumTraceBytes) {
        WriteRaw(line);
        return;
    }
    if (!g_limitReported) {
        g_limitReported = true;
        const std::string limitLine =
            MakeLine(L"trace_limit_reached", L"maximum_bytes=1048576");
        if (g_bytesWritten + limitLine.size() <= kMaximumTraceBytes) {
            WriteRaw(limitLine);
        }
    }
}

void CloseFile() {
    if (g_file == INVALID_HANDLE_VALUE) return;
    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
}

} // namespace

void Initialize(const std::filesystem::path& configPath) {
    std::lock_guard lock(g_mutex);
    CloseFile();
    const std::filesystem::path directory = configPath.parent_path();
    g_tracePath = directory / L"San9Toolkit.movie.log";
    g_previousPath = directory / L"San9Toolkit.movie.previous.log";
    g_bytesWritten = 0;
    g_startTick = 0;
    g_limitReported = false;
}

void BeginPlayback(std::wstring_view movieName) {
    std::lock_guard lock(g_mutex);
    CloseFile();
    if (g_tracePath.empty()) return;
    MoveFileExW(g_tracePath.c_str(), g_previousPath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    g_file = CreateFileW(g_tracePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                         nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) return;
    g_bytesWritten = 0;
    g_startTick = GetTickCount64();
    g_limitReported = false;
    std::wstring details = L"movie=";
    details.append(movieName);
    WriteLine(L"playback_begin", details);
}

void Record(std::wstring_view event, std::wstring_view details) {
    std::lock_guard lock(g_mutex);
    WriteLine(event, details);
}

void EndPlayback(std::wstring_view details) {
    std::lock_guard lock(g_mutex);
    WriteLine(L"playback_end", details);
    CloseFile();
}

} // namespace san9::movie_trace
