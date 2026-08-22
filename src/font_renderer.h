#pragma once

#include "font_frame.h"
#include "viewport.h"

#include <d3d11.h>

#include <cstdint>
#include <string_view>

namespace san9::font_renderer {

bool Initialize(std::wstring_view fontPath);
bool ResolveGlyph(std::uint32_t codePoint, std::uint16_t& glyphIndex);
bool BindTarget(ID3D11Texture2D* backBuffer);
void ReleaseTarget();
bool Draw(const font_frame::Frame& frame, const viewport::Bounds& bounds);
void Shutdown();

} // namespace san9::font_renderer
