#include "font_capture.h"

#include "code_hook.h"
#include "font_renderer.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace san9::font_capture {
namespace {

constexpr std::uintptr_t kRenderFontGlyphsRva = 0x1BACC0;
constexpr std::uintptr_t kBlitSurfaceRva = 0x1C25F0;
constexpr std::uintptr_t kBlitBitmapRva = 0x1BD650;
constexpr std::uintptr_t kDestroySurfaceRva = 0x1BE310;
constexpr std::uintptr_t kLayoutListsRva = 0x1660F88;
constexpr std::uintptr_t kGlyphCacheRva = 0x165F788;
constexpr std::uintptr_t kFontPalettePointerRva = 0x16A1950;
constexpr std::size_t kLayoutListCount = 0x270;
constexpr std::size_t kGlyphCacheCount = 1024;
constexpr std::size_t kMaximumGlyphsPerDraw = 4096;

#pragma pack(push, 1)
struct GameSurface {
    void* vtable;
    void* pixels;
    std::uint32_t ownsPixels;
    void* bitmapInfo;
    std::array<std::uint8_t, 8> reserved10;
    std::int32_t topDown;
    std::uint16_t generation;
    std::int16_t width;
    std::int16_t height;
    std::int16_t bitsPerPixel;
    std::uint32_t blitMode;
    std::int32_t left;
    std::int32_t top;
    std::int32_t right;
    std::int32_t bottom;
};

struct LayoutRecord {
    std::int16_t x;
    std::int16_t y;
    std::uint16_t glyphId;
    std::uint16_t styleFlags;
};

struct GlyphCacheEntry {
    std::uint16_t encodedCharacter;
    std::uint16_t sizeClass;
    std::int16_t referenceCount;
};
#pragma pack(pop)

static_assert(sizeof(void*) == 4);
static_assert(sizeof(GameSurface) == 56);
static_assert(sizeof(LayoutRecord) == 8);
static_assert(sizeof(GlyphCacheEntry) == 6);

using RenderFontGlyphsFunction = void(__thiscall*)(void*, int, int, const RECT*, GameSurface*);
using BlitSurfaceFunction = int(__thiscall*)(GameSurface*, GameSurface*, int, int, int, int,
                                             int, int, int);
using BlitBitmapFunction = int(__thiscall*)(unsigned int*, GameSurface*, int, int, int, int,
                                            int, int);
using DestroySurfaceFunction = int(__thiscall*)(GameSurface*);

struct MaskRun {
    int y{};
    int left{};
    int right{};
};

struct TrackedGlyph {
    font_frame::Glyph draw;
    RECT pixels{};
    std::vector<MaskRun> mask;
    std::vector<MaskRun> occlusion;
};

struct SurfaceState {
    void* pixels{};
    int width{};
    int height{};
    int stride{};
    bool topDown{};
    std::vector<std::uint8_t> cleanPixels;
    std::vector<std::uint8_t> coverageProbe;
    std::vector<TrackedGlyph> glyphs;
};

struct PendingGlyph {
    font_frame::Glyph draw;
    RECT pixels{};
    bool replaceable{};
};

RenderFontGlyphsFunction g_originalRenderFontGlyphs = nullptr;
BlitSurfaceFunction g_originalBlitSurface = nullptr;
BlitBitmapFunction g_originalBlitBitmap = nullptr;
DestroySurfaceFunction g_originalDestroySurface = nullptr;
std::unordered_map<void*, SurfaceState> g_surfaces;
std::recursive_mutex g_mutex;
bool g_enabled = false;

int SurfaceStride(const GameSurface& surface) {
    const int rowBytes = (surface.width * surface.bitsPerPixel + 7) / 8;
    return (rowBytes + 3) & ~3;
}

bool IsUsableSurface(const GameSurface* surface) {
    return surface && surface->pixels && surface->bitsPerPixel == 16 &&
           surface->width > 0 && surface->height > 0 &&
           surface->width <= 4096 && surface->height <= 4096;
}

int StorageRow(const SurfaceState& state, int logicalY) {
    return state.topDown ? logicalY : state.height - logicalY - 1;
}

const std::uint8_t* RowPointer(const SurfaceState& state, const void* pixels, int logicalY) {
    return static_cast<const std::uint8_t*>(pixels) +
           static_cast<std::size_t>(StorageRow(state, logicalY)) * state.stride;
}

std::uint8_t* RowPointer(const SurfaceState& state, void* pixels, int logicalY) {
    return static_cast<std::uint8_t*>(pixels) +
           static_cast<std::size_t>(StorageRow(state, logicalY)) * state.stride;
}

RECT SurfaceBounds(const GameSurface& surface) {
    return RECT{0, 0, surface.width, surface.height};
}

bool Intersect(const RECT& first, const RECT& second, RECT& result) {
    result.left = std::max(first.left, second.left);
    result.top = std::max(first.top, second.top);
    result.right = std::min(first.right, second.right);
    result.bottom = std::min(first.bottom, second.bottom);
    return result.left < result.right && result.top < result.bottom;
}

bool Intersects(const RECT& first, const RECT& second) {
    RECT ignored{};
    return Intersect(first, second, ignored);
}

bool SameGlyph(const font_frame::Glyph& first, const font_frame::Glyph& second) {
    return first.glyphIndex == second.glyphIndex && first.styleFlags == second.styleFlags &&
           first.mainColor == second.mainColor && first.effectColor == second.effectColor &&
           first.x == second.x && first.y == second.y && first.cellSize == second.cellSize;
}

std::vector<MaskRun> BuildMask(const SurfaceState& state, const RECT& rectangle) {
    std::vector<MaskRun> mask;
    for (int y = rectangle.top; y < rectangle.bottom; ++y) {
        const auto* actual = reinterpret_cast<const std::uint16_t*>(
            RowPointer(state, state.pixels, y));
        const auto* clean = reinterpret_cast<const std::uint16_t*>(
            RowPointer(state, state.cleanPixels.data(), y));
        int start = -1;
        for (int x = rectangle.left; x < rectangle.right; ++x) {
            if (actual[x] != clean[x]) {
                if (start < 0) {
                    start = x;
                }
            } else if (start >= 0) {
                mask.push_back(MaskRun{y, start, x});
                start = -1;
            }
        }
        if (start >= 0) {
            mask.push_back(MaskRun{y, start, rectangle.right});
        }
    }
    return mask;
}

void NormalizeMask(std::vector<MaskRun>& mask) {
    std::sort(mask.begin(), mask.end(), [](const MaskRun& first, const MaskRun& second) {
        return first.y < second.y || (first.y == second.y && first.left < second.left);
    });
    std::vector<MaskRun> normalized;
    normalized.reserve(mask.size());
    for (const MaskRun& run : mask) {
        if (!normalized.empty() && normalized.back().y == run.y &&
            run.left <= normalized.back().right) {
            normalized.back().right = std::max(normalized.back().right, run.right);
        } else {
            normalized.push_back(run);
        }
    }
    mask = std::move(normalized);
}

SurfaceState& ResetState(const GameSurface& surface) {
    SurfaceState& state = g_surfaces[surface.pixels];
    state.pixels = surface.pixels;
    state.width = surface.width;
    state.height = surface.height;
    state.stride = SurfaceStride(surface);
    state.topDown = surface.topDown != 0;
    const std::size_t byteCount = static_cast<std::size_t>(state.stride) * state.height;
    state.cleanPixels.resize(byteCount);
    state.coverageProbe.resize(byteCount);
    std::memcpy(state.cleanPixels.data(), surface.pixels, byteCount);
    state.glyphs.clear();
    return state;
}

SurfaceState& EnsureState(const GameSurface& surface) {
    const auto found = g_surfaces.find(surface.pixels);
    if (found == g_surfaces.end() || found->second.width != surface.width ||
        found->second.height != surface.height ||
        found->second.stride != SurfaceStride(surface) ||
        found->second.topDown != (surface.topDown != 0)) {
        return ResetState(surface);
    }
    return found->second;
}

void CopyActualToClean(SurfaceState& state, const RECT& rectangle) {
    for (int y = rectangle.top; y < rectangle.bottom; ++y) {
        const auto* source = RowPointer(state, state.pixels, y) + rectangle.left * 2;
        auto* destination = RowPointer(state, state.cleanPixels.data(), y) + rectangle.left * 2;
        std::memcpy(destination, source,
                    static_cast<std::size_t>(rectangle.right - rectangle.left) * 2);
    }
}

void SynchronizeBackground(SurfaceState& state, const RECT& rectangle) {
    for (int y = rectangle.top; y < rectangle.bottom; ++y) {
        const auto* actual = reinterpret_cast<const std::uint16_t*>(
            RowPointer(state, state.pixels, y));
        auto* clean = reinterpret_cast<std::uint16_t*>(
            RowPointer(state, state.cleanPixels.data(), y));
        int cursor = rectangle.left;
        std::vector<MaskRun> protectedRuns;
        for (const TrackedGlyph& glyph : state.glyphs) {
            for (const MaskRun& run : glyph.mask) {
                if (run.y == y && run.right > rectangle.left && run.left < rectangle.right) {
                    protectedRuns.push_back(MaskRun{
                        y, std::max(run.left, static_cast<int>(rectangle.left)),
                        std::min(run.right, static_cast<int>(rectangle.right))});
                }
            }
        }
        NormalizeMask(protectedRuns);
        for (const MaskRun& run : protectedRuns) {
            if (cursor < run.left) {
                std::copy(actual + cursor, actual + run.left, clean + cursor);
            }
            cursor = std::max(cursor, run.right);
        }
        if (cursor < rectangle.right) {
            std::copy(actual + cursor, actual + rectangle.right, clean + cursor);
        }
    }
}

std::vector<MaskRun> SubtractRuns(const std::vector<MaskRun>& source,
                                  const std::vector<MaskRun>& coverage) {
    std::vector<MaskRun> result;
    for (const MaskRun& run : source) {
        int cursor = run.left;
        for (const MaskRun& covered : coverage) {
            if (covered.y != run.y || covered.right <= cursor || covered.left >= run.right) {
                continue;
            }
            if (cursor < covered.left) {
                result.push_back(MaskRun{run.y, cursor, std::min(run.right, covered.left)});
            }
            cursor = std::max(cursor, covered.right);
            if (cursor >= run.right) {
                break;
            }
        }
        if (cursor < run.right) {
            result.push_back(MaskRun{run.y, cursor, run.right});
        }
    }
    return result;
}

bool CoversRectangle(const std::vector<MaskRun>& coverage, const RECT& rectangle) {
    for (int y = rectangle.top; y < rectangle.bottom; ++y) {
        int cursor = rectangle.left;
        for (const MaskRun& run : coverage) {
            if (run.y != y || run.right <= cursor || run.left >= rectangle.right) {
                continue;
            }
            if (run.left > cursor) {
                return false;
            }
            cursor = std::max(cursor, run.right);
            if (cursor >= rectangle.right) {
                break;
            }
        }
        if (cursor < rectangle.right) {
            return false;
        }
    }
    return true;
}

void ApplyCoverage(SurfaceState& state, const std::vector<MaskRun>& coverage) {
    if (coverage.empty()) {
        return;
    }
    for (TrackedGlyph& glyph : state.glyphs) {
        std::vector<MaskRun> clipped;
        for (const MaskRun& run : coverage) {
            if (run.y >= glyph.pixels.top && run.y < glyph.pixels.bottom &&
                run.right > glyph.pixels.left && run.left < glyph.pixels.right) {
                clipped.push_back(MaskRun{
                    run.y, std::max(run.left, static_cast<int>(glyph.pixels.left)),
                    std::min(run.right, static_cast<int>(glyph.pixels.right))});
            }
        }
        if (clipped.empty()) {
            continue;
        }
        NormalizeMask(clipped);
        glyph.mask = SubtractRuns(glyph.mask, clipped);
        glyph.occlusion.insert(glyph.occlusion.end(), clipped.begin(), clipped.end());
        NormalizeMask(glyph.occlusion);
    }
    std::erase_if(state.glyphs, [](const TrackedGlyph& glyph) {
        return CoversRectangle(glyph.occlusion, glyph.pixels);
    });
}

std::vector<MaskRun> BuildOpaqueCoverage(const SurfaceState& sourceState,
                                         int sourceX, int sourceY,
                                         int destinationX, int destinationY,
                                         int width, int height, int transparentColor,
                                         const RECT& destinationBounds) {
    std::vector<MaskRun> result;
    const std::uint16_t key = static_cast<std::uint16_t>(transparentColor);
    for (int offsetY = 0; offsetY < height; ++offsetY) {
        const int sourceRow = sourceY + offsetY;
        const int destinationRow = destinationY + offsetY;
        if (sourceRow < 0 || sourceRow >= sourceState.height ||
            destinationRow < destinationBounds.top || destinationRow >= destinationBounds.bottom) {
            continue;
        }
        const auto* pixels = reinterpret_cast<const std::uint16_t*>(
            RowPointer(sourceState, sourceState.pixels, sourceRow));
        int start = -1;
        for (int offsetX = 0; offsetX < width; ++offsetX) {
            const int sourceColumn = sourceX + offsetX;
            const int destinationColumn = destinationX + offsetX;
            const bool inside = sourceColumn >= 0 && sourceColumn < sourceState.width &&
                                destinationColumn >= destinationBounds.left &&
                                destinationColumn < destinationBounds.right;
            const bool opaque = inside && pixels[sourceColumn] != key;
            if (opaque && start < 0) {
                start = destinationColumn;
            } else if (!opaque && start >= 0) {
                result.push_back(MaskRun{destinationRow, start, destinationColumn});
                start = -1;
            }
        }
        if (start >= 0) {
            result.push_back(MaskRun{destinationRow, start,
                                     std::min(destinationX + width,
                                              static_cast<int>(destinationBounds.right))});
        }
    }
    NormalizeMask(result);
    return result;
}

std::vector<MaskRun> TranslateRuns(const std::vector<MaskRun>& runs,
                                   const RECT& sourceRectangle,
                                   int deltaX, int deltaY,
                                   const RECT& destinationBounds) {
    std::vector<MaskRun> result;
    for (const MaskRun& run : runs) {
        if (run.y < sourceRectangle.top || run.y >= sourceRectangle.bottom ||
            run.right <= sourceRectangle.left || run.left >= sourceRectangle.right) {
            continue;
        }
        const int y = run.y + deltaY;
        if (y < destinationBounds.top || y >= destinationBounds.bottom) {
            continue;
        }
        const int left = std::max(run.left, static_cast<int>(sourceRectangle.left)) + deltaX;
        const int right = std::min(run.right, static_cast<int>(sourceRectangle.right)) + deltaX;
        if (right > destinationBounds.left && left < destinationBounds.right) {
            result.push_back(MaskRun{y, std::max(left, static_cast<int>(destinationBounds.left)),
                                     std::min(right, static_cast<int>(destinationBounds.right))});
        }
    }
    NormalizeMask(result);
    return result;
}

struct RectangleSnapshot {
    RECT rectangle{};
    std::vector<std::uint16_t> actual;
};

RectangleSnapshot CaptureRectangle(const SurfaceState& state, const RECT& requested) {
    RectangleSnapshot snapshot;
    if (!Intersect(requested, RECT{0, 0, state.width, state.height}, snapshot.rectangle)) {
        return snapshot;
    }
    const int width = snapshot.rectangle.right - snapshot.rectangle.left;
    const int height = snapshot.rectangle.bottom - snapshot.rectangle.top;
    snapshot.actual.resize(static_cast<std::size_t>(width) * height);
    for (int y = snapshot.rectangle.top; y < snapshot.rectangle.bottom; ++y) {
        const auto* actual = reinterpret_cast<const std::uint16_t*>(
            RowPointer(state, state.pixels, y)) + snapshot.rectangle.left;
        const std::size_t offset = static_cast<std::size_t>(y - snapshot.rectangle.top) * width;
        std::copy(actual, actual + width, snapshot.actual.data() + offset);
    }
    return snapshot;
}

void PrepareCoverageProbe(SurfaceState& state, const RectangleSnapshot& before) {
    const int width = before.rectangle.right - before.rectangle.left;
    for (int y = before.rectangle.top; y < before.rectangle.bottom; ++y) {
        auto* probe = reinterpret_cast<std::uint16_t*>(
            RowPointer(state, state.coverageProbe.data(), y));
        const std::size_t offset = static_cast<std::size_t>(y - before.rectangle.top) * width;
        for (int x = before.rectangle.left; x < before.rectangle.right; ++x) {
            const std::size_t index = offset + static_cast<std::size_t>(x - before.rectangle.left);
            probe[x] = static_cast<std::uint16_t>(before.actual[index] ^ 0x7FFFU);
        }
    }
}

std::vector<MaskRun> FindOperationCoverage(const SurfaceState& state,
                                           const RectangleSnapshot& before) {
    std::vector<MaskRun> result;
    const int width = before.rectangle.right - before.rectangle.left;
    for (int y = before.rectangle.top; y < before.rectangle.bottom; ++y) {
        const auto* actual = reinterpret_cast<const std::uint16_t*>(
            RowPointer(state, state.pixels, y));
        const auto* probe = reinterpret_cast<const std::uint16_t*>(
            RowPointer(state, state.coverageProbe.data(), y));
        const std::size_t offset = static_cast<std::size_t>(y - before.rectangle.top) * width;
        int start = -1;
        for (int x = before.rectangle.left; x < before.rectangle.right; ++x) {
            const std::size_t index = offset + static_cast<std::size_t>(x - before.rectangle.left);
            const std::uint16_t probeBefore =
                static_cast<std::uint16_t>(before.actual[index] ^ 0x7FFFU);
            const bool written = actual[x] != before.actual[index] ||
                                 probe[x] != probeBefore;
            if (written && start < 0) {
                start = x;
            } else if (!written && start >= 0) {
                result.push_back(MaskRun{y, start, x});
                start = -1;
            }
        }
        if (start >= 0) {
            result.push_back(MaskRun{y, start, before.rectangle.right});
        }
    }
    NormalizeMask(result);
    return result;
}

void CopyActualRunsToClean(SurfaceState& state, const std::vector<MaskRun>& runs) {
    for (const MaskRun& run : runs) {
        const auto* actual = reinterpret_cast<const std::uint16_t*>(
            RowPointer(state, state.pixels, run.y));
        auto* clean = reinterpret_cast<std::uint16_t*>(
            RowPointer(state, state.cleanPixels.data(), run.y));
        std::copy(actual + run.left, actual + run.right, clean + run.left);
    }
}

bool DecodeCharacter(std::uint16_t encoded, std::uint32_t& codePoint) {
    std::array<char, 2> bytes{static_cast<char>(encoded & 0xFFU),
                              static_cast<char>(encoded >> 8)};
    const int byteCount = static_cast<unsigned char>(bytes[0]) >= 0xA1U ? 2 : 1;
    wchar_t decoded[2]{};
    const int count = MultiByteToWideChar(950, MB_ERR_INVALID_CHARS, bytes.data(), byteCount,
                                          decoded, static_cast<int>(std::size(decoded)));
    if (count != 1) {
        return false;
    }
    codePoint = static_cast<std::uint16_t>(decoded[0]);
    return true;
}

std::uint32_t ResolvePaletteColor(std::size_t paletteIndex) {
    const auto* module = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!module) {
        return 0xF0F0F0U;
    }
    const auto palette = *reinterpret_cast<const std::uint8_t* const*>(
        module + kFontPalettePointerRva);
    if (!palette) {
        return 0xF0F0F0U;
    }
    const std::size_t block = paletteIndex * 64;
    const std::uint8_t* opaque = palette + block + 15 * 4;
    return (static_cast<std::uint32_t>(opaque[0]) << 16) |
           (static_cast<std::uint32_t>(opaque[1]) << 8) | opaque[2];
}

std::vector<PendingGlyph> ReadGlyphs(void* layout, int originX, int originY,
                                     const RECT& clip, const GameSurface& surface) {
    std::vector<PendingGlyph> result;
    const auto* module = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!module || !layout) {
        return result;
    }
    const auto layoutIndex = *reinterpret_cast<const std::uint32_t*>(
        static_cast<const std::uint8_t*>(layout) + 56);
    if (layoutIndex >= kLayoutListCount) {
        return result;
    }
    const auto* lists = reinterpret_cast<LayoutRecord* const*>(module + kLayoutListsRva);
    const LayoutRecord* records = lists[layoutIndex];
    const auto* cache = reinterpret_cast<const GlyphCacheEntry*>(module + kGlyphCacheRva);
    if (!records) {
        return result;
    }

    const RECT bounds = SurfaceBounds(surface);
    for (std::size_t index = 0; index < kMaximumGlyphsPerDraw; ++index) {
        const LayoutRecord& record = records[index];
        if (record.glyphId == 0xFFFFU) {
            break;
        }
        if ((record.styleFlags & 0x8000U) != 0 || record.glyphId >= kGlyphCacheCount) {
            continue;
        }
        const GlyphCacheEntry& entry = cache[record.glyphId];
        if (entry.sizeClass > 2) {
            continue;
        }
        constexpr std::array<int, 3> cellSizes{24, 16, 12};
        const int cellSize = cellSizes[entry.sizeClass];
        const bool fullWidth = static_cast<std::uint8_t>(entry.encodedCharacter) >= 0xA1U;
        const int width = fullWidth ? cellSize : cellSize / 2;
        RECT pixels{originX + record.x, originY + record.y,
                    originX + record.x + width, originY + record.y + cellSize};
        if ((record.styleFlags & 0x4000U) != 0) {
            --pixels.left;
            --pixels.top;
            ++pixels.right;
            ++pixels.bottom;
        } else if ((record.styleFlags & 0x2000U) != 0) {
            ++pixels.right;
            ++pixels.bottom;
        }
        RECT clipped{};
        RECT clipAndBounds{};
        if (!Intersect(clip, bounds, clipAndBounds) ||
            !Intersect(pixels, clipAndBounds, clipped)) {
            continue;
        }

        std::uint32_t codePoint = 0;
        std::uint16_t glyphIndex = 0;
        const bool replaceable = DecodeCharacter(entry.encodedCharacter, codePoint) &&
                                 codePoint != L' ' &&
                                 font_renderer::ResolveGlyph(codePoint, glyphIndex);
        font_frame::Glyph draw{};
        draw.glyphIndex = glyphIndex;
        draw.styleFlags = record.styleFlags;
        draw.mainColor = ResolvePaletteColor(record.styleFlags & 0x1FU);
        draw.effectColor = ResolvePaletteColor((record.styleFlags >> 8) & 0x1FU);
        draw.x = static_cast<float>(originX + record.x);
        draw.y = static_cast<float>(originY + record.y);
        draw.cellSize = static_cast<float>(cellSize);
        draw.clip = clipAndBounds;
        draw.bounds = clipped;
        result.push_back(PendingGlyph{draw, clipped, replaceable});
    }
    return result;
}

void __fastcall HookRenderFontGlyphs(void* layout, void*, int originX, int originY,
                                     const RECT* clip, GameSurface* destination) {
    if (!g_enabled || !g_originalRenderFontGlyphs || !clip || !IsUsableSurface(destination)) {
        g_originalRenderFontGlyphs(layout, originX, originY, clip, destination);
        return;
    }

    const std::vector<PendingGlyph> pending =
        ReadGlyphs(layout, originX, originY, *clip, *destination);
    {
        std::scoped_lock lock(g_mutex);
        SurfaceState& state = EnsureState(*destination);
        for (const PendingGlyph& glyph : pending) {
            SynchronizeBackground(state, glyph.pixels);
        }
    }

    g_originalRenderFontGlyphs(layout, originX, originY, clip, destination);

    std::scoped_lock lock(g_mutex);
    SurfaceState& state = EnsureState(*destination);
    for (const PendingGlyph& glyph : pending) {
        if (!glyph.replaceable) {
            CopyActualToClean(state, glyph.pixels);
            continue;
        }
        std::vector<MaskRun> mask = BuildMask(state, glyph.pixels);
        if (!mask.empty()) {
            std::erase_if(state.glyphs, [&glyph](const TrackedGlyph& existing) {
                return SameGlyph(existing.draw, glyph.draw);
            });
            state.glyphs.push_back(
                TrackedGlyph{glyph.draw, glyph.pixels, std::move(mask), {}});
        }
    }
}

RECT TranslateAndClip(const RECT& rectangle, const RECT& sourceBounds,
                      int sourceX, int sourceY, int destinationX, int destinationY,
                      const RECT& destinationBounds) {
    RECT clipped{};
    if (!Intersect(rectangle, sourceBounds, clipped)) {
        return RECT{};
    }
    OffsetRect(&clipped, destinationX - sourceX, destinationY - sourceY);
    RECT result{};
    if (!Intersect(clipped, destinationBounds, result)) {
        return RECT{};
    }
    return result;
}

int __fastcall HookBlitSurface(GameSurface* source, void*, GameSurface* destination,
                               int destinationX, int destinationY, int width, int height,
                               int sourceX, int sourceY, int transparentColor) {
    if (!g_enabled || !g_originalBlitSurface || !IsUsableSurface(source) ||
        !IsUsableSurface(destination) || width <= 0 || height <= 0) {
        return g_originalBlitSurface(source, destination, destinationX, destinationY,
                                     width, height, sourceX, sourceY, transparentColor);
    }

    std::scoped_lock lock(g_mutex);
    const auto sourceStateIt = g_surfaces.find(source->pixels);
    if (sourceStateIt != g_surfaces.end()) {
        RECT synchronized{};
        if (Intersect(RECT{sourceX, sourceY, sourceX + width, sourceY + height},
                      SurfaceBounds(*source), synchronized)) {
            SynchronizeBackground(sourceStateIt->second, synchronized);
        }
    }
    const bool sourceHasText = sourceStateIt != g_surfaces.end() &&
                               !sourceStateIt->second.glyphs.empty();
    const bool destinationTracked = g_surfaces.contains(destination->pixels);
    if (!sourceHasText && !destinationTracked) {
        return g_originalBlitSurface(source, destination, destinationX, destinationY,
                                     width, height, sourceX, sourceY, transparentColor);
    }

    const std::vector<TrackedGlyph> sourceGlyphs =
        sourceHasText ? sourceStateIt->second.glyphs : std::vector<TrackedGlyph>{};
    const bool sourceTracked = sourceStateIt != g_surfaces.end();
    const std::uint8_t* sourceCleanPixels = sourceTracked
        ? sourceStateIt->second.cleanPixels.data()
        : nullptr;
    SurfaceState sourceView{};
    sourceView.pixels = source->pixels;
    sourceView.width = source->width;
    sourceView.height = source->height;
    sourceView.stride = SurfaceStride(*source);
    sourceView.topDown = source->topDown != 0;

    SurfaceState& destinationState = EnsureState(*destination);
    const std::vector<MaskRun> coverage = BuildOpaqueCoverage(
        sourceView, sourceX, sourceY, destinationX, destinationY, width, height,
        transparentColor, SurfaceBounds(*destination));
    ApplyCoverage(destinationState, coverage);

    GameSurface cleanSource = *source;
    GameSurface cleanDestination = *destination;
    if (sourceTracked) {
        cleanSource.pixels = const_cast<std::uint8_t*>(sourceCleanPixels);
    }
    cleanDestination.pixels = destinationState.cleanPixels.data();
    const int cleanResult = g_originalBlitSurface(
        &cleanSource, &cleanDestination, destinationX, destinationY, width, height,
        sourceX, sourceY, transparentColor);
    const int result = g_originalBlitSurface(source, destination, destinationX, destinationY,
                                             width, height, sourceX, sourceY, transparentColor);
    if (!cleanResult || !result) {
        ResetState(*destination);
        return result;
    }

    const RECT sourceRectangle{sourceX, sourceY, sourceX + width, sourceY + height};
    const RECT destinationRectangle{destinationX, destinationY,
                                    destinationX + width, destinationY + height};
    const RECT destinationBounds = SurfaceBounds(*destination);
    for (const TrackedGlyph& sourceGlyph : sourceGlyphs) {
        RECT pixels = TranslateAndClip(sourceGlyph.pixels, sourceRectangle,
                                       sourceX, sourceY, destinationX, destinationY,
                                       destinationBounds);
        if (pixels.left >= pixels.right || pixels.top >= pixels.bottom) {
            continue;
        }
        font_frame::Glyph draw = sourceGlyph.draw;
        draw.x += static_cast<float>(destinationX - sourceX);
        draw.y += static_cast<float>(destinationY - sourceY);
        draw.clip = TranslateAndClip(draw.clip, sourceRectangle, sourceX, sourceY,
                                     destinationX, destinationY, destinationBounds);
        draw.bounds = pixels;
        if (draw.clip.left >= draw.clip.right || draw.clip.top >= draw.clip.bottom ||
            !Intersects(pixels, destinationRectangle)) {
            continue;
        }
        const int deltaX = destinationX - sourceX;
        const int deltaY = destinationY - sourceY;
        std::vector<MaskRun> mask = TranslateRuns(
            sourceGlyph.mask, sourceRectangle, deltaX, deltaY, destinationBounds);
        if (mask.empty()) {
            continue;
        }
        std::vector<MaskRun> occlusion = TranslateRuns(
            sourceGlyph.occlusion, sourceRectangle, deltaX, deltaY, destinationBounds);
        draw.bounds = pixels;
        std::erase_if(destinationState.glyphs, [&draw](const TrackedGlyph& glyph) {
            return SameGlyph(glyph.draw, draw);
        });
        destinationState.glyphs.push_back(
            TrackedGlyph{draw, pixels, std::move(mask), std::move(occlusion)});
    }
    return result;
}

int __fastcall HookBlitBitmap(unsigned int* bitmap, void*, GameSurface* destination,
                              int destinationX, int destinationY, int width, int height,
                              int sourceX, int sourceY) {
    if (!g_enabled || !g_originalBlitBitmap || !bitmap || !IsUsableSurface(destination) ||
        width <= 0 || height <= 0) {
        return g_originalBlitBitmap(bitmap, destination, destinationX, destinationY,
                                    width, height, sourceX, sourceY);
    }

    std::scoped_lock lock(g_mutex);
    const auto found = g_surfaces.find(destination->pixels);
    if (found == g_surfaces.end() || found->second.glyphs.empty()) {
        return g_originalBlitBitmap(bitmap, destination, destinationX, destinationY,
                                    width, height, sourceX, sourceY);
    }

    SurfaceState& state = found->second;
    const RECT requested{destinationX, destinationY,
                         destinationX + width, destinationY + height};
    const bool touchesText = std::any_of(
        state.glyphs.begin(), state.glyphs.end(), [&requested](const TrackedGlyph& glyph) {
            return Intersects(requested, glyph.pixels);
        });
    if (!touchesText) {
        return g_originalBlitBitmap(bitmap, destination, destinationX, destinationY,
                                    width, height, sourceX, sourceY);
    }
    const RectangleSnapshot before = CaptureRectangle(
        state, requested);
    PrepareCoverageProbe(state, before);
    GameSurface probeDestination = *destination;
    probeDestination.pixels = state.coverageProbe.data();
    const int probeResult = g_originalBlitBitmap(
        bitmap, &probeDestination, destinationX, destinationY, width, height, sourceX, sourceY);
    const int result = g_originalBlitBitmap(bitmap, destination, destinationX, destinationY,
                                            width, height, sourceX, sourceY);
    if (!probeResult || !result) {
        ResetState(*destination);
        return result;
    }
    const std::vector<MaskRun> coverage = FindOperationCoverage(state, before);
    ApplyCoverage(state, coverage);
    CopyActualRunsToClean(state, coverage);
    return result;
}

int __fastcall HookDestroySurface(GameSurface* surface, void*) {
    if (surface) {
        std::scoped_lock lock(g_mutex);
        if (surface->pixels) {
            g_surfaces.erase(surface->pixels);
        }
    }
    return g_originalDestroySurface(surface);
}

} // namespace

bool InstallHooks() {
    std::scoped_lock lock(g_mutex);
    g_enabled = false;
    auto* module = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!module) {
        return false;
    }
    constexpr std::array<unsigned char, 7> renderExpected{
        0x6A, 0xFF, 0x68, 0x6A, 0x99, 0x5F, 0x00};
    constexpr std::array<unsigned char, 8> blitExpected{
        0x8B, 0x44, 0x24, 0x18, 0x8B, 0x54, 0x24, 0x0C};
    constexpr std::array<unsigned char, 5> bitmapExpected{
        0x55, 0x8B, 0xEC, 0x6A, 0xFF};
    constexpr std::array<unsigned char, 7> destroyExpected{
        0x6A, 0xFF, 0x68, 0x98, 0x9A, 0x5F, 0x00};
    void* renderTrampoline = nullptr;
    void* blitTrampoline = nullptr;
    void* bitmapTrampoline = nullptr;
    void* destroyTrampoline = nullptr;
    if (!code_hook::Install(module + kDestroySurfaceRva,
                            reinterpret_cast<void*>(&HookDestroySurface), destroyExpected,
                            &destroyTrampoline)) {
        return false;
    }
    g_originalDestroySurface = reinterpret_cast<DestroySurfaceFunction>(destroyTrampoline);
    if (!code_hook::Install(module + kRenderFontGlyphsRva,
                            reinterpret_cast<void*>(&HookRenderFontGlyphs), renderExpected,
                            &renderTrampoline)) {
        return false;
    }
    g_originalRenderFontGlyphs = reinterpret_cast<RenderFontGlyphsFunction>(renderTrampoline);
    if (!code_hook::Install(module + kBlitSurfaceRva,
                            reinterpret_cast<void*>(&HookBlitSurface), blitExpected,
                            &blitTrampoline)) {
        return false;
    }
    g_originalBlitSurface = reinterpret_cast<BlitSurfaceFunction>(blitTrampoline);
    if (!code_hook::Install(module + kBlitBitmapRva,
                            reinterpret_cast<void*>(&HookBlitBitmap), bitmapExpected,
                            &bitmapTrampoline)) {
        return false;
    }
    g_originalBlitBitmap = reinterpret_cast<BlitBitmapFunction>(bitmapTrampoline);
    g_enabled = true;
    return true;
}

font_frame::Frame PrepareFrame(void* framebufferPixels, int width, int height) {
    font_frame::Frame frame;
    if (!g_enabled || !framebufferPixels) {
        return frame;
    }
    std::scoped_lock lock(g_mutex);
    const auto found = g_surfaces.find(framebufferPixels);
    if (found == g_surfaces.end() || found->second.width != width ||
        found->second.height != height) {
        return frame;
    }
    SurfaceState& state = found->second;
    for (const TrackedGlyph& glyph : state.glyphs) {
        font_frame::Glyph draw = glyph.draw;
        draw.occlusion.reserve(glyph.occlusion.size());
        for (const MaskRun& run : glyph.occlusion) {
            draw.occlusion.push_back(RECT{run.left, run.y, run.right, run.y + 1});
        }
        frame.glyphs.push_back(std::move(draw));
        for (const MaskRun& mask : glyph.mask) {
            font_frame::PixelRun run{};
            run.storageRow = static_cast<std::uint32_t>(StorageRow(state, mask.y));
            run.x = static_cast<std::uint16_t>(mask.left);
            const auto* source = reinterpret_cast<const std::uint16_t*>(
                RowPointer(state, state.cleanPixels.data(), mask.y));
            run.pixels.assign(source + mask.left, source + mask.right);
            frame.cleanPixels.push_back(std::move(run));
        }
    }
    return frame;
}

void Shutdown() {
    std::scoped_lock lock(g_mutex);
    g_enabled = false;
    g_surfaces.clear();
}

} // namespace san9::font_capture
