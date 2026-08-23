#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::array<unsigned char, 32> kSupportedSha256{
    0xD2, 0x07, 0x94, 0xAE, 0xFF, 0x67, 0x30, 0x1E,
    0xC2, 0xBF, 0x8C, 0x3B, 0xEC, 0xB1, 0xE9, 0x94,
    0x4C, 0x68, 0xC6, 0xC0, 0x58, 0x8F, 0xBF, 0xD4,
    0xBF, 0x04, 0xE8, 0x59, 0x7F, 0x0E, 0x50, 0x28,
};
constexpr char kWindowClass[] = "KOEI_SAN9WINDOW";
constexpr char kStatusProperty[] = "San9Toolkit.RuntimeStatus";
constexpr wchar_t kBootEventEnvironmentVariable[] = L"SAN9_TOOLKIT_BOOT_EVENT";
constexpr wchar_t kConfigEnvironmentVariable[] = L"SAN9_TOOLKIT_CONFIG";
constexpr std::array<char, 16> kPayloadMagic{
    'S', 'A', 'N', '9', 'T', 'O', 'O', 'L', 'K', 'I', 'T', 'D', 'L', 'L', '1', '\0',
};

struct PayloadFooter {
    std::array<char, 16> magic;
    std::uint32_t payloadSize;
    std::array<unsigned char, 32> payloadSha256;
};

static_assert(sizeof(PayloadFooter) == 52);

class Handle final {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const { return value_; }
    [[nodiscard]] explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE release() { return std::exchange(value_, nullptr); }
    void reset(HANDLE value = nullptr) {
        if (*this) {
            CloseHandle(value_);
        }
        value_ = value;
    }
private:
    HANDLE value_{};
};

std::wstring FormatError(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message = length ? std::wstring(buffer, length) : L"未知错误";
    if (buffer) {
        LocalFree(buffer);
    }
    return message;
}

bool HashFile(const std::filesystem::path& path, std::array<unsigned char, 32>& digest) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file) {
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD copied = 0;
    std::vector<unsigned char> object;
    std::array<unsigned char, 64 * 1024> buffer{};
    bool ok = false;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &copied, 0) < 0) {
        goto cleanup;
    }
    object.resize(objectLength);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) < 0) {
        goto cleanup;
    }

    for (;;) {
        DWORD bytesRead = 0;
        if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr)) {
            goto cleanup;
        }
        if (bytesRead == 0) {
            break;
        }
        if (BCryptHashData(hash, buffer.data(), bytesRead, 0) < 0) {
            goto cleanup;
        }
    }
    ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;

cleanup:
    if (hash) {
        BCryptDestroyHash(hash);
    }
    if (algorithm) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return ok;
}

bool HashBytes(const std::vector<unsigned char>& bytes, std::array<unsigned char, 32>& digest) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD copied = 0;
    std::vector<unsigned char> object;
    bool ok = false;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &copied, 0) < 0) {
        goto cleanup;
    }
    object.resize(objectLength);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) < 0 ||
        BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) < 0) {
        goto cleanup;
    }
    ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;

cleanup:
    if (hash) {
        BCryptDestroyHash(hash);
    }
    if (algorithm) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return ok;
}

bool ReadExact(HANDLE file, void* buffer, DWORD size) {
    DWORD bytesRead = 0;
    return ReadFile(file, buffer, size, &bytesRead, nullptr) && bytesRead == size;
}

bool ReadEmbeddedPayload(const std::filesystem::path& launcher,
                         std::vector<unsigned char>& payload,
                         std::array<unsigned char, 32>& digest) {
    Handle file(CreateFileW(launcher.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    LARGE_INTEGER fileSize{};
    if (!file || !GetFileSizeEx(file.get(), &fileSize) ||
        fileSize.QuadPart < static_cast<LONGLONG>(sizeof(PayloadFooter))) {
        return false;
    }

    LARGE_INTEGER footerOffset{};
    footerOffset.QuadPart = fileSize.QuadPart - sizeof(PayloadFooter);
    if (!SetFilePointerEx(file.get(), footerOffset, nullptr, FILE_BEGIN)) {
        return false;
    }
    PayloadFooter footer{};
    if (!ReadExact(file.get(), &footer, static_cast<DWORD>(sizeof(footer))) || footer.magic != kPayloadMagic ||
        footer.payloadSize == 0 || footer.payloadSize > 16 * 1024 * 1024 ||
        fileSize.QuadPart < static_cast<LONGLONG>(sizeof(PayloadFooter) + footer.payloadSize)) {
        return false;
    }

    LARGE_INTEGER payloadOffset{};
    payloadOffset.QuadPart = fileSize.QuadPart - sizeof(PayloadFooter) - footer.payloadSize;
    if (!SetFilePointerEx(file.get(), payloadOffset, nullptr, FILE_BEGIN)) {
        return false;
    }
    payload.resize(footer.payloadSize);
    if (!ReadExact(file.get(), payload.data(), footer.payloadSize) || !HashBytes(payload, digest)) {
        return false;
    }
    return digest == footer.payloadSha256;
}

bool ReadPayloadFile(const std::filesystem::path& path,
                     std::vector<unsigned char>& payload,
                     std::array<unsigned char, 32>& digest) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    LARGE_INTEGER fileSize{};
    if (!file || !GetFileSizeEx(file.get(), &fileSize) || fileSize.QuadPart <= 0 ||
        fileSize.QuadPart > 16 * 1024 * 1024) {
        return false;
    }
    payload.resize(static_cast<std::size_t>(fileSize.QuadPart));
    return ReadExact(file.get(), payload.data(), static_cast<DWORD>(payload.size())) &&
           HashBytes(payload, digest);
}

std::wstring DigestPrefix(const std::array<unsigned char, 32>& digest) {
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(16);
    for (std::size_t index = 0; index < 8; ++index) {
        result.push_back(hex[digest[index] >> 4]);
        result.push_back(hex[digest[index] & 0x0F]);
    }
    return result;
}

bool ExtractPayload(const std::vector<unsigned char>& payload,
                    const std::array<unsigned char, 32>& digest,
                    std::filesystem::path& dllPath) {
    std::error_code error;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path(error) / L"San9Toolkit";
    if (error || (!std::filesystem::create_directories(directory, error) && error)) {
        return false;
    }
    dllPath = directory / (L"San9Toolkit.Runtime-" + DigestPrefix(digest) + L".dll");

    std::array<unsigned char, 32> existingDigest{};
    if (std::filesystem::is_regular_file(dllPath, error) && !error &&
        HashFile(dllPath, existingDigest) && existingDigest == digest) {
        return true;
    }

    Handle output(CreateFileW(dllPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr));
    if (!output) {
        return false;
    }
    DWORD bytesWritten = 0;
    const bool written = WriteFile(output.get(), payload.data(), static_cast<DWORD>(payload.size()),
                                   &bytesWritten, nullptr) &&
                         bytesWritten == static_cast<DWORD>(payload.size()) && FlushFileBuffers(output.get());
    output.reset();
    if (!written) {
        DeleteFileW(dllPath.c_str());
    }
    return written;
}

std::wstring QuoteArgument(std::wstring_view argument) {
    if (argument.find_first_of(L" \t\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }
    std::wstring result(1, L'"');
    unsigned backslashes = 0;
    for (const wchar_t value : argument) {
        if (value == L'\\') {
            ++backslashes;
            continue;
        }
        if (value == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(value);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

int Fail(const std::wstring& message) {
    std::wcerr << L"San9 Toolkit: " << message << L'\n';
    return 1;
}

int ConfigureRemoteDpiAwareness(HANDLE process) {
    const HMODULE localUser32 = GetModuleHandleW(L"user32.dll");
    const FARPROC localSetContext =
        localUser32 ? GetProcAddress(localUser32, "SetProcessDpiAwarenessContext") : nullptr;
    if (!localSetContext) {
        return 1;
    }

    const auto remoteSetContext = reinterpret_cast<LPTHREAD_START_ROUTINE>(localSetContext);
    Handle thread(CreateRemoteThread(process, nullptr, 0, remoteSetContext,
                                     reinterpret_cast<void*>(static_cast<INT_PTR>(-2)), 0, nullptr));
    if (!thread || WaitForSingleObject(thread.get(), 10'000) != WAIT_OBJECT_0) {
        return 4;
    }
    DWORD result = 0;
    if (!GetExitCodeThread(thread.get(), &result)) {
        return 5;
    }
    return result != 0 ? 0 : 6;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    std::array<wchar_t, MAX_PATH> launcherPathBuffer{};
    const DWORD launcherLength = GetModuleFileNameW(
        nullptr, launcherPathBuffer.data(), static_cast<DWORD>(launcherPathBuffer.size()));
    if (launcherLength == 0 || launcherLength == launcherPathBuffer.size()) {
        return Fail(L"无法确定启动器路径。" );
    }
    const std::filesystem::path launcherPath(launcherPathBuffer.data());
    std::error_code filesystemError;
    const std::filesystem::path requestedExecutable =
        argc >= 2 ? std::filesystem::path(argv[1]) : launcherPath.parent_path() / L"San9PK.exe";
    const std::filesystem::path executable =
        std::filesystem::weakly_canonical(requestedExecutable, filesystemError);
    if (filesystemError || !std::filesystem::is_regular_file(executable)) {
        return Fail(argc >= 2 ? L"指定的目标 EXE 不存在。"
                              : L"未指定目标，且启动器同目录下没有 San9PK.exe。" );
    }

    std::array<unsigned char, 32> digest{};
    if (!HashFile(executable, digest)) {
        return Fail(L"无法计算目标 EXE 的 SHA-256。" );
    }
    if (digest != kSupportedSha256) {
        return Fail(L"目标 EXE 不是受支持的 SRC-SAN9PK-TC-101；未启动进程。" );
    }

    std::vector<unsigned char> payload;
    std::array<unsigned char, 32> payloadDigest{};
    std::filesystem::path dll;
    if (ReadEmbeddedPayload(launcherPath, payload, payloadDigest)) {
        if (!ExtractPayload(payload, payloadDigest, dll)) {
            return Fail(L"无法把内嵌 DLL 释放到临时目录。" );
        }
    } else {
        const std::filesystem::path siblingDll =
            launcherPath.parent_path() / L"San9Toolkit.Runtime.dll";
        if (!std::filesystem::is_regular_file(siblingDll, filesystemError) || filesystemError) {
            return Fail(L"启动器没有内嵌 DLL，且同目录下没有 San9Toolkit.Runtime.dll。" );
        }
        if (!ReadPayloadFile(siblingDll, payload, payloadDigest)) {
            return Fail(L"无法读取或校验同目录的 San9Toolkit.Runtime.dll。" );
        }
        if (!ExtractPayload(payload, payloadDigest, dll)) {
            return Fail(L"无法把同目录 DLL 缓存到临时目录。" );
        }
    }

    std::wstring commandLine = QuoteArgument(executable.wstring());
    for (int index = argc >= 2 ? 2 : 1; index < argc; ++index) {
        commandLine.push_back(L' ');
        commandLine += QuoteArgument(argv[index]);
    }
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    const std::wstring bootEventName =
        L"Local\\San9Toolkit.Boot." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64());
    Handle bootEvent(CreateEventW(nullptr, TRUE, FALSE, bootEventName.c_str()));
    const std::wstring configPath =
        (launcherPath.parent_path() / L"San9Toolkit.ini").wstring();
    if (!bootEvent || GetLastError() == ERROR_ALREADY_EXISTS ||
        !SetEnvironmentVariableW(kBootEventEnvironmentVariable, bootEventName.c_str()) ||
        !SetEnvironmentVariableW(kConfigEnvironmentVariable, configPath.c_str())) {
        return Fail(L"无法创建 Runtime 安装握手事件。" );
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring workingDirectory = executable.parent_path().wstring();
    if (!CreateProcessW(executable.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, workingDirectory.c_str(), &startup, &process)) {
        return Fail(L"创建挂起进程失败：" + FormatError(GetLastError()));
    }
    Handle processHandle(process.hProcess);
    Handle mainThread(process.hThread);
    const int dpiSetup = ConfigureRemoteDpiAwareness(processHandle.get());
    if (dpiSetup != 0) {
        TerminateProcess(processHandle.get(), 1);
        return Fail(L"[E-DPI-" + std::to_wstring(dpiSetup) +
                    L"] 无法在窗口创建前启用目标进程 DPI 感知。" );
    }
    const std::wstring dllPath = dll.wstring();
    const SIZE_T allocationSize = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(processHandle.get(), nullptr, allocationSize,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        TerminateProcess(processHandle.get(), 1);
        return Fail(L"无法在目标进程分配 DLL 路径。" );
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(processHandle.get(), remotePath, dllPath.c_str(), allocationSize, &written) ||
        written != allocationSize) {
        VirtualFreeEx(processHandle.get(), remotePath, 0, MEM_RELEASE);
        TerminateProcess(processHandle.get(), 1);
        return Fail(L"无法写入目标进程的 DLL 路径。" );
    }

    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));
    Handle injectionThread(CreateRemoteThread(processHandle.get(), nullptr, 0, loadLibrary, remotePath, 0, nullptr));
    if (!injectionThread) {
        VirtualFreeEx(processHandle.get(), remotePath, 0, MEM_RELEASE);
        TerminateProcess(processHandle.get(), 1);
        return Fail(L"创建注入线程失败：" + FormatError(GetLastError()));
    }

    const DWORD waitResult = WaitForSingleObject(injectionThread.get(), 10'000);
    DWORD remoteModule = 0;
    const bool injected = waitResult == WAIT_OBJECT_0 &&
                          GetExitCodeThread(injectionThread.get(), &remoteModule) && remoteModule != 0;
    VirtualFreeEx(processHandle.get(), remotePath, 0, MEM_RELEASE);
    if (!injected) {
        TerminateProcess(processHandle.get(), 1);
        return Fail(L"DLL 注入失败；已终止仍处于挂起状态的目标进程。" );
    }

    if (WaitForSingleObject(bootEvent.get(), 10'000) != WAIT_OBJECT_0) {
        TerminateProcess(processHandle.get(), 1);
        return Fail(L"Runtime 未能在游戏主线程恢复前完成钩子安装。" );
    }

    if (ResumeThread(mainThread.get()) == static_cast<DWORD>(-1)) {
        TerminateProcess(processHandle.get(), 1);
        return Fail(L"恢复游戏主线程失败。" );
    }

    int installStatus = 0;
    for (int attempt = 0; attempt < 700; ++attempt) {
        DWORD exitCode = STILL_ACTIVE;
        if (!GetExitCodeProcess(processHandle.get(), &exitCode) || exitCode != STILL_ACTIVE) {
            return Fail(L"游戏在缩放 DLL 完成安装前退出。" );
        }
        const HWND window = FindWindowA(kWindowClass, nullptr);
        if (window) {
            DWORD windowProcessId = 0;
            GetWindowThreadProcessId(window, &windowProcessId);
            if (windowProcessId == process.dwProcessId) {
                installStatus = static_cast<int>(reinterpret_cast<INT_PTR>(
                    GetPropA(window, kStatusProperty)));
                if (installStatus == 1 || installStatus == 30) {
                    break;
                }
            }
        }
        Sleep(50);
    }
    if (installStatus != 1) {
        TerminateProcess(processHandle.get(), 1);
        if (installStatus == 30) {
            return Fail(L"DLL 已挂钩绘制入口，但主窗口改造失败。" );
        }
        return Fail(L"等待 DLL 安装完成超时；已终止本次创建的游戏进程。" );
    }
    std::wcout << L"San9 Toolkit 已注入，游戏进程已恢复。\n";
    return 0;
}
