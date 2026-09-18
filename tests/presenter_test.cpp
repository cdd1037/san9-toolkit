#include "d3d11_presenter.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
namespace presenter = san9::d3d11_presenter;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "Presenter test failed: %s\n", message);
        std::exit(1);
    }
}

void PumpFor(unsigned int milliseconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(1);
    } while (std::chrono::steady_clock::now() < end);
}
} // namespace

int main() {
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"San9ToolkitPresenterTest";
    Require(RegisterClassW(&windowClass) != 0, "register window");
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE, windowClass.lpszClassName,
                                  L"Toolkit renderer test", WS_OVERLAPPEDWINDOW,
                                  0, 0, 400, 320, nullptr, nullptr, instance, nullptr);
    Require(window != nullptr, "create window");
    ShowWindow(window, SW_SHOWNOACTIVATE);
    Require(presenter::Initialize(window), "initialize hardware Flip Model renderer");

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 1024;
    info.bmiHeader.biHeight = 768;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 16;
    void* pixels = nullptr;
    const HDC dc = CreateCompatibleDC(nullptr);
    const HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    Require(dc && bitmap && pixels, "create synthetic RGB555 game buffer");
    const auto previous = SelectObject(dc, bitmap);
    auto* colors = static_cast<std::uint16_t*>(pixels);
    std::fill_n(colors, 1024 * 768, static_cast<std::uint16_t>(0x03E0));

    Require(presenter::QueueFrame(dc) && presenter::PresentPendingFrame(), "initial frame");
    PumpFor(150);
    auto stats = presenter::GetStatistics();
    Require(stats.presentedFrames == 1 && stats.gameUploads == 1, "initial upload and present");

    const auto start = std::chrono::steady_clock::now();
    std::uint64_t requests = 0;
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500)) {
        for (int burst = 0; burst < 32; ++burst) {
            colors[0] = static_cast<std::uint16_t>(requests);
            Require(presenter::QueueFrame(dc) && presenter::PresentPendingFrame(), "input burst");
            ++requests;
        }
        PumpFor(1);
    }
    const auto beforeTail = presenter::GetStatistics();
    colors[0] = 0x7C00;
    Require(presenter::QueueFrame(dc) && presenter::PresentPendingFrame(), "final input");
    PumpFor(150);
    stats = presenter::GetStatistics();
    Require(stats.gameUploads > beforeTail.gameUploads, "deferred final update is not lost");
    Require(stats.presentedFrames <= 33 && stats.gameUploads < requests, "burst coalescing and 60 Hz cap");
    const auto afterBurst = stats;
    PumpFor(100);
    Require(presenter::GetStatistics().presentedFrames == stats.presentedFrames, "idle stops presenting");

    SetWindowPos(window, nullptr, 0, 0, 700, 350, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    Require(presenter::PresentCurrentFrame(), "resize repaint");
    PumpFor(100);
    stats = presenter::GetStatistics();
    Require(stats.presentedFrames > afterBurst.presentedFrames &&
            stats.gameUploads == afterBurst.gameUploads, "resize reuses uploaded texture");

    ShowWindow(window, SW_HIDE);
    const auto beforeHidden = stats;
    for (int i = 0; i < 100; ++i) {
        Require(presenter::QueueFrame(dc) && presenter::PresentPendingFrame(), "hidden update");
    }
    PumpFor(100);
    stats = presenter::GetStatistics();
    Require(stats.gameUploads == beforeHidden.gameUploads &&
            stats.presentedFrames == beforeHidden.presentedFrames, "hidden window has no GPU submissions");
    ShowWindow(window, SW_SHOWNOACTIVATE);
    Require(presenter::PresentCurrentFrame(), "restore");
    PumpFor(100);
    Require(presenter::GetStatistics().gameUploads == stats.gameUploads + 1, "restore retains latest update");

    ShowWindow(window, SW_SHOWMINNOACTIVE);
    stats = presenter::GetStatistics();
    Require(presenter::QueueFrame(dc) && presenter::PresentPendingFrame(), "minimized update");
    PumpFor(100);
    Require(presenter::GetStatistics().gameUploads == stats.gameUploads, "minimized window skips uploads");
    ShowWindow(window, SW_SHOWNOACTIVATE);
    Require(presenter::PresentCurrentFrame(), "restore from minimized");
    PumpFor(100);
    Require(presenter::GetStatistics().gameUploads == stats.gameUploads + 1, "minimized update survives restore");

    Require(presenter::BeginMovie(window, 320, 240), "begin synthetic movie");
    std::vector<std::uint32_t> movie(320 * 240, 0xFF0000FF);
    Require(presenter::PresentMovieFrame(movie.data(), 320 * 4), "movie frame");
    PumpFor(100);
    stats = presenter::GetStatistics();
    Require(stats.movieUploads == 1, "movie upload");
    Require(presenter::PresentCurrentFrame(), "movie repaint");
    PumpFor(100);
    Require(presenter::GetStatistics().movieUploads == 1, "movie repaint reuses texture");
    presenter::EndMovie();
    PumpFor(100);
    Require(!presenter::IsMovieActive(), "game restored after movie");
    Require(presenter::GetStatistics().gameUploads == stats.gameUploads + 1, "game frame after movie");

    std::printf("Presenter integration passed: %llu input requests, %llu burst presents, %llu uploads.\n",
                static_cast<unsigned long long>(requests),
                static_cast<unsigned long long>(afterBurst.presentedFrames - 1),
                static_cast<unsigned long long>(afterBurst.gameUploads - 1));
    presenter::Shutdown();
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    DestroyWindow(window);
    UnregisterClassW(windowClass.lpszClassName, instance);
    return 0;
}
