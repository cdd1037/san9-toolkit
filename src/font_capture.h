#pragma once

#include "font_frame.h"

namespace san9::font_capture {

bool InstallHooks();
font_frame::Frame PrepareFrame(void* framebufferPixels, int width, int height);
void Shutdown();

} // namespace san9::font_capture
