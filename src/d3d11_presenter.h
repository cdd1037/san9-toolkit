#pragma once

#include <windows.h>

namespace san9::d3d11_presenter {

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
