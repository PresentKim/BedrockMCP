#include "capture.hpp"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "game_window.hpp"
#include "swapchain_capture.hpp"

using Microsoft::WRL::ComPtr;

namespace winrt {
using namespace ::winrt::Windows::Graphics::Capture;
using namespace ::winrt::Windows::Graphics::DirectX;
using namespace ::winrt::Windows::Graphics::DirectX::Direct3D11;
} // namespace winrt

namespace capture {
namespace {

void ensure_apartment()
{
    static thread_local bool done = false;
    if (!done)
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        done = true;
    }
}

Result err(const std::string& msg)
{
    return {false, {}, msg};
}

std::string encode_source(IWICImagingFactory* factory, IWICBitmapSource* src)
{
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
        return {};

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)))
        return {};
    encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    encoder->CreateNewFrame(&frame, &props);
    frame->Initialize(props.Get());
    if (FAILED(frame->WriteSource(src, nullptr)))
        return {};
    frame->Commit();
    encoder->Commit();

    HGLOBAL hg = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &hg)))
        return {};
    SIZE_T size = GlobalSize(hg);
    void* data = GlobalLock(hg);
    std::string out(static_cast<const char*>(data), size);
    GlobalUnlock(hg);
    return out;
}

Result encode(BYTE* bgra, UINT pitch, UINT img_w, UINT img_h, WICRect crop, int client_w,
              int client_h, int max_width, int max_height, const Painter& paint)
{
    UINT crop_w = static_cast<UINT>(crop.Width), crop_h = static_cast<UINT>(crop.Height);

    std::vector<BYTE> painted;
    if (paint)
    {
        painted.resize(static_cast<size_t>(crop_w) * 4 * crop_h);
        for (UINT row = 0; row < crop_h; ++row)
            std::memcpy(painted.data() + static_cast<size_t>(row) * crop_w * 4,
                        bgra + static_cast<size_t>(crop.Y + row) * pitch + static_cast<size_t>(crop.X) * 4,
                        static_cast<size_t>(crop_w) * 4);
        paint(painted.data(), crop_w * 4, static_cast<int>(crop_w), static_cast<int>(crop_h));
        bgra = painted.data();
        pitch = crop_w * 4;
        img_w = crop_w;
        img_h = crop_h;
        crop = WICRect{0, 0, static_cast<INT>(crop_w), static_cast<INT>(crop_h)};
    }

    // Scale to fit within (max_width, max_height), preserving aspect, never up.
    UINT out_w = crop_w, out_h = crop_h;
    if (max_width > 0 && max_height > 0)
    {
        double s = static_cast<double>(max_width) / crop_w;
        double sy = static_cast<double>(max_height) / crop_h;
        if (sy < s)
            s = sy;
        if (s > 1.0)
            s = 1.0;
        out_w = static_cast<UINT>(crop_w * s + 0.5);
        out_h = static_cast<UINT>(crop_h * s + 0.5);
        if (out_w < 1)
            out_w = 1;
        if (out_h < 1)
            out_h = 1;
    }

    ComPtr<IWICImagingFactory> factory;
    std::string png;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory))))
    {
        ComPtr<IWICBitmap> full;
        factory->CreateBitmapFromMemory(img_w, img_h, GUID_WICPixelFormat32bppBGRA, pitch,
                                        pitch * img_h, bgra, &full);
        ComPtr<IWICBitmapClipper> clipper;
        factory->CreateBitmapClipper(&clipper);
        IWICBitmapSource* src = nullptr;
        if (full && SUCCEEDED(clipper->Initialize(full.Get(), &crop)))
        {
            src = clipper.Get();
            ComPtr<IWICBitmapScaler> scaler;
            if (out_w != crop_w || out_h != crop_h)
            {
                factory->CreateBitmapScaler(&scaler);
                if (SUCCEEDED(scaler->Initialize(clipper.Get(), out_w, out_h,
                                                 WICBitmapInterpolationModeFant)))
                    src = scaler.Get();
            }
            png = encode_source(factory.Get(), src);
        }
    }

    if (png.empty())
        return err("PNG encoding failed");
    Result r{true, std::move(png), {}};
    r.width = static_cast<int>(out_w);
    r.height = static_cast<int>(out_h);
    r.client_width = client_w;
    r.client_height = client_h;
    return r;
}

Result capture_wgc(HWND hwnd, int max_width, int max_height, const Painter& paint)
{
    try
    {
        ensure_apartment();

        if (!winrt::GraphicsCaptureSession::IsSupported())
            return err("Windows.Graphics.Capture is not supported on this system");

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                     &device, nullptr, &context)))
            return err("D3D11CreateDevice failed");

        ComPtr<IDXGIDevice> dxgi;
        device.As(&dxgi);
        winrt::com_ptr<::IInspectable> inspectable;
        if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put())))
            return err("CreateDirect3D11DeviceFromDXGIDevice failed");
        auto winrt_device = inspectable.as<winrt::IDirect3DDevice>();

        auto interop = winrt::get_activation_factory<winrt::GraphicsCaptureItem,
                                                     ::IGraphicsCaptureItemInterop>();
        winrt::GraphicsCaptureItem item{nullptr};
        if (FAILED(interop->CreateForWindow(
                hwnd, winrt::guid_of<winrt::GraphicsCaptureItem>(), winrt::put_abi(item))))
            return err("CreateForWindow failed");

        auto size = item.Size();
        if (size.Width <= 0 || size.Height <= 0)
            return err("game window has no client area");

        auto pool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrt_device, winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        auto session = pool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(false);
        try
        {
            session.IsBorderRequired(false);
        }
        catch (...)
        {
            // Older Windows builds require the capture border; ignore.
        }
        session.StartCapture();

        winrt::Direct3D11CaptureFrame frame{nullptr};
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline)
        {
            frame = pool.TryGetNextFrame();
            if (frame)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        if (!frame)
        {
            session.Close();
            pool.Close();
            return err("timed out waiting for a frame");
        }

        auto access = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> texture;
        access->GetInterface(IID_PPV_ARGS(&texture));

        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
        {
            session.Close();
            pool.Close();
            return err("CreateTexture2D(staging) failed");
        }
        context->CopyResource(staging.Get(), texture.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        {
            session.Close();
            pool.Close();
            return err("Map(staging) failed");
        }

        UINT img_w = desc.Width, img_h = desc.Height;

        // The WGC image is the whole window (title bar on top). Crop to the client
        // area so the output maps straight to client coordinates.
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int client_w = rc.right, client_h = rc.bottom;
        if (client_w <= 0 || client_w > static_cast<int>(img_w))
            client_w = static_cast<int>(img_w);
        if (client_h <= 0 || client_h > static_cast<int>(img_h))
            client_h = static_cast<int>(img_h);
        WICRect crop{0, static_cast<INT>(img_h) - client_h, client_w, client_h};

        Result r = encode(static_cast<BYTE*>(mapped.pData), mapped.RowPitch, img_w, img_h, crop,
                          client_w, client_h, max_width, max_height, paint);

        context->Unmap(staging.Get(), 0);
        session.Close();
        pool.Close();
        return r;
    }
    catch (const winrt::hresult_error& e)
    {
        return err(winrt::to_string(e.message()));
    }
    catch (...)
    {
        return err("unexpected capture failure");
    }
}

Result capture_swapchain(HWND hwnd, int max_width, int max_height, const Painter& paint)
{
    swapchain_capture::Frame frame;
    std::string error;
    if (!swapchain_capture::grab(frame, error))
        return err(error);

    RECT rc{};
    GetClientRect(hwnd, &rc);
    int client_w = rc.right > 0 ? rc.right : static_cast<int>(frame.width);
    int client_h = rc.bottom > 0 ? rc.bottom : static_cast<int>(frame.height);
    WICRect whole{0, 0, static_cast<INT>(frame.width), static_cast<INT>(frame.height)};
    try
    {
        ensure_apartment();
        return encode(frame.bgra.data(), frame.width * 4, frame.width, frame.height, whole,
                      client_w, client_h, max_width, max_height, paint);
    }
    catch (const winrt::hresult_error& e)
    {
        return err(winrt::to_string(e.message()));
    }
}

} // namespace

Result screenshot(int max_width, int max_height, const Painter& paint)
{
    // WGC device/session creation is not safe to run concurrently from several
    // HTTP worker threads; serialize so overlapping requests cannot deadlock.
    static std::mutex capture_mutex;
    std::lock_guard<std::mutex> lock(capture_mutex);

    HWND hwnd = find_game_window();
    if (!hwnd)
        return err("game window not found");

    Result wgc = capture_wgc(hwnd, max_width, max_height, paint);
    if (wgc.ok)
        return wgc;
    Result fallback = capture_swapchain(hwnd, max_width, max_height, paint);
    if (fallback.ok)
        return fallback;
    return err(wgc.error + "; swapchain fallback: " + fallback.error);
}

} // namespace capture
