#include "ui/snapshot.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <winrt/base.h>

#include "ui/renderer.h"

bool SaveSnapshot(const Ride& ride, std::wstring_view status, const std::wstring& path, unsigned width,
                  unsigned height) {
    try {
        winrt::com_ptr<IWICImagingFactory> wic;
        winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(wic.put())));
        winrt::com_ptr<IWICBitmap> bitmap;
        winrt::check_hresult(
            wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bitmap.put()));

        winrt::com_ptr<ID2D1Factory> d2d;
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.put()));
        winrt::com_ptr<IDWriteFactory> dwrite;
        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                 reinterpret_cast<IUnknown**>(dwrite.put())));
        const auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        winrt::com_ptr<ID2D1RenderTarget> target;
        winrt::check_hresult(d2d->CreateWicBitmapRenderTarget(bitmap.get(), props, target.put()));

        Renderer renderer(dwrite.get());
        target->BeginDraw();
        renderer.Draw(target.get(), ride, status);
        winrt::check_hresult(target->EndDraw());

        winrt::com_ptr<IWICStream> stream;
        winrt::check_hresult(wic->CreateStream(stream.put()));
        winrt::check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
        winrt::com_ptr<IWICBitmapEncoder> encoder;
        winrt::check_hresult(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
        winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
        winrt::com_ptr<IWICBitmapFrameEncode> frame;
        winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
        winrt::check_hresult(frame->Initialize(nullptr));
        winrt::check_hresult(frame->SetSize(width, height));
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        winrt::check_hresult(frame->SetPixelFormat(&format));
        winrt::check_hresult(frame->WriteSource(bitmap.get(), nullptr));
        winrt::check_hresult(frame->Commit());
        winrt::check_hresult(encoder->Commit());
        return true;
    } catch (...) {
        return false;
    }
}
