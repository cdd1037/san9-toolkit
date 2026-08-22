#include "font_renderer.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>

namespace san9::font_renderer {
namespace {

using Microsoft::WRL::ComPtr;

constexpr float kFontAscent = 965.0F / 1000.0F;
constexpr float kFullPaletteCoverage = 128.0F;
constexpr float kOutlinePaletteCoverage = 128.0F / 4.0F;
constexpr float kOutlineOpacity = kOutlinePaletteCoverage / kFullPaletteCoverage;

ComPtr<ID2D1Factory> g_d2dFactory;
ComPtr<IDWriteFactory> g_dwriteFactory;
ComPtr<IDWriteFontFile> g_fontFile;
ComPtr<IDWriteFontFace> g_fontFace;
ComPtr<ID2D1RenderTarget> g_renderTarget;
ComPtr<ID2D1Layer> g_clipLayer;
std::unordered_map<std::uint32_t, ComPtr<ID2D1SolidColorBrush>> g_mainBrushes;
std::unordered_map<std::uint32_t, ComPtr<ID2D1SolidColorBrush>> g_outlineBrushes;

D2D1_COLOR_F ToColor(std::uint32_t rgb, float alpha) {
    return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFFU) / 255.0F,
                        static_cast<float>((rgb >> 8) & 0xFFU) / 255.0F,
                        static_cast<float>(rgb & 0xFFU) / 255.0F, alpha);
}

ID2D1SolidColorBrush* GetBrush(std::uint32_t color, bool outline) {
    auto& brushes = outline ? g_outlineBrushes : g_mainBrushes;
    const auto found = brushes.find(color);
    if (found != brushes.end()) {
        return found->second.Get();
    }
    ComPtr<ID2D1SolidColorBrush> brush;
    const float alpha = outline ? kOutlineOpacity : 1.0F;
    if (!g_renderTarget ||
        FAILED(g_renderTarget->CreateSolidColorBrush(ToColor(color, alpha), &brush))) {
        return nullptr;
    }
    ID2D1SolidColorBrush* result = brush.Get();
    brushes.emplace(color, std::move(brush));
    return result;
}

bool DrawGlyph(const font_frame::Glyph& glyph, float offsetX, float offsetY,
               ID2D1Brush* brush) {
    if (!brush || !g_fontFace) {
        return false;
    }
    const FLOAT advance = glyph.cellSize;
    DWRITE_GLYPH_RUN run{};
    run.fontFace = g_fontFace.Get();
    run.fontEmSize = glyph.cellSize;
    run.glyphCount = 1;
    run.glyphIndices = &glyph.glyphIndex;
    run.glyphAdvances = &advance;
    const D2D1_POINT_2F origin = D2D1::Point2F(
        glyph.x + offsetX, glyph.y + kFontAscent * glyph.cellSize + offsetY);
    g_renderTarget->DrawGlyphRun(origin, &run, brush, DWRITE_MEASURING_MODE_NATURAL);
    return true;
}

void AddRectangle(ID2D1GeometrySink& sink, float left, float top,
                  float right, float bottom) {
    sink.BeginFigure(D2D1::Point2F(left, top), D2D1_FIGURE_BEGIN_FILLED);
    const std::array<D2D1_POINT_2F, 3> points{
        D2D1::Point2F(right, top), D2D1::Point2F(right, bottom),
        D2D1::Point2F(left, bottom)};
    sink.AddLines(points.data(), static_cast<UINT32>(points.size()));
    sink.EndFigure(D2D1_FIGURE_END_CLOSED);
}

ComPtr<ID2D1PathGeometry> BuildVisibleGeometry(const font_frame::Glyph& glyph) {
    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (!g_d2dFactory ||
        FAILED(g_d2dFactory->CreatePathGeometry(&geometry)) ||
        FAILED(geometry->Open(&sink))) {
        return nullptr;
    }
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    for (LONG y = glyph.bounds.top; y < glyph.bounds.bottom; ++y) {
        LONG cursor = glyph.bounds.left;
        for (const RECT& covered : glyph.occlusion) {
            if (covered.top != y || covered.right <= cursor ||
                covered.left >= glyph.bounds.right) {
                continue;
            }
            if (cursor < covered.left) {
                AddRectangle(*sink.Get(), static_cast<float>(cursor), static_cast<float>(y),
                             static_cast<float>(covered.left), static_cast<float>(y + 1));
            }
            cursor = std::max(cursor, covered.right);
            if (cursor >= glyph.bounds.right) {
                break;
            }
        }
        if (cursor < glyph.bounds.right) {
            AddRectangle(*sink.Get(), static_cast<float>(cursor), static_cast<float>(y),
                         static_cast<float>(glyph.bounds.right), static_cast<float>(y + 1));
        }
    }
    if (FAILED(sink->Close())) {
        return nullptr;
    }
    return geometry;
}

} // namespace

bool Initialize(std::wstring_view fontPath) {
    Shutdown();
    if (fontPath.empty() ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 IID_PPV_ARGS(&g_d2dFactory))) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(g_dwriteFactory.GetAddressOf())))) {
        Shutdown();
        return false;
    }

    const std::wstring path(fontPath);
    if (FAILED(g_dwriteFactory->CreateFontFileReference(path.c_str(), nullptr, &g_fontFile))) {
        Shutdown();
        return false;
    }
    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE fileType{};
    DWRITE_FONT_FACE_TYPE faceType{};
    UINT32 faceCount = 0;
    if (FAILED(g_fontFile->Analyze(&supported, &fileType, &faceType, &faceCount)) ||
        !supported || faceCount != 1 ||
        FAILED(g_dwriteFactory->CreateFontFace(faceType, 1, g_fontFile.GetAddressOf(), 0,
                                               DWRITE_FONT_SIMULATIONS_NONE, &g_fontFace))) {
        Shutdown();
        return false;
    }
    DWRITE_FONT_METRICS metrics{};
    g_fontFace->GetMetrics(&metrics);
    if (metrics.designUnitsPerEm != 1000 || metrics.ascent != 965) {
        Shutdown();
        return false;
    }
    return true;
}

bool ResolveGlyph(std::uint32_t codePoint, std::uint16_t& glyphIndex) {
    glyphIndex = 0;
    if (!g_fontFace || codePoint > 0xFFFFU) {
        return false;
    }
    const UINT32 value = codePoint;
    UINT16 index = 0;
    if (FAILED(g_fontFace->GetGlyphIndices(&value, 1, &index)) || index == 0) {
        return false;
    }
    glyphIndex = index;
    return true;
}

bool BindTarget(ID3D11Texture2D* backBuffer) {
    ReleaseTarget();
    if (!g_d2dFactory || !backBuffer) {
        return false;
    }
    ComPtr<IDXGISurface> surface;
    if (FAILED(backBuffer->QueryInterface(IID_PPV_ARGS(&surface)))) {
        return false;
    }
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        0.0F, 0.0F, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    if (FAILED(g_d2dFactory->CreateDxgiSurfaceRenderTarget(surface.Get(), properties,
                                                           &g_renderTarget)) ||
        FAILED(g_renderTarget->CreateLayer(nullptr, &g_clipLayer))) {
        ReleaseTarget();
        return false;
    }
    g_renderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g_renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return true;
}

void ReleaseTarget() {
    g_outlineBrushes.clear();
    g_mainBrushes.clear();
    g_clipLayer.Reset();
    g_renderTarget.Reset();
}

bool Draw(const font_frame::Frame& frame, const viewport::Bounds& bounds) {
    if (frame.glyphs.empty()) {
        return true;
    }
    if (!g_renderTarget || bounds.width <= 0 || bounds.height <= 0) {
        return false;
    }
    const float scaleX = static_cast<float>(bounds.width) / 1024.0F;
    const float scaleY = static_cast<float>(bounds.height) / 768.0F;
    g_renderTarget->BeginDraw();
    g_renderTarget->SetTransform(D2D1::Matrix3x2F(
        scaleX, 0.0F, 0.0F, scaleY,
        static_cast<float>(bounds.x), static_cast<float>(bounds.y)));

    constexpr std::array<D2D1_POINT_2F, 8> outlineOffsets{
        D2D1_POINT_2F{-1.0F, -1.0F}, D2D1_POINT_2F{0.0F, -1.0F},
        D2D1_POINT_2F{1.0F, -1.0F}, D2D1_POINT_2F{-1.0F, 0.0F},
        D2D1_POINT_2F{1.0F, 0.0F}, D2D1_POINT_2F{-1.0F, 1.0F},
        D2D1_POINT_2F{0.0F, 1.0F}, D2D1_POINT_2F{1.0F, 1.0F},
    };
    bool ok = true;
    for (const font_frame::Glyph& glyph : frame.glyphs) {
        g_renderTarget->PushAxisAlignedClip(
            D2D1::RectF(static_cast<float>(glyph.clip.left), static_cast<float>(glyph.clip.top),
                        static_cast<float>(glyph.clip.right), static_cast<float>(glyph.clip.bottom)),
            D2D1_ANTIALIAS_MODE_ALIASED);
        bool layerPushed = false;
        if (!glyph.occlusion.empty()) {
            const ComPtr<ID2D1PathGeometry> geometry = BuildVisibleGeometry(glyph);
            if (!geometry || !g_clipLayer) {
                ok = false;
                g_renderTarget->PopAxisAlignedClip();
                continue;
            }
            g_renderTarget->PushLayer(
                D2D1::LayerParameters(
                    D2D1::RectF(static_cast<float>(glyph.bounds.left),
                                static_cast<float>(glyph.bounds.top),
                                static_cast<float>(glyph.bounds.right),
                                static_cast<float>(glyph.bounds.bottom)),
                    geometry.Get(), D2D1_ANTIALIAS_MODE_ALIASED),
                g_clipLayer.Get());
            layerPushed = true;
        }
        ID2D1SolidColorBrush* mainBrush = GetBrush(glyph.mainColor, false);
        if ((glyph.styleFlags & 0x2000U) != 0) {
            ID2D1SolidColorBrush* effectBrush = GetBrush(glyph.effectColor, false);
            ok = DrawGlyph(glyph, 1.0F, 1.0F, effectBrush) && ok;
        } else if ((glyph.styleFlags & 0x4000U) != 0) {
            ID2D1SolidColorBrush* outlineBrush = GetBrush(glyph.effectColor, true);
            for (const D2D1_POINT_2F offset : outlineOffsets) {
                ok = DrawGlyph(glyph, offset.x, offset.y, outlineBrush) && ok;
            }
        }
        ok = DrawGlyph(glyph, 0.0F, 0.0F, mainBrush) && ok;
        if (layerPushed) {
            g_renderTarget->PopLayer();
        }
        g_renderTarget->PopAxisAlignedClip();
    }
    g_renderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
    return SUCCEEDED(g_renderTarget->EndDraw()) && ok;
}

void Shutdown() {
    ReleaseTarget();
    g_fontFace.Reset();
    g_fontFile.Reset();
    g_dwriteFactory.Reset();
    g_d2dFactory.Reset();
}

} // namespace san9::font_renderer
