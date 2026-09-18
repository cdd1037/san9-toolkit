#include "d3d11_presenter.h"

#include "presentation_schedule.h"
#include "viewport.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace san9::d3d11_presenter {

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kLogicalWidth = 1024;
constexpr int kLogicalHeight = 768;
constexpr DXGI_FORMAT kFrameTextureFormat = DXGI_FORMAT_B5G5R5A1_UNORM;
constexpr char kShaderSource[] = R"(
Texture2D frameTexture : register(t0);
SamplerState frameSampler : register(s0);

cbuffer FrameParameters : register(b0) {
    float flipVertical;
    float3 padding;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput VertexMain(uint vertexId : SV_VertexID) {
    const float2 coordinates = float2((vertexId << 1) & 2, vertexId & 2);
    VertexOutput output;
    output.position = float4(coordinates.x * 2.0 - 1.0,
                             1.0 - coordinates.y * 2.0, 0.0, 1.0);
    output.uv = coordinates;
    return output;
}

float4 PixelMain(VertexOutput input) : SV_Target {
    const float2 sampleUv = float2(input.uv.x,
        lerp(input.uv.y, 1.0 - input.uv.y, flipVertical));
    return float4(frameTexture.SampleLevel(frameSampler, sampleUv, 0).rgb, 1.0);
}
)";

HWND g_window = nullptr;
HDC g_pendingFramebufferDc = nullptr;
bool g_frameDirty = false;
bool g_frameAvailable = false;
bool g_movieDirty = false;
PresentationSchedule g_schedule;
UINT_PTR g_timer = 0;
HANDLE g_frameLatency = nullptr;
bool g_frameReady = false;
std::vector<std::uint8_t> g_moviePixels;
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<IDXGISwapChain2> g_swapChain;
ComPtr<ID3D11SamplerState> g_sampler;
ComPtr<ID3D11RenderTargetView> g_renderTarget;
ComPtr<ID3D11Texture2D> g_frameTexture;
ComPtr<ID3D11ShaderResourceView> g_frameView;
ComPtr<ID3D11Texture2D> g_movieTexture;
ComPtr<ID3D11ShaderResourceView> g_movieView;
ComPtr<ID3D11Buffer> g_frameParameters;
ComPtr<ID3D11VertexShader> g_vertexShader;
ComPtr<ID3D11PixelShader> g_pixelShader;
UINT g_movieWidth = 0;
UINT g_movieHeight = 0;
bool g_movieActive = false;
bool g_movieFrameAvailable = false;
std::recursive_mutex g_presenterMutex;
Statistics g_statistics;

struct FrameParameters {
    float flipVertical;
    float padding[3];
};

bool CompileShader(const char* entryPoint, const char* profile, ComPtr<ID3DBlob>& bytecode) {
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr,
                                      nullptr, nullptr, entryPoint, profile,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                      &bytecode, &errors);
    if (FAILED(result)) {
        if (errors) {
            OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        }
        return false;
    }
    return true;
}

bool CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> backBuffer;
    return SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) &&
           SUCCEEDED(g_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_renderTarget));
}

bool ReadFramebuffer(HDC framebufferDc, BITMAP& bitmap) {
    const HGDIOBJ bitmapHandle = GetCurrentObject(framebufferDc, OBJ_BITMAP);
    return bitmapHandle &&
           GetObjectW(bitmapHandle, sizeof(bitmap), &bitmap) == sizeof(bitmap) &&
           bitmap.bmWidth == kLogicalWidth && std::abs(bitmap.bmHeight) == kLogicalHeight &&
           bitmap.bmBitsPixel == 16 && bitmap.bmBits;
}

bool UploadFrame(const BITMAP& bitmap) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_context->Map(g_frameTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    const auto* sourceBase = static_cast<const std::uint8_t*>(bitmap.bmBits);
    for (int y = 0; y < kLogicalHeight; ++y) {
        const auto* source = sourceBase + y * bitmap.bmWidthBytes;
        auto* destination = static_cast<std::uint8_t*>(mapped.pData) + y * mapped.RowPitch;
        std::memcpy(destination, source,
                    static_cast<std::size_t>(kLogicalWidth) * sizeof(std::uint16_t));
    }
    g_context->Unmap(g_frameTexture.Get(), 0);
    return true;
}

viewport::Bounds FitSourceToCanvas(UINT width, UINT height) {
    viewport::Bounds bounds{0, 0, kLogicalWidth, kLogicalHeight};
    if (width == 0 || height == 0) {
        return {};
    }
    const long long widthLimitedHeight =
        static_cast<long long>(bounds.width) * height / width;
    if (widthLimitedHeight <= bounds.height) {
        const int fittedHeight = static_cast<int>(widthLimitedHeight);
        bounds.y += (bounds.height - fittedHeight) / 2;
        bounds.height = fittedHeight;
    } else {
        const int fittedWidth = static_cast<int>(
            static_cast<long long>(bounds.height) * width / height);
        bounds.x += (bounds.width - fittedWidth) / 2;
        bounds.width = fittedWidth;
    }
    return bounds;
}

void DrawFrame(ID3D11ShaderResourceView* view, UINT width, UINT height,
               bool flipVertical) {
    constexpr float clearColor[4]{0.0F, 0.0F, 0.0F, 1.0F};
    g_context->OMSetRenderTargets(1, g_renderTarget.GetAddressOf(), nullptr);
    g_context->ClearRenderTargetView(g_renderTarget.Get(), clearColor);

    const viewport::Bounds bounds = FitSourceToCanvas(width, height);
    D3D11_VIEWPORT graphicsViewport{};
    // Express physical letterboxing in the fixed-size buffer. DXGI stretches
    // the buffer to the client; the resulting viewport matches mouse mapping.
    RECT client{};
    GetClientRect(g_window, &client);
    const auto viewport = viewport::Calculate(g_window);
    const float scaleX = static_cast<float>(kLogicalWidth) / client.right;
    const float scaleY = static_cast<float>(kLogicalHeight) / client.bottom;
    graphicsViewport.TopLeftX = (viewport.x + bounds.x * viewport.width /
                                static_cast<float>(kLogicalWidth)) * scaleX;
    graphicsViewport.TopLeftY = (viewport.y + bounds.y * viewport.height /
                                static_cast<float>(kLogicalHeight)) * scaleY;
    graphicsViewport.Width = bounds.width * viewport.width /
                             static_cast<float>(kLogicalWidth) * scaleX;
    graphicsViewport.Height = bounds.height * viewport.height /
                              static_cast<float>(kLogicalHeight) * scaleY;
    graphicsViewport.MinDepth = 0.0F;
    graphicsViewport.MaxDepth = 1.0F;
    g_context->RSSetViewports(1, &graphicsViewport);
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_vertexShader.Get(), nullptr, 0);
    g_context->PSSetShader(g_pixelShader.Get(), nullptr, 0);
    const FrameParameters parameters{flipVertical ? 1.0F : 0.0F, {}};
    g_context->UpdateSubresource(g_frameParameters.Get(), 0, nullptr, &parameters, 0, 0);
    ID3D11Buffer* constantBuffer = g_frameParameters.Get();
    g_context->PSSetConstantBuffers(0, 1, &constantBuffer);
    g_context->PSSetShaderResources(0, 1, &view);
    g_context->PSSetSamplers(0, 1, g_sampler.GetAddressOf());
    g_context->Draw(3, 0);
    ID3D11ShaderResourceView* noResource = nullptr;
    g_context->PSSetShaderResources(0, 1, &noResource);
}

} // namespace

bool Initialize(HWND window) {
    const std::lock_guard lock(g_presenterMutex);
    if (window && g_window == window && g_device && g_context && g_swapChain &&
        g_renderTarget && g_frameTexture && g_frameView && g_frameParameters &&
        g_vertexShader && g_pixelShader && g_sampler && g_frameLatency) {
        return true;
    }
    Shutdown();
    if (!window) {
        return false;
    }

    RECT client{};
    if (!GetClientRect(window, &client)) {
        return false;
    }
    const UINT width = static_cast<UINT>(client.right - client.left);
    const UINT height = static_cast<UINT>(client.bottom - client.top);
    if (width == 0 || height == 0) {
        return false;
    }

    constexpr std::array<D3D_FEATURE_LEVEL, 3> featureLevels{
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL createdFeatureLevel{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels.data(),
                                static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION,
                                &g_device, &createdFeatureLevel, &g_context))) {
        Shutdown();
        return false;
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain1> swapChain;
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = kLogicalWidth;
    description.Height = kLogicalHeight;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (FAILED(g_device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateSwapChainForHwnd(g_device.Get(), window, &description,
                                               nullptr, nullptr, &swapChain)) ||
        FAILED(swapChain.As(&g_swapChain)) ||
        FAILED(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER)) ||
        FAILED(g_swapChain->SetMaximumFrameLatency(1))) {
        Shutdown();
        return false;
    }
    g_frameLatency = g_swapChain->GetFrameLatencyWaitableObject();
    if (!g_frameLatency) {
        Shutdown();
        return false;
    }
    g_window = window;

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_device->CreateSamplerState(&sampler, &g_sampler))) {
        Shutdown();
        return false;
    }

    UINT frameFormatSupport = 0;
    constexpr UINT requiredFrameFormatSupport =
        D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
    if (FAILED(g_device->CheckFormatSupport(kFrameTextureFormat, &frameFormatSupport)) ||
        (frameFormatSupport & requiredFrameFormatSupport) != requiredFrameFormatSupport) {
        Shutdown();
        return false;
    }

    ComPtr<ID3DBlob> vertexBytecode;
    ComPtr<ID3DBlob> pixelBytecode;
    if (!CompileShader("VertexMain", "vs_4_0", vertexBytecode) ||
        !CompileShader("PixelMain", "ps_4_0", pixelBytecode) ||
        FAILED(g_device->CreateVertexShader(vertexBytecode->GetBufferPointer(),
                                            vertexBytecode->GetBufferSize(), nullptr,
                                            &g_vertexShader)) ||
        FAILED(g_device->CreatePixelShader(pixelBytecode->GetBufferPointer(),
                                           pixelBytecode->GetBufferSize(), nullptr,
                                           &g_pixelShader))) {
        Shutdown();
        return false;
    }

    D3D11_TEXTURE2D_DESC textureDescription{};
    textureDescription.Width = kLogicalWidth;
    textureDescription.Height = kLogicalHeight;
    textureDescription.MipLevels = 1;
    textureDescription.ArraySize = 1;
    textureDescription.Format = kFrameTextureFormat;
    textureDescription.SampleDesc.Count = 1;
    textureDescription.Usage = D3D11_USAGE_DYNAMIC;
    textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    textureDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_device->CreateTexture2D(&textureDescription, nullptr, &g_frameTexture)) ||
        FAILED(g_device->CreateShaderResourceView(g_frameTexture.Get(), nullptr, &g_frameView))) {
        Shutdown();
        return false;
    }

    D3D11_BUFFER_DESC parameterDescription{};
    parameterDescription.ByteWidth = sizeof(FrameParameters);
    parameterDescription.Usage = D3D11_USAGE_DEFAULT;
    parameterDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g_device->CreateBuffer(&parameterDescription, nullptr, &g_frameParameters))) {
        Shutdown();
        return false;
    }

    if (!CreateRenderTarget()) {
        Shutdown();
        return false;
    }
    return true;
}

namespace {

bool TryPresent();

void CancelTimer() {
    if (g_timer) {
        KillTimer(g_window, g_timer);
        g_timer = 0;
    }
}

void CALLBACK OnPresentTimer(HWND window, UINT, UINT_PTR timer, DWORD) {
    const std::lock_guard lock(g_presenterMutex);
    if (window != g_window || timer != g_timer) return;
    CancelTimer();
    if (!TryPresent()) {
        OutputDebugStringW(L"San9Toolkit: deferred frame presentation failed.\n");
    }
}

bool ArmTimer() {
    if (g_timer) return true;
    // A window timer executes on the game's window thread, where the DIB is
    // owned. Do not read the live GDI buffer from a rendering worker.
    g_timer = SetTimer(g_window, reinterpret_cast<UINT_PTR>(&g_schedule),
                      g_schedule.DelayMilliseconds(PresentationSchedule::Clock::now()),
                      &OnPresentTimer);
    return g_timer != 0;
}

bool TryPresent() {
    if (!g_schedule.Pending()) return true;
    if (!g_swapChain) return false;
    RECT client{};
    if (!IsWindowVisible(g_window) || IsIconic(g_window) ||
        !GetClientRect(g_window, &client) || client.right <= 0 || client.bottom <= 0) {
        // Restore/paint will resume the retained request. No hidden-window poll.
        CancelTimer();
        return true;
    }
    const auto now = PresentationSchedule::Clock::now();
    if (!g_schedule.Due(now)) return ArmTimer();
    if (!g_frameReady) {
        const DWORD ready = WaitForSingleObject(g_frameLatency, 0);
        if (ready == WAIT_FAILED) return false;
        g_frameReady = ready == WAIT_OBJECT_0;
    }
    if (!g_frameReady) {
        g_schedule.Retry(now);
        return ArmTimer();
    }
    if (g_movieActive) {
        if (!g_movieFrameAvailable) return true;
        if (g_movieDirty) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(g_context->Map(g_movieTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD,
                                      0, &mapped))) return false;
            const std::size_t rowBytes = g_movieWidth * sizeof(std::uint32_t);
            for (UINT y = 0; y < g_movieHeight; ++y) {
                std::memcpy(static_cast<std::uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                            g_moviePixels.data() + y * rowBytes, rowBytes);
            }
            g_context->Unmap(g_movieTexture.Get(), 0);
            ++g_statistics.movieUploads;
            g_movieDirty = false;
        }
        DrawFrame(g_movieView.Get(), g_movieWidth, g_movieHeight, false);
    } else {
        if (g_frameDirty) {
            BITMAP bitmap{};
            // Complete the window thread's GDI writes before reading DIB memory.
            GdiFlush();
            if (!ReadFramebuffer(g_pendingFramebufferDc, bitmap) || !UploadFrame(bitmap)) {
                return false;
            }
            g_frameDirty = false;
            g_frameAvailable = true;
            ++g_statistics.gameUploads;
        }
        if (!g_frameAvailable) return true;
        DrawFrame(g_frameView.Get(), kLogicalWidth, kLogicalHeight, true);
    }
    const HRESULT result = g_swapChain->Present(1, DXGI_PRESENT_DO_NOT_WAIT);
    if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
        g_schedule.Retry(now);
        return ArmTimer();
    }
    if (FAILED(result)) return false;
    ++g_statistics.presentedFrames;
    g_frameReady = false;
    g_schedule.Complete(PresentationSchedule::Clock::now());
    CancelTimer();
    return true;
}

} // namespace

bool QueueFrame(HDC framebufferDc) {
    const std::lock_guard lock(g_presenterMutex);
    if (!framebufferDc || !g_device || !g_window) return false;
    g_pendingFramebufferDc = framebufferDc;
    g_frameDirty = true;
    if (!g_movieActive) g_schedule.Request();
    return true;
}

bool PresentFrame(HDC framebufferDc) {
    const std::lock_guard lock(g_presenterMutex);
    if (!g_frameAvailable && !QueueFrame(framebufferDc)) return false;
    return PresentCurrentFrame();
}

bool PresentPendingFrame() {
    const std::lock_guard lock(g_presenterMutex);
    return TryPresent();
}

bool BeginMovie(HWND window, UINT width, UINT height) {
    const std::lock_guard lock(g_presenterMutex);
    if (!window || width == 0 || height == 0) {
        return false;
    }
    if (!g_device || g_window != window) {
        if (!Initialize(window)) {
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DYNAMIC;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(g_device->CreateTexture2D(&description, nullptr, &texture)) ||
        FAILED(g_device->CreateShaderResourceView(texture.Get(), nullptr, &view))) {
        return false;
    }
    g_movieTexture = std::move(texture);
    g_movieView = std::move(view);
    g_movieWidth = width;
    g_movieHeight = height;
    g_movieFrameAvailable = false;
    g_movieDirty = false;
    CancelTimer();
    g_schedule = {};
    g_movieActive = true;
    return true;
}

bool PresentMovieFrame(const void* pixels, UINT rowPitch) {
    const std::lock_guard lock(g_presenterMutex);
    if (!g_movieActive || !g_movieTexture || !pixels ||
        rowPitch < g_movieWidth * sizeof(std::uint32_t)) {
        return false;
    }
    const std::size_t rowBytes = g_movieWidth * sizeof(std::uint32_t);
    g_moviePixels.resize(rowBytes * g_movieHeight);
    const auto* source = static_cast<const std::uint8_t*>(pixels);
    for (UINT y = 0; y < g_movieHeight; ++y) {
        std::memcpy(g_moviePixels.data() + y * rowBytes, source + y * rowPitch, rowBytes);
    }
    g_movieFrameAvailable = true;
    g_movieDirty = true;
    g_schedule.Request();
    return TryPresent();
}

bool PresentCurrentFrame() {
    const std::lock_guard lock(g_presenterMutex);
    if ((g_movieActive && !g_movieFrameAvailable) ||
        (!g_movieActive && !g_frameAvailable && !g_frameDirty)) return true;
    g_schedule.Request();
    return TryPresent();
}

void EndMovie() {
    const std::lock_guard lock(g_presenterMutex);
    g_movieActive = false;
    g_movieFrameAvailable = false;
    g_movieDirty = false;
    g_movieWidth = 0;
    g_movieHeight = 0;
    g_movieView.Reset();
    g_movieTexture.Reset();
    g_moviePixels.clear();
    if (g_pendingFramebufferDc) {
        QueueFrame(g_pendingFramebufferDc);
        TryPresent();
    }
}

bool IsMovieActive() {
    const std::lock_guard lock(g_presenterMutex);
    return g_movieActive;
}

Statistics GetStatistics() {
    const std::lock_guard lock(g_presenterMutex);
    return g_statistics;
}

void Shutdown() {
    const std::lock_guard lock(g_presenterMutex);
    CancelTimer();
    g_schedule = {};
    g_statistics = {};
    g_frameDirty = false;
    g_frameAvailable = false;
    g_frameReady = false;
    g_movieDirty = false;
    g_moviePixels.clear();
    g_pendingFramebufferDc = nullptr;
    if (g_context) {
        g_context->ClearState();
        g_context->Flush();
    }
    g_pixelShader.Reset();
    g_vertexShader.Reset();
    g_frameParameters.Reset();
    g_movieView.Reset();
    g_movieTexture.Reset();
    g_frameView.Reset();
    g_frameTexture.Reset();
    g_renderTarget.Reset();
    g_swapChain.Reset();
    if (g_frameLatency) {
        CloseHandle(g_frameLatency);
        g_frameLatency = nullptr;
    }
    g_sampler.Reset();
    g_context.Reset();
    g_device.Reset();
    g_movieWidth = 0;
    g_movieHeight = 0;
    g_movieActive = false;
    g_movieFrameAvailable = false;
    g_window = nullptr;
}

} // namespace san9::d3d11_presenter
