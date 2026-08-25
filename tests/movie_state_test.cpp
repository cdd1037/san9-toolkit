#include "movie_state.h"
#include "movie_trace.h"
#include "toolkit_config.h"
#include "window_placement.h"

#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

LRESULT CALLBACK TestWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    const LRESULT result = DefWindowProcW(window, message, wParam, lParam);
    if (message == WM_GETMINMAXINFO) {
        san9::window_placement::HandleGetMinMaxInfo(
            window, *reinterpret_cast<MINMAXINFO*>(lParam));
    }
    return result;
}

void VerifyMaximumGeometry(const RECT& workArea, int expectedWidth,
                           int expectedHeight, int expectedLeft, int expectedTop) {
    san9::window_placement::MaximumGeometry geometry{};
    assert(san9::window_placement::CalculateMaximumFourByThree(
        workArea, 0, 0, geometry));
    assert(geometry.clientWidth == expectedWidth);
    assert(geometry.clientHeight == expectedHeight);
    assert(geometry.outer.left == expectedLeft);
    assert(geometry.outer.top == expectedTop);
    assert(geometry.outer.right == expectedLeft + expectedWidth);
    assert(geometry.outer.bottom == expectedTop + expectedHeight);
    assert(geometry.clientWidth * 3 == geometry.clientHeight * 4);
    assert(geometry.outer.left >= workArea.left);
    assert(geometry.outer.top >= workArea.top);
    assert(geometry.outer.right <= workArea.right);
    assert(geometry.outer.bottom <= workArea.bottom);
}

void VerifyMaximumGeometries() {
    VerifyMaximumGeometry({0, 0, 1920, 1080}, 1440, 1080, 240, 0);
    VerifyMaximumGeometry({0, 0, 1920, 1200}, 1600, 1200, 160, 0);
    VerifyMaximumGeometry({0, 0, 1600, 1200}, 1600, 1200, 0, 0);
    VerifyMaximumGeometry({0, 0, 3440, 1440}, 1920, 1440, 760, 0);
    VerifyMaximumGeometry({0, 0, 1080, 1920}, 1080, 810, 0, 1110);
    VerifyMaximumGeometry({-1920, 40, 0, 1080}, 1384, 1038, -1652, 42);

    san9::window_placement::MaximumGeometry framed{};
    assert(san9::window_placement::CalculateMaximumFourByThree(
        {100, 50, 2020, 1130}, 16, 39, framed));
    assert(framed.clientWidth == 1388);
    assert(framed.clientHeight == 1041);
    assert(framed.outer.left == 358);
    assert(framed.outer.top == 50);
    assert(framed.outer.right == 1762);
    assert(framed.outer.bottom == 1130);
    assert(!san9::window_placement::CalculateMaximumFourByThree(
        {0, 0, 3, 2}, 0, 0, framed));
}

void VerifyNativeWindowPlacement() {
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    assert(length != 0 && length < executable.size());
    const std::filesystem::path configPath =
        std::filesystem::path(executable.data()).parent_path() /
        (L"window-placement-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    assert(WritePrivateProfileStringW(L"WindowState", L"Placement",
                                      L"1|missing-monitor|20|30|640|0",
                                      configPath.c_str()));

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const std::wstring className =
        L"San9Toolkit.WindowPlacementTest." + std::to_wstring(GetCurrentProcessId());
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = TestWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className.c_str();
    assert(RegisterClassW(&windowClass) != 0);
    const HWND window = CreateWindowExW(0, className.c_str(), L"test",
                                        WS_OVERLAPPEDWINDOW, 10, 10, 200, 200,
                                        nullptr, nullptr, instance, nullptr);
    assert(window);

    bool shouldMaximize = true;
    assert(san9::window_placement::Restore(window, configPath, 1024, 768, false,
                                          shouldMaximize));
    assert(!shouldMaximize);
    RECT client{};
    assert(GetClientRect(window, &client));
    assert(client.right - client.left == 640);
    assert(client.bottom - client.top == 480);
    san9::window_placement::StartTracking(window);

    MINMAXINFO limits{};
    assert(san9::window_placement::HandleGetMinMaxInfo(window, limits));
    assert(limits.ptMaxSize.x > 0);
    assert(limits.ptMaxSize.y > 0);
    ShowWindow(window, SW_MAXIMIZE);
    assert(IsZoomed(window));
    assert(GetClientRect(window, &client));
    assert((client.right - client.left) * 3 == (client.bottom - client.top) * 4);
    ShowWindow(window, SW_RESTORE);
    assert(!IsZoomed(window));
    assert(GetClientRect(window, &client));
    assert(client.right - client.left == 640);
    assert(client.bottom - client.top == 480);
    san9::window_placement::HandleWindowMessageAfter(window, WM_EXITSIZEMOVE, 0);

    std::array<wchar_t, 1024> persisted{};
    assert(GetPrivateProfileStringW(L"WindowState", L"Placement", L"", persisted.data(),
                                    static_cast<DWORD>(persisted.size()),
                                    configPath.c_str()) != 0);
    san9::window_placement::State restored{};
    assert(san9::window_placement::TryParse(persisted.data(), restored));
    assert(restored.clientWidthDip == 640);

    san9::window_placement::Shutdown();
    assert(DestroyWindow(window));

    const std::wstring preserved = L"1|missing-monitor|20|30|640|1";
    assert(WritePrivateProfileStringW(L"WindowState", L"Placement", preserved.c_str(),
                                      configPath.c_str()));
    const HWND borderlessWindow = CreateWindowExW(0, className.c_str(), L"test",
                                                  WS_POPUP, 10, 10, 200, 200,
                                                  nullptr, nullptr, instance, nullptr);
    assert(borderlessWindow);
    shouldMaximize = true;
    assert(san9::window_placement::Restore(borderlessWindow, configPath, 1024, 768,
                                          true, shouldMaximize));
    assert(!shouldMaximize);
    assert(GetClientRect(borderlessWindow, &client));
    assert((client.right - client.left) * 3 == (client.bottom - client.top) * 4);
    san9::window_placement::StartTracking(borderlessWindow);
    assert(!san9::window_placement::HandleGetMinMaxInfo(borderlessWindow, limits));
    san9::window_placement::HandleWindowMessageAfter(borderlessWindow,
                                                     WM_EXITSIZEMOVE, 0);
    persisted.fill(L'\0');
    assert(GetPrivateProfileStringW(L"WindowState", L"Placement", L"", persisted.data(),
                                    static_cast<DWORD>(persisted.size()),
                                    configPath.c_str()) != 0);
    assert(std::wstring_view(persisted.data()) == preserved);
    san9::window_placement::Shutdown();
    assert(DestroyWindow(borderlessWindow));
    assert(UnregisterClassW(className.c_str(), instance));
    assert(DeleteFileW(configPath.c_str()));
}

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void VerifyMovieTrace() {
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    assert(length != 0 && length < executable.size());
    const std::filesystem::path directory =
        std::filesystem::path(executable.data()).parent_path();
    const std::filesystem::path configPath = directory / L"trace-test.ini";
    const std::filesystem::path tracePath = directory / L"San9Toolkit.movie.log";
    const std::filesystem::path previousPath =
        directory / L"San9Toolkit.movie.previous.log";
    DeleteFileW(tracePath.c_str());
    DeleteFileW(previousPath.c_str());

    san9::movie_trace::Initialize(configPath);
    assert(!std::filesystem::exists(tracePath));
    san9::movie_trace::BeginPlayback(L"first.avi");
    san9::movie_trace::Record(L"snapshot", L"decoded_video=12");
    san9::movie_trace::EndPlayback(L"result=0x00000000 stopped=0");
    const std::string first = ReadTextFile(tracePath);
    assert(first.find("event=playback_begin movie=first.avi") != std::string::npos);
    assert(first.find("event=snapshot decoded_video=12") != std::string::npos);
    assert(first.find("event=playback_end result=0x00000000") != std::string::npos);

    san9::movie_trace::BeginPlayback(L"second.avi");
    san9::movie_trace::EndPlayback(L"result=0x00000000 stopped=0");
    assert(ReadTextFile(previousPath) == first);
    assert(ReadTextFile(tracePath).find("event=playback_begin movie=second.avi") !=
           std::string::npos);
    assert(DeleteFileW(tracePath.c_str()));
    assert(DeleteFileW(previousPath.c_str()));
}

} // namespace

int main() {
    san9::movie_state::PlaybackState state;
    assert(state.IsFinished());
    assert(!state.IsPlaying());
    assert(state.Begin());
    assert(!state.Begin());
    assert(state.IsPlaying());
    assert(!state.IsFinished());
    state.PauseByApi(true);
    assert(state.IsPaused());
    state.PauseByWindow(true);
    state.PauseByApi(false);
    assert(state.IsPaused());
    state.PauseByWindow(false);
    assert(!state.IsPaused());
    state.RequestStop();
    assert(state.IsStopRequested());
    state.Finish();
    assert(state.IsFinished());
    assert(state.Begin());
    assert(!state.IsStopRequested());
    state.Finish();

    using namespace san9::movie_state;
    assert(!ReadyToStart(false, kPrebufferDuration, false));
    assert(!ReadyToStart(true, kPrebufferDuration - 1, false));
    assert(ReadyToStart(true, kPrebufferDuration, false));
    assert(ReadyToStart(true, 1, true));

    constexpr std::array<std::int64_t, 4> timestamps{0, 333'333, 666'666, 999'999};
    assert(LateFramesToDrop(timestamps, 0) == 0);
    assert(LateFramesToDrop(timestamps, 800'000) == 2);
    assert(LateFramesToDrop(timestamps, 20'000'000) == 3);
    assert(!PlaybackComplete(false, true, 0, 0));
    assert(!PlaybackComplete(true, false, 0, 0));
    assert(!PlaybackComplete(true, true, 1, 0));
    assert(!PlaybackComplete(true, true, 0, 1));
    assert(PlaybackComplete(true, true, 0, 0));
    assert(!AudioUnderrun(false, false, 0));
    assert(!AudioUnderrun(true, true, 0));
    assert(!AudioUnderrun(true, false, 1));
    assert(AudioUnderrun(true, false, 0));
    assert(!ReadyAfterUnderrun(kPrebufferDuration, 0, false));
    assert(!ReadyAfterUnderrun(kPrebufferDuration - 1, 1, false));
    assert(ReadyAfterUnderrun(kPrebufferDuration, 1, false));
    assert(ReadyAfterUnderrun(1, 1, true));

    using san9::movie_state::DecodeStream;
    assert(SelectDecodeStream(false, 45, 45, false, 0, 15'000'000,
                              DecodeStream::Video) ==
           DecodeStream::Audio);
    assert(SelectDecodeStream(false, 0, 45, false, 15'000'000, 15'000'000,
                              DecodeStream::Audio) ==
           DecodeStream::Video);
    assert(SelectDecodeStream(false, 45, 45, false, 15'000'000, 15'000'000,
                              DecodeStream::Audio) ==
           DecodeStream::None);
    assert(SelectDecodeStream(true, 0, 45, false, 0, 15'000'000,
                              DecodeStream::Video) ==
           DecodeStream::Audio);
    assert(SelectDecodeStream(false, 0, 45, true, 0, 15'000'000,
                              DecodeStream::Audio) ==
           DecodeStream::Video);
    assert(SelectDecodeStream(true, 0, 45, true, 0, 15'000'000,
                              DecodeStream::Audio) ==
           DecodeStream::None);
    assert(SelectDecodeStream(false, 0, 45, false, 0, 15'000'000,
                              DecodeStream::Audio) ==
           DecodeStream::Audio);
    assert(SelectDecodeStream(false, 0, 45, false, 0, 15'000'000,
                              DecodeStream::Video) ==
           DecodeStream::Video);

    using san9::toolkit_config::EdgeScrollMode;
    using san9::toolkit_config::GetEdgeScrollMode;
    using san9::toolkit_config::SetEdgeScrollMode;
    constexpr DWORD unrelatedFlags = 0xA5A5001F;
    const DWORD hoverFlags = SetEdgeScrollMode(unrelatedFlags, EdgeScrollMode::Hover);
    assert(GetEdgeScrollMode(hoverFlags) == EdgeScrollMode::Hover);
    assert((hoverFlags & ~san9::toolkit_config::kHoverEdgeScrollFlag) ==
           (unrelatedFlags & ~san9::toolkit_config::kHoverEdgeScrollFlag));
    const DWORD holdFlags = SetEdgeScrollMode(hoverFlags, EdgeScrollMode::HoldLeftButton);
    assert(GetEdgeScrollMode(holdFlags) == EdgeScrollMode::HoldLeftButton);
    assert((holdFlags & ~san9::toolkit_config::kHoverEdgeScrollFlag) ==
           (unrelatedFlags & ~san9::toolkit_config::kHoverEdgeScrollFlag));

    const san9::window_placement::State placement{
        L"\\\\.\\DISPLAY2", -120, 75, 1280, true};
    const std::wstring serialized = san9::window_placement::Serialize(placement);
    assert(serialized == L"1|\\\\.\\DISPLAY2|-120|75|1280|1");
    san9::window_placement::State restored{};
    assert(san9::window_placement::TryParse(serialized, restored));
    assert(restored.monitor == placement.monitor);
    assert(restored.xDip == placement.xDip);
    assert(restored.yDip == placement.yDip);
    assert(restored.clientWidthDip == placement.clientWidthDip);
    assert(restored.maximized == placement.maximized);
    assert(!san9::window_placement::TryParse(L"2|\\\\.\\DISPLAY1|0|0|1024|0", restored));
    assert(!san9::window_placement::TryParse(L"1|\\\\.\\DISPLAY1|0|0|0|0", restored));
    assert(!san9::window_placement::TryParse(L"1|\\\\.\\DISPLAY1|0|0|1024|2", restored));
    assert(san9::window_placement::Serialize({L"bad|monitor", 0, 0, 1024, false}).empty());
    VerifyMaximumGeometries();
    VerifyNativeWindowPlacement();
    VerifyMovieTrace();
    return 0;
}
