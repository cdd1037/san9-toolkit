#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr std::array<unsigned char, 32> kSupportedSha256{
    0xD2,0x07,0x94,0xAE,0xFF,0x67,0x30,0x1E,0xC2,0xBF,0x8C,0x3B,0xEC,0xB1,0xE9,0x94,
    0x4C,0x68,0xC6,0xC0,0x58,0x8F,0xBF,0xD4,0xBF,0x04,0xE8,0x59,0x7F,0x0E,0x50,0x28};
constexpr char kWindowClass[] = "KOEI_SAN9WINDOW";
constexpr char kStatusProperty[] = "San9Toolkit.RuntimeStatus";

class Handle final {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { if (*this) CloseHandle(value_); }
    Handle(const Handle&) = delete; Handle& operator=(const Handle&) = delete;
    [[nodiscard]] HANDLE get() const { return value_; }
    [[nodiscard]] explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
private: HANDLE value_{};
};

bool HashFile(const std::filesystem::path& path, std::array<unsigned char, 32>& digest) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0, copied = 0; std::vector<unsigned char> object;
    std::array<unsigned char, 65536> buffer{}; bool ok = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
                          sizeof(objectLength), &copied, 0) < 0) goto cleanup;
    object.resize(objectLength);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) < 0) goto cleanup;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) goto cleanup;
        if (read == 0) break;
        if (BCryptHashData(hash, buffer.data(), read, 0) < 0) goto cleanup;
    }
    ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
cleanup:
    if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

std::wstring Quote(std::wstring_view value) {
    std::wstring result(1, L'\"'); unsigned slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'\"') { result.append(slashes * 2 + 1, L'\\'); result.push_back(ch); slashes = 0; continue; }
        result.append(slashes, L'\\'); slashes = 0; result.push_back(ch);
    }
    result.append(slashes * 2, L'\\'); result.push_back(L'\"'); return result;
}

bool ParseArguments(int argc, wchar_t** argv, std::filesystem::path& game,
                    std::filesystem::path& config, std::wstring& readyEvent) {
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::wstring_view name(argv[i]);
        if (name == L"--game") game = argv[i + 1];
        else if (name == L"--config") config = argv[i + 1];
        else if (name == L"--ready-event") readyEvent = argv[i + 1];
        else return false;
    }
    return argc == 7 && !game.empty() && !config.empty() && !readyEvent.empty();
}

bool ConfigureRemoteDpiAwareness(HANDLE process) {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const FARPROC proc = user32 ? GetProcAddress(user32, "SetProcessDpiAwarenessContext") : nullptr;
    if (!proc) return false;
    Handle thread(CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(proc),
                                     reinterpret_cast<void*>(static_cast<INT_PTR>(-2)), 0, nullptr));
    DWORD result = 0;
    return thread && WaitForSingleObject(thread.get(), 10000) == WAIT_OBJECT_0 &&
           GetExitCodeThread(thread.get(), &result) && result != 0;
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 10;
    std::filesystem::path game, config; std::wstring readyEventName;
    const bool parsed = ParseArguments(argc, argv, game, config, readyEventName); LocalFree(argv);
    if (!parsed || game.filename() != L"San9PK.exe" || !std::filesystem::is_regular_file(game)) return 11;
    std::array<unsigned char, 32> digest{};
    if (!HashFile(game, digest) || digest != kSupportedSha256) return 12;

    std::array<wchar_t, 32768> modulePath{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length >= modulePath.size()) return 13;
    const std::filesystem::path runtime = std::filesystem::path(modulePath.data()).parent_path() / L"San9Toolkit.Runtime.dll";
    if (!std::filesystem::is_regular_file(runtime)) return 14;
    Handle guiReady(OpenEventW(EVENT_MODIFY_STATE, FALSE, readyEventName.c_str()));
    const std::wstring bootName = L"Local\\San9Toolkit.Boot." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    Handle bootReady(CreateEventW(nullptr, TRUE, FALSE, bootName.c_str()));
    if (!guiReady || !bootReady || !SetEnvironmentVariableW(L"SAN9_TOOLKIT_BOOT_EVENT", bootName.c_str()) ||
        !SetEnvironmentVariableW(L"SAN9_TOOLKIT_CONFIG", config.c_str())) return 15;

    std::wstring commandLine = Quote(game.wstring());
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end()); mutableCommandLine.push_back(L'\0');
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    const std::wstring workingDirectory = game.parent_path().wstring();
    if (!CreateProcessW(game.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                        nullptr, workingDirectory.c_str(), &startup, &process)) return 16;
    Handle processHandle(process.hProcess), mainThread(process.hThread);
    if (!ConfigureRemoteDpiAwareness(processHandle.get())) { TerminateProcess(processHandle.get(), 1); return 17; }

    const std::wstring runtimePath = runtime.wstring(); const SIZE_T bytes = (runtimePath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(processHandle.get(), nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    SIZE_T written = 0;
    if (!remote || !WriteProcessMemory(processHandle.get(), remote, runtimePath.c_str(), bytes, &written) || written != bytes) {
        if (remote) VirtualFreeEx(processHandle.get(), remote, 0, MEM_RELEASE);
        TerminateProcess(processHandle.get(), 1); return 18;
    }
    const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    Handle injection(CreateRemoteThread(processHandle.get(), nullptr, 0, loadLibrary, remote, 0, nullptr));
    DWORD remoteModule = 0;
    const bool injected = injection && WaitForSingleObject(injection.get(), 10000) == WAIT_OBJECT_0 &&
                          GetExitCodeThread(injection.get(), &remoteModule) && remoteModule != 0;
    VirtualFreeEx(processHandle.get(), remote, 0, MEM_RELEASE);
    if (!injected || WaitForSingleObject(bootReady.get(), 10000) != WAIT_OBJECT_0) { TerminateProcess(processHandle.get(), 1); return 19; }
    if (ResumeThread(mainThread.get()) == static_cast<DWORD>(-1)) { TerminateProcess(processHandle.get(), 1); return 20; }

    int status = 0;
    for (int attempt = 0; attempt < 700; ++attempt) {
        if (WaitForSingleObject(processHandle.get(), 0) == WAIT_OBJECT_0) return 21;
        const HWND window = FindWindowA(kWindowClass, nullptr); DWORD pid = 0;
        if (window) GetWindowThreadProcessId(window, &pid);
        if (pid == process.dwProcessId) {
            status = static_cast<int>(reinterpret_cast<INT_PTR>(GetPropA(window, kStatusProperty)));
            if (status == 1 || status == 30) break;
        }
        Sleep(50);
    }
    if (status != 1) { TerminateProcess(processHandle.get(), 1); return status == 30 ? 22 : 23; }
    SetEvent(guiReady.get()); WaitForSingleObject(processHandle.get(), INFINITE); return 0;
}
