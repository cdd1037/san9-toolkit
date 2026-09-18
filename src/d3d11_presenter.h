#pragma once

#include <windows.h>
#include <cstdint>

namespace san9::d3d11_presenter {

struct Statistics {
    std::uint64_t presentedFrames = 0;
    std::uint64_t gameUploads = 0;
    std::uint64_t movieUploads = 0;
};

Statistics GetStatistics();

bool Initialize(HWND window);
bool QueueFrame(HDC framebufferDc);
bool PresentFrame(HDC framebufferDc);
bool PresentPendingFrame();
bool BeginMovie(HWND window, UINT width, UINT height);
bool PresentMovieFrame(const void* pixels, UINT rowPitch);
bool PresentCurrentFrame();
void EndMovie();
bool IsMovieActive();
void Shutdown();

} // namespace san9::d3d11_presenter
