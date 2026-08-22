#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

namespace san9::font_frame {

struct Glyph {
    std::uint16_t glyphIndex{};
    std::uint16_t styleFlags{};
    std::uint32_t mainColor{};
    std::uint32_t effectColor{};
    float x{};
    float y{};
    float cellSize{};
    RECT clip{};
    RECT bounds{};
    std::vector<RECT> occlusion;
};

struct PixelRun {
    std::uint32_t storageRow{};
    std::uint16_t x{};
    std::vector<std::uint16_t> pixels;
};

struct Frame {
    std::vector<PixelRun> cleanPixels;
    std::vector<Glyph> glyphs;
};

} // namespace san9::font_frame
