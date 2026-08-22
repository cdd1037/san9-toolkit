#include "d3d11_presenter.h"

#include "viewport.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace san9::d3d11_presenter {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int kLogicalWidth = 1024;
constexpr int kLogicalHeight = 768;

constexpr char kShaderSource[] = R"(
Texture2D frameTexture : register(t0);
SamplerState frameSampler : register(s0);

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput VertexMain(uint vertexId : SV_VertexID) {
    const float2 positions[3] = {
        float2(-1.0,  1.0),
        float2( 3.0,  1.0),
        float2(-1.0, -3.0)
    };
    const float2 coordinates[3] = {
        float2(0.0, 0.0),
        float2(2.0, 0.0),
        float2(0.0, 2.0)
    };
    VertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = coordinates[vertexId];
    return output;
}

float4 PixelMain(VertexOutput input) : SV_Target {
    return frameTexture.Sample(frameSampler, input.uv);
}
)";

HWND g_window = nullptr;
UINT g_backBufferWidth = 0;
UINT g_backBufferHeight = 0;
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<IDXGISwapChain> g_swapChain;
ComPtr<ID3D11RenderTargetView> g_renderTarget;
ComPtr<ID3D11Texture2D> g_frameTexture;
ComPtr<ID3D11ShaderResourceView> g_frameView;
ComPtr<ID3D11SamplerState> g_sampler;
ComPtr<ID3D11VertexShader> g_vertexShader;
ComPtr<ID3D11PixelShader> g_pixelShader;

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

bool CreateRenderTarget(UINT width, UINT height) {
    g_renderTarget.Reset();
    if (g_backBufferWidth != 0 || g_backBufferHeight != 0) {
        const HRESULT resized = g_swapChain->ResizeBuffers(0, width, height,
                                                           DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(resized)) {
            return false;
        }
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
        FAILED(g_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_renderTarget))) {
        return false;
    }
    g_backBufferWidth = width;
    g_backBufferHeight = height;
    return true;
}

bool EnsureRenderTarget() {
    RECT client{};
    if (!g_window || !GetClientRect(g_window, &client)) {
        return false;
    }
    const UINT width = static_cast<UINT>(client.right - client.left);
    const UINT height = static_cast<UINT>(client.bottom - client.top);
    if (width == 0 || height == 0) {
        return false;
    }
    if (g_renderTarget && width == g_backBufferWidth && height == g_backBufferHeight) {
        return true;
    }
    return CreateRenderTarget(width, height);
}

bool UploadFrame(HDC framebufferDc) {
    const HGDIOBJ bitmapHandle = GetCurrentObject(framebufferDc, OBJ_BITMAP);
    DIBSECTION section{};
    if (!bitmapHandle ||
        GetObjectW(bitmapHandle, sizeof(section), &section) != sizeof(section) ||
        section.dsBm.bmWidth != kLogicalWidth ||
        section.dsBm.bmHeight != kLogicalHeight ||
        section.dsBm.bmBitsPixel != 16 || !section.dsBm.bmBits ||
        section.dsBmih.biCompression != BI_RGB) {
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_context->Map(g_frameTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    const auto* sourceBase = static_cast<const std::uint8_t*>(section.dsBm.bmBits);
    for (int y = 0; y < kLogicalHeight; ++y) {
        const int sourceY = kLogicalHeight - 1 - y;
        const auto* source = reinterpret_cast<const std::uint16_t*>(
            sourceBase + sourceY * section.dsBm.bmWidthBytes);
        auto* destination = reinterpret_cast<std::uint32_t*>(
            static_cast<std::uint8_t*>(mapped.pData) + y * mapped.RowPitch);
        for (int x = 0; x < kLogicalWidth; ++x) {
            const std::uint16_t pixel = source[x];
            const std::uint32_t red5 = (pixel >> 10) & 0x1F;
            const std::uint32_t green5 = (pixel >> 5) & 0x1F;
            const std::uint32_t blue5 = pixel & 0x1F;
            const std::uint32_t red = (red5 << 3) | (red5 >> 2);
            const std::uint32_t green = (green5 << 3) | (green5 >> 2);
            const std::uint32_t blue = (blue5 << 3) | (blue5 >> 2);
            destination[x] = 0xFF000000u | (red << 16) | (green << 8) | blue;
        }
    }
    g_context->Unmap(g_frameTexture.Get(), 0);
    return true;
}

} // namespace

bool Initialize(HWND window) {
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

    DXGI_SWAP_CHAIN_DESC swapChainDescription{};
    swapChainDescription.BufferDesc.Width = width;
    swapChainDescription.BufferDesc.Height = height;
    swapChainDescription.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapChainDescription.SampleDesc.Count = 1;
    swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDescription.BufferCount = 2;
    swapChainDescription.OutputWindow = window;
    swapChainDescription.Windowed = TRUE;
    swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr std::array<D3D_FEATURE_LEVEL, 4> featureLevels{
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
    };
    D3D_FEATURE_LEVEL createdFeatureLevel{};
    const HRESULT created = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels.data(), static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION,
        &swapChainDescription, &g_swapChain, &g_device, &createdFeatureLevel, &g_context);
    if (FAILED(created)) {
        Shutdown();
        return false;
    }
    g_window = window;
    g_backBufferWidth = 0;
    g_backBufferHeight = 0;

    ComPtr<ID3DBlob> vertexBytecode;
    ComPtr<ID3DBlob> pixelBytecode;
    if (!CompileShader("VertexMain", "vs_4_0_level_9_3", vertexBytecode) ||
        !CompileShader("PixelMain", "ps_4_0_level_9_3", pixelBytecode) ||
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
    textureDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDescription.SampleDesc.Count = 1;
    textureDescription.Usage = D3D11_USAGE_DYNAMIC;
    textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    textureDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_device->CreateTexture2D(&textureDescription, nullptr, &g_frameTexture)) ||
        FAILED(g_device->CreateShaderResourceView(g_frameTexture.Get(), nullptr, &g_frameView))) {
        Shutdown();
        return false;
    }

    D3D11_SAMPLER_DESC samplerDescription{};
    samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_device->CreateSamplerState(&samplerDescription, &g_sampler)) ||
        !CreateRenderTarget(width, height)) {
        Shutdown();
        return false;
    }
    return true;
}

bool Present(HDC framebufferDc) {
    if (!g_device || !g_context || !g_swapChain || !EnsureRenderTarget() ||
        !UploadFrame(framebufferDc)) {
        return false;
    }

    constexpr float clearColor[4]{0.0F, 0.0F, 0.0F, 1.0F};
    g_context->OMSetRenderTargets(1, g_renderTarget.GetAddressOf(), nullptr);
    g_context->ClearRenderTargetView(g_renderTarget.Get(), clearColor);

    const viewport::Bounds bounds = viewport::Calculate(g_window);
    D3D11_VIEWPORT graphicsViewport{};
    graphicsViewport.TopLeftX = static_cast<float>(bounds.x);
    graphicsViewport.TopLeftY = static_cast<float>(bounds.y);
    graphicsViewport.Width = static_cast<float>(bounds.width);
    graphicsViewport.Height = static_cast<float>(bounds.height);
    graphicsViewport.MinDepth = 0.0F;
    graphicsViewport.MaxDepth = 1.0F;
    g_context->RSSetViewports(1, &graphicsViewport);
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_vertexShader.Get(), nullptr, 0);
    g_context->PSSetShader(g_pixelShader.Get(), nullptr, 0);
    g_context->PSSetShaderResources(0, 1, g_frameView.GetAddressOf());
    g_context->PSSetSamplers(0, 1, g_sampler.GetAddressOf());
    g_context->Draw(3, 0);
    ID3D11ShaderResourceView* noResource = nullptr;
    g_context->PSSetShaderResources(0, 1, &noResource);

    const HRESULT presented = g_swapChain->Present(0, 0);
    if (presented == DXGI_ERROR_DEVICE_REMOVED || presented == DXGI_ERROR_DEVICE_RESET) {
        Shutdown();
    }
    return SUCCEEDED(presented);
}

void Shutdown() {
    if (g_context) {
        g_context->ClearState();
        g_context->Flush();
    }
    g_pixelShader.Reset();
    g_vertexShader.Reset();
    g_sampler.Reset();
    g_frameView.Reset();
    g_frameTexture.Reset();
    g_renderTarget.Reset();
    g_swapChain.Reset();
    g_context.Reset();
    g_device.Reset();
    g_backBufferWidth = 0;
    g_backBufferHeight = 0;
    g_window = nullptr;
}

} // namespace san9::d3d11_presenter
