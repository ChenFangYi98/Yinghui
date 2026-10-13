#include "D3DCanvas.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace comp {
using Microsoft::WRL::ComPtr;
struct D3DCanvas::Impl {
    HWND window = nullptr;
    bool software = false;
    QSize size;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Texture2D> texture, captured;
    ComPtr<ID3D11ShaderResourceView> resource;
};
D3DCanvas::D3DCanvas() : impl_(std::make_unique<Impl>()) {}
D3DCanvas::~D3DCanvas() = default;
QImage D3DCanvas::readback() const {
    auto& i = *impl_;
    if (!i.captured || !i.device)
        return {};
    ComPtr<ID3D11Texture2D> surface, staging;
    surface = i.captured;
    D3D11_TEXTURE2D_DESC desc;
    surface->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    if (FAILED(i.device->CreateTexture2D(&desc, nullptr, &staging)))
        return {};
    i.context->CopyResource(staging.Get(), surface.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(i.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return {};
    QImage image(int(desc.Width), int(desc.Height), QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y)
        memcpy(image.scanLine(y), static_cast<const char*>(mapped.pData) + y * mapped.RowPitch,
               image.width() * 4);
    i.context->Unmap(staging.Get(), 0);
    return image;
}
bool D3DCanvas::initialize(quintptr window) {
    impl_ = std::make_unique<Impl>();
    auto& i = *impl_;
    i.window = reinterpret_cast<HWND>(window);
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = i.window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL feature;
    auto hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                            &desc, &i.swapChain, &i.device, &feature, &i.context);
    if (FAILED(hr)) {
        i.software = true;
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                           &desc, &i.swapChain, &i.device, &feature, &i.context);
    }
    if (FAILED(hr))
        return false;
    const char* shader = R"(
        Texture2D image : register(t0); SamplerState sampling : register(s0);
        struct V { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
        V vs(uint id : SV_VertexID) { V v; float2 p = float2((id << 1) & 2, id & 2); v.position = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1); v.uv = p; return v; }
        float4 ps(V v) : SV_TARGET { return image.Sample(sampling, v.uv); }
    )";
    ComPtr<ID3DBlob> vs, ps;
    if (FAILED(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs,
                          nullptr)) ||
        FAILED(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps,
                          nullptr)))
        return false;
    if (FAILED(
            i.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &i.vertex)) ||
        FAILED(i.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &i.pixel)))
        return false;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(i.device->CreateSamplerState(&sampler, &i.sampler));
}
bool D3DCanvas::present(const QImage& input, bool capture) {
    auto& i = *impl_;
    if (!i.device || input.isNull())
        return false;
    const auto image = input.convertToFormat(QImage::Format_RGBA8888);
    if (i.size != image.size() || !i.target) {
        i.context->OMSetRenderTargets(0, nullptr, nullptr);
        i.target.Reset();
        i.texture.Reset();
        i.resource.Reset();
        if (FAILED(i.swapChain->ResizeBuffers(0, image.width(), image.height(), DXGI_FORMAT_UNKNOWN, 0)))
            return false;
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(i.swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) ||
            FAILED(i.device->CreateRenderTargetView(back.Get(), nullptr, &i.target)))
            return false;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = image.width();
        desc.Height = image.height();
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(i.device->CreateTexture2D(&desc, nullptr, &i.texture)) ||
            FAILED(i.device->CreateShaderResourceView(i.texture.Get(), nullptr, &i.resource)))
            return false;
        i.size = image.size();
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(i.context->Map(i.texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    for (int y = 0; y < image.height(); ++y)
        memcpy(static_cast<char*>(mapped.pData) + size_t(y) * mapped.RowPitch, image.constScanLine(y),
               image.width() * 4);
    i.context->Unmap(i.texture.Get(), 0);
    D3D11_VIEWPORT viewport{0, 0, float(image.width()), float(image.height()), 0, 1};
    i.context->RSSetViewports(1, &viewport);
    auto target = i.target.Get();
    i.context->OMSetRenderTargets(1, &target, nullptr);
    i.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    i.context->VSSetShader(i.vertex.Get(), nullptr, 0);
    i.context->PSSetShader(i.pixel.Get(), nullptr, 0);
    auto resource = i.resource.Get();
    auto sampler = i.sampler.Get();
    i.context->PSSetShaderResources(0, 1, &resource);
    i.context->PSSetSamplers(0, 1, &sampler);
    i.context->Draw(3, 0);
    if (capture) {
        ComPtr<ID3D11Texture2D> surface;
        i.swapChain->GetBuffer(0, IID_PPV_ARGS(&surface));
        D3D11_TEXTURE2D_DESC d;
        surface->GetDesc(&d);
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = 0;
        d.CPUAccessFlags = 0;
        i.captured.Reset();
        if (SUCCEEDED(i.device->CreateTexture2D(&d, nullptr, &i.captured)))
            i.context->CopyResource(i.captured.Get(), surface.Get());
    }
    auto hr = i.swapChain->Present(0, 0);
    return SUCCEEDED(hr);
}
QString D3DCanvas::backend() const {
    return impl_->device ? (impl_->software ? "Direct3D 11 WARP" : "Direct3D 11") : "CPU";
}
} // namespace comp
