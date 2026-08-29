#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <ui_core.h>

#include "documents_path.h"
#include "toolkit_config.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr UINT kHelperReadyMessage = WM_APP + 0x491;
constexpr UINT kHelperExitMessage = WM_APP + 0x492;
constexpr std::array<unsigned char, 32> kSupportedSha256{
    0xD2,0x07,0x94,0xAE,0xFF,0x67,0x30,0x1E,0xC2,0xBF,0x8C,0x3B,0xEC,0xB1,0xE9,0x94,
    0x4C,0x68,0xC6,0xC0,0x58,0x8F,0xBF,0xD4,0xBF,0x04,0xE8,0x59,0x7F,0x0E,0x50,0x28};

struct AppState {
    std::filesystem::path root;
    std::filesystem::path configPath;
    san9::toolkit_config::Settings settings;
    UiPage page{};
    UiWindow window{};
    UiWidget gamePath{}, documentsPath{}, title{}, hotkeyLabel{};
    UiWidget messageSpeed{}, gameReport{}, edgeScroll{}, playBgm{}, playSound{}, playMovie{};
    UiWidget dpiScale{}, borderless{}, speed{}, launch{};
    std::array<UiWidget, 4> navigation{};
    UiWidget pages{};
    bool capturingKey = false;
};

AppState g_app;
WNDPROC g_originalWindowProc = nullptr;

std::wstring Quote(std::wstring_view value) {
    std::wstring result(1, L'\"'); unsigned slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'\"') { result.append(slashes * 2 + 1, L'\\'); result.push_back(ch); slashes = 0; continue; }
        result.append(slashes, L'\\'); slashes = 0; result.push_back(ch);
    }
    result.append(slashes * 2, L'\\'); result.push_back(L'\"'); return result;
}

bool HashFile(const std::filesystem::path& path, std::array<unsigned char, 32>& digest) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
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
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) goto cleanup;
        if (read == 0) break;
        if (BCryptHashData(hash, buffer.data(), read, 0) < 0) goto cleanup;
    }
    ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
cleanup:
    if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file); return ok;
}

enum class GameValidationResult {
    Supported,
    InvalidFileName,
    ReadFailed,
    ContentMismatch,
};

GameValidationResult ValidateGame(const std::filesystem::path& path) {
    if (path.filename() != L"San9PK.exe") return GameValidationResult::InvalidFileName;
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return GameValidationResult::ReadFailed;
    std::array<unsigned char, 32> digest{};
    if (!HashFile(path, digest)) return GameValidationResult::ReadFailed;
    return digest == kSupportedSha256 ? GameValidationResult::Supported
                                      : GameValidationResult::ContentMismatch;
}

std::wstring KeyName(UINT key) {
    UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
    if (key == VK_LEFT || key == VK_UP || key == VK_RIGHT || key == VK_DOWN || key == VK_PRIOR ||
        key == VK_NEXT || key == VK_END || key == VK_HOME || key == VK_INSERT || key == VK_DELETE ||
        key == VK_DIVIDE || key == VK_NUMLOCK) scan |= 0x100;
    wchar_t name[64]{};
    return GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0 ? name : L"VK " + std::to_wstring(key);
}

void ShowError(const std::wstring& message) {
    const wchar_t* buttons[]{L"确定"};
    ui_msgbox(g_app.window, L"三国志 IX 工具箱", message.c_str(), buttons, 1, 0, 0, UI_MSGBOX_ICON_ERROR);
}

std::wstring DescribeLaunchFailure(DWORD code) {
    const std::wstring suffix = L"\n\n问题代码：" + std::to_wstring(code);
    switch (code) {
    case 10:
        return L"启动组件无法读取内部启动参数。请完整解压工具箱后重试。" + suffix;
    case 11:
        return L"启动组件找不到有效的 San9PK.exe。请在工具箱中重新选择游戏主程序。" + suffix;
    case 12:
        return L"游戏主程序内容与当前支持的繁体中文版 1.0.1.0 不一致。请不要使用修改版或损坏的 San9PK.exe。" + suffix;
    case 13:
        return L"启动组件无法确定自身所在目录。请把工具箱完整解压到本地磁盘后重试。" + suffix;
    case 14:
        return L"缺少 bin\\x86\\San9Toolkit.Runtime.dll。工具箱文件不完整，请重新解压完整程序包。" + suffix;
    case 15:
        return L"启动组件无法建立与 Runtime 的初始化通道。请重试；若问题持续，请检查安全软件是否限制了本地进程通信。" + suffix;
    case 16:
        return L"Windows 无法创建游戏进程。请检查 San9PK.exe 的访问权限以及安全软件拦截记录。" + suffix;
    case 17:
        return L"无法为游戏进程配置 DPI 模式。请检查 Windows 兼容性设置或进程防护策略。" + suffix;
    case 18:
        return L"无法把 Runtime 加载路径写入游戏进程。安全软件或 Windows Exploit Protection 可能阻止了进程操作。" + suffix;
    case 19:
        return L"游戏版本校验已通过，但 Runtime 未能注入、加载或在 10 秒内完成初始化。\n\n"
               L"请确认工具箱已完整解压，检查 Defender 或其他安全软件的拦截记录。若使用 Windows N 或精简版系统，请确认已安装 Media Feature Pack。反馈时请附上 Windows 版本、系统版本号和本问题代码。" + suffix;
    case 20:
        return L"Runtime 已完成初始化，但游戏主线程无法恢复运行。请检查安全软件或进程防护策略。" + suffix;
    case 21:
        return L"游戏在工具箱完成接管前已经退出。请检查游戏自身是否能够正常启动，以及安全软件的拦截记录。" + suffix;
    case 22:
        return L"Runtime 已加载，但无法安装游戏窗口处理。请恢复默认兼容性设置后重试。" + suffix;
    case 23:
        return L"Runtime 已加载，但未能在等待时间内确认游戏窗口和画面缓冲。请确认游戏使用窗口模式，并检查是否有其他窗口或分辨率补丁冲突。" + suffix;
    default:
        return L"游戏启动过程中发生未识别的错误。反馈时请附上 Windows 版本、系统版本号和本问题代码。" + suffix;
    }
}

void ShowGameValidationError(GameValidationResult result) {
    switch (result) {
    case GameValidationResult::InvalidFileName:
        ShowError(L"文件名不符合要求。请选择名称为 San9PK.exe 的游戏主程序。");
        break;
    case GameValidationResult::ContentMismatch:
        ShowError(L"文件内容校验不一致。当前仅支持繁体中文版 1.0.1.0 的原始 San9PK.exe。");
        break;
    case GameValidationResult::ReadFailed:
        ShowError(L"无法读取所选文件。请确认文件存在、未被其他程序占用，并检查访问权限后重试。");
        break;
    case GameValidationResult::Supported:
        break;
    }
}

std::filesystem::path SelectPath(bool folder) {
    IFileDialog* dialog = nullptr;
    const CLSID clsid = folder ? CLSID_FileOpenDialog : CLSID_FileOpenDialog;
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD options = 0; dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!folder) {
        const COMDLG_FILTERSPEC filters[]{{L"San9PK.exe", L"San9PK.exe"}, {L"可执行文件", L"*.exe"}};
        dialog->SetFileTypes(2, filters); dialog->SetFileName(L"San9PK.exe");
    }
    const HRESULT shown = dialog->Show(static_cast<HWND>(ui_window_hwnd(g_app.window)));
    IShellItem* item = nullptr; PWSTR selected = nullptr; std::filesystem::path result;
    if (SUCCEEDED(shown) && SUCCEEDED(dialog->GetResult(&item)) &&
        SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected))) result = selected;
    if (selected) CoTaskMemFree(selected); if (item) item->Release(); dialog->Release(); return result;
}

void PullControls() {
    g_app.settings.gameExecutable = ui_text_input_get_text(g_app.gamePath);
    g_app.settings.documentsRoot = ui_text_input_get_text(g_app.documentsPath);
    g_app.settings.windowTitle = ui_text_input_get_text(g_app.title);
    g_app.settings.messageSpeed = static_cast<DWORD>(ui_combobox_get_selected(g_app.messageSpeed));
    g_app.settings.gameReport = static_cast<DWORD>(ui_combobox_get_selected(g_app.gameReport));
    g_app.settings.edgeScrollMode = static_cast<san9::toolkit_config::EdgeScrollMode>(
        ui_combobox_get_selected(g_app.edgeScroll));
    g_app.settings.playBgm = ui_toggle_get_on(g_app.playBgm) != 0;
    g_app.settings.playSound = ui_toggle_get_on(g_app.playSound) != 0;
    g_app.settings.playMovie = ui_toggle_get_on(g_app.playMovie) != 0;
    g_app.settings.scaleInitialWindowForSystemDpi = ui_toggle_get_on(g_app.dpiScale) != 0;
    g_app.settings.borderlessFullscreen = ui_toggle_get_on(g_app.borderless) != 0;
    g_app.settings.gameSpeedMultiplier = static_cast<DWORD>(ui_combobox_get_selected(g_app.speed) + 1);
}

bool SaveSettings(bool toast) {
    PullControls();
    if (!san9::toolkit_config::Save(g_app.configPath, g_app.settings)) { ShowError(L"无法保存设置。请确认工具箱所在目录可以写入。"); return false; }
    if (toast) ui_toast(g_app.window, L"设置已保存", 1800);
    return true;
}

void OnBrowseGame(UiWidget, void*) {
    const auto selected = SelectPath(false); if (selected.empty()) return;
    const auto validation = ValidateGame(selected);
    if (validation != GameValidationResult::Supported) { ShowGameValidationError(validation); return; }
    g_app.settings.gameExecutable = selected.wstring(); ui_text_input_set_text(g_app.gamePath, selected.c_str()); SaveSettings(false);
}

void OnBrowseDocuments(UiWidget, void*) {
    const auto selected = SelectPath(true); if (selected.empty()) return;
    g_app.settings.documentsRoot = selected.wstring(); ui_text_input_set_text(g_app.documentsPath, selected.c_str()); SaveSettings(false);
}

void OnCaptureKey(UiWidget, void*) {
    g_app.capturingKey = true; ui_label_set_text(g_app.hotkeyLabel, L"请按下一个按键…");
}

void OnBorderlessFullscreenChanged(UiWidget, int enabled, void*) {
    ui_widget_set_enabled(g_app.dpiScale, enabled == 0);
}

void OnKey(UiWindow, int key, void*) {
    if (!g_app.capturingKey) return;
    if (key == VK_ESCAPE) { g_app.capturingKey = false; ui_label_set_text(g_app.hotkeyLabel, KeyName(g_app.settings.cursorLockVirtualKey).c_str()); return; }
    if (key == VK_BACK) key = VK_F12;
    if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || key == VK_LWIN || key == VK_RWIN) return;
    g_app.settings.cursorLockVirtualKey = static_cast<UINT>(key); g_app.capturingKey = false;
    ui_label_set_text(g_app.hotkeyLabel, KeyName(g_app.settings.cursorLockVirtualKey).c_str());
    if (key == VK_F8 || key == VK_F10) ui_toast(g_app.window, L"这个按键可能与游戏自带的快捷键冲突", 2600);
}

DWORD WINAPI MonitorHelper(void* parameter) {
    auto* handles = static_cast<HANDLE*>(parameter); HANDLE ready = handles[0], process = handles[1]; delete[] handles;
    HANDLE waitHandles[]{ready, process}; const DWORD first = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
    HWND hwnd = static_cast<HWND>(ui_window_hwnd(g_app.window));
    if (first == WAIT_OBJECT_0) { PostMessageW(hwnd, kHelperReadyMessage, 0, 0); WaitForSingleObject(process, INFINITE); }
    DWORD exitCode = 1; GetExitCodeProcess(process, &exitCode); CloseHandle(ready); CloseHandle(process);
    PostMessageW(hwnd, kHelperExitMessage, exitCode, 0); return 0;
}

void OnLaunch(UiWidget, void*) {
    if (!SaveSettings(false)) return;
    const std::filesystem::path game(g_app.settings.gameExecutable);
    const auto validation = ValidateGame(game);
    if (validation != GameValidationResult::Supported) { ShowGameValidationError(validation); return; }
    std::string encodedDocumentsRoot;
    if (!g_app.settings.documentsRoot.empty() &&
        !san9::documents_path::EncodeRoot(g_app.settings.documentsRoot, encodedDocumentsRoot)) {
        ShowError(L"文档根目录必须是当前 Windows 代码页可表示的绝对路径，并且不能超过游戏的路径长度限制。");
        return;
    }
    const auto bootstrap = g_app.root / L"bin" / L"x86" / L"San9Toolkit.Bootstrap.exe";
    if (!std::filesystem::is_regular_file(bootstrap)) { ShowError(L"工具箱文件不完整，请重新解压完整的程序包。"); return; }
    const std::wstring eventName = L"Local\\San9Toolkit.GuiReady." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    if (!ready) { ShowError(L"准备启动游戏时发生错误，请重试。"); return; }
    std::wstring command = Quote(bootstrap.wstring()) + L" --game " + Quote(game.wstring()) + L" --config " +
                           Quote(g_app.configPath.wstring()) + L" --ready-event " + Quote(eventName);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end()); mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(bootstrap.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, game.parent_path().c_str(), &startup, &process)) {
        CloseHandle(ready); ShowError(L"无法启动游戏。请确认工具箱文件完整，然后重试。"); return;
    }
    CloseHandle(process.hThread); ui_widget_set_enabled(g_app.launch, 0);
    HANDLE* handles = new HANDLE[2]{ready, process.hProcess};
    HANDLE monitor = CreateThread(nullptr, 0, MonitorHelper, handles, 0, nullptr);
    if (monitor) CloseHandle(monitor); else { delete[] handles; CloseHandle(ready); CloseHandle(process.hProcess); ui_widget_set_enabled(g_app.launch, 1); }
}

void OnSave(UiWidget, void*) { SaveSettings(true); }

void ShowSection(std::size_t selected) {
    if (selected >= g_app.navigation.size()) return;
    ui_stack_set_active_index(g_app.pages, static_cast<int>(selected));
    for (std::size_t index = 0; index < g_app.navigation.size(); ++index) {
        ui_nav_set_selected(g_app.navigation[index], index == selected ? 1 : 0);
    }
    if (g_app.window) ui_window_relayout(g_app.window);
}

void OnNavigate(UiWidget, void* userdata) {
    ShowSection(static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(userdata)));
}

LRESULT CALLBACK WindowSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kHelperReadyMessage) { ui_window_hide(g_app.window); return 0; }
    if (message == kHelperExitMessage) {
        g_app.settings = san9::toolkit_config::Load(g_app.configPath);
        ui_combobox_set_selected(g_app.messageSpeed, static_cast<int>(g_app.settings.messageSpeed));
        ui_combobox_set_selected(g_app.gameReport, static_cast<int>(g_app.settings.gameReport));
        ui_combobox_set_selected(g_app.edgeScroll,
                                 static_cast<int>(g_app.settings.edgeScrollMode));
        ui_toggle_set_on(g_app.playBgm, g_app.settings.playBgm); ui_toggle_set_on(g_app.playSound, g_app.settings.playSound);
        ui_toggle_set_on(g_app.playMovie, g_app.settings.playMovie); ui_widget_set_enabled(g_app.launch, 1);
        ui_window_show_immediate(g_app.window); SetForegroundWindow(hwnd);
        if (wParam != 0) ShowError(DescribeLaunchFailure(static_cast<DWORD>(wParam)));
        return 0;
    }
    return CallWindowProcW(g_originalWindowProc, hwnd, message, wParam, lParam);
}

UiPage LoadLayout() {
    return ui_page_load_file((g_app.root / L"ui" / L"app.uix").c_str());
}

UiWidget Widget(const char* id) {
    return ui_widget_find_by_id(ui_page_root(g_app.page), id);
}

bool BindLayout() {
    g_app.gamePath = Widget("gamePath");
    g_app.documentsPath = Widget("documentsPath");
    g_app.title = Widget("windowTitle");
    g_app.hotkeyLabel = Widget("hotkeyLabel");
    g_app.messageSpeed = Widget("messageSpeed");
    g_app.gameReport = Widget("gameReport");
    g_app.edgeScroll = Widget("edgeScroll");
    g_app.playBgm = Widget("playBgm");
    g_app.playSound = Widget("playSound");
    g_app.playMovie = Widget("playMovie");
    g_app.dpiScale = Widget("dpiScale");
    g_app.borderless = Widget("borderless");
    g_app.speed = Widget("gameSpeed");
    g_app.launch = Widget("launch");
    const UiWidget browseGame = Widget("browseGame");
    const UiWidget browseDocuments = Widget("browseDocuments");
    const UiWidget captureKey = Widget("captureKey");
    const UiWidget save = Widget("save");
    const UiWidget reportPlaceholder = Widget("reportDialogPlaceholder");
    const UiWidget jumpPlaceholder = Widget("jumpListPlaceholder");
    g_app.navigation = {Widget("navPath"), Widget("navTools"), Widget("navGame"), Widget("navAbout")};
    g_app.pages = Widget("pages");
    if (!g_app.gamePath || !g_app.documentsPath || !g_app.title || !g_app.hotkeyLabel ||
        !g_app.messageSpeed || !g_app.gameReport || !g_app.edgeScroll ||
        !g_app.playBgm || !g_app.playSound ||
        !g_app.playMovie || !g_app.dpiScale || !g_app.borderless || !g_app.speed ||
        !g_app.launch || !browseGame || !browseDocuments || !captureKey || !save ||
        !reportPlaceholder || !jumpPlaceholder ||
        !g_app.navigation[0] || !g_app.navigation[1] || !g_app.navigation[2] || !g_app.navigation[3] ||
        !g_app.pages) return false;
    ui_text_input_set_read_only(g_app.gamePath, 1);
    ui_text_input_set_read_only(g_app.documentsPath, 1);
    ui_widget_set_enabled(reportPlaceholder, 0);
    ui_widget_set_enabled(jumpPlaceholder, 0);
    ui_widget_on_click(browseGame, OnBrowseGame, nullptr);
    ui_widget_on_click(browseDocuments, OnBrowseDocuments, nullptr);
    ui_widget_on_click(captureKey, OnCaptureKey, nullptr);
    ui_widget_on_click(save, OnSave, nullptr);
    ui_widget_on_click(g_app.launch, OnLaunch, nullptr);
    ui_toggle_on_changed(g_app.borderless, OnBorderlessFullscreenChanged, nullptr);
    for (std::size_t index = 0; index < g_app.navigation.size(); ++index) {
        ui_widget_on_click(g_app.navigation[index], OnNavigate, reinterpret_cast<void*>(index));
    }
    ShowSection(0);
    return true;
}

void LoadDefaults() {
    g_app.settings = san9::toolkit_config::Load(g_app.configPath);
    if (g_app.settings.gameExecutable.empty()) {
        const auto candidate = g_app.root / L"San9PK.exe";
        if (ValidateGame(candidate) == GameValidationResult::Supported) g_app.settings.gameExecutable = candidate.wstring();
    }
    if (g_app.settings.documentsRoot.empty()) {
        PWSTR documents = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents))) {
            g_app.settings.documentsRoot = documents; CoTaskMemFree(documents);
        }
    }
}

void PopulateControls() {
    ui_text_input_set_text(g_app.gamePath, g_app.settings.gameExecutable.c_str());
    ui_text_input_set_text(g_app.documentsPath, g_app.settings.documentsRoot.c_str());
    ui_text_input_set_text(g_app.title, g_app.settings.windowTitle.c_str());
    ui_label_set_text(g_app.hotkeyLabel, KeyName(g_app.settings.cursorLockVirtualKey).c_str());
    ui_combobox_set_selected(g_app.messageSpeed, static_cast<int>(g_app.settings.messageSpeed));
    ui_combobox_set_selected(g_app.gameReport, static_cast<int>(g_app.settings.gameReport));
    ui_combobox_set_selected(g_app.edgeScroll,
                             static_cast<int>(g_app.settings.edgeScrollMode));
    ui_toggle_set_on_immediate(g_app.playBgm, g_app.settings.playBgm); ui_toggle_set_on_immediate(g_app.playSound, g_app.settings.playSound);
    ui_toggle_set_on_immediate(g_app.playMovie, g_app.settings.playMovie); ui_toggle_set_on_immediate(g_app.dpiScale, g_app.settings.scaleInitialWindowForSystemDpi);
    ui_toggle_set_on_immediate(g_app.borderless, g_app.settings.borderlessFullscreen);
    ui_widget_set_enabled(g_app.dpiScale, !g_app.settings.borderlessFullscreen);
    ui_combobox_set_selected(g_app.speed, static_cast<int>(g_app.settings.gameSpeedMultiplier - 1));
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::array<wchar_t, 32768> module{}; const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (length == 0 || length >= module.size()) return 1;
    g_app.root = std::filesystem::path(module.data()).parent_path(); g_app.configPath = g_app.root / L"data" / L"San9Toolkit.ini";
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    const DLL_DIRECTORY_COOKIE cookie = AddDllDirectory((g_app.root / L"bin" / L"x64").c_str());
    if (!cookie || FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) || ui_init_with_theme(UI_THEME_LIGHT) != 0) return 2;
    LoadDefaults();
    g_app.page = LoadLayout();
    if (!g_app.page || !BindLayout()) { ui_shutdown(); RemoveDllDirectory(cookie); CoUninitialize(); return 3; }
    ui_widget_set_expand(ui_page_root(g_app.page), 1);
    g_app.window = ui_page_prepare_window(g_app.page, nullptr);
    if (!g_app.window) { ui_page_destroy(g_app.page); ui_shutdown(); RemoveDllDirectory(cookie); CoUninitialize(); return 4; }
    PopulateControls(); ui_window_relayout(g_app.window);
    ui_window_on_key(g_app.window, OnKey, nullptr);
    HWND hwnd = static_cast<HWND>(ui_window_hwnd(g_app.window));
    g_originalWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WindowSubclass)));
    ui_window_show_immediate(g_app.window); const int result = ui_run();
    ui_page_destroy(g_app.page); ui_shutdown(); RemoveDllDirectory(cookie); CoUninitialize(); return result;
}
