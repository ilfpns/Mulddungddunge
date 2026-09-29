#include "Snapshot.h"

using namespace winrt::Windows::Graphics::Capture;
using winrt::Windows::Graphics::DirectX::DirectXAlphaMode;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

namespace
{
    // Draws into a composition surface; `draw` receives a context already translated to the surface origin.
    template <typename F>
    void Draw(wuc::CompositionDrawingSurface const& surface, F&& draw)
    {
        auto interop = surface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
        winrt::com_ptr<ID2D1DeviceContext> dc;
        POINT offset{};
        if (FAILED(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset)))
            return;
        dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        draw(dc.get());
        interop->EndDraw();
    }

    std::wstring ProcessPath(HWND hwnd)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!proc)
            return {};
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        std::wstring out;
        if (QueryFullProcessImageNameW(proc, 0, path, &len))
            out.assign(path, len);
        CloseHandle(proc);
        return out;
    }

    HICON LoadAppIcon(HWND hwnd, int px, bool& owned)
    {
        owned = false;
        auto path = ProcessPath(hwnd);
        // UWP apps are hosted by ApplicationFrameHost, whose exe icon is generic; ask the window instead.
        bool frameHost = path.size() >= 24 && _wcsicmp(path.c_str() + path.size() - 24, L"ApplicationFrameHost.exe") == 0;
        if (!path.empty() && !frameHost)
        {
            HICON icon = nullptr;
            if (SUCCEEDED(SHDefExtractIconW(path.c_str(), 0, 0, &icon, nullptr, MAKELONG(px, 16))) && icon)
            {
                owned = true;
                return icon;
            }
        }
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 50, &result) && result)
            return reinterpret_cast<HICON>(result);
        if (auto icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON)))
            return icon;
        return LoadIconW(nullptr, IDI_APPLICATION);
    }
}

void Snapshot::Init(wuc::Compositor const& compositor)
{
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, m_d3d.put(), nullptr, nullptr));
    // Composition may touch the device from its own thread.
    if (auto mt = m_d3d.try_as<ID3D11Multithread>())
        mt->SetMultithreadProtected(TRUE);

    auto dxgi = m_d3d.as<IDXGIDevice>();
    winrt::com_ptr<ID2D1Factory1> factory;
    D2D1_FACTORY_OPTIONS fo{};
    winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &fo, factory.put_void()));
    winrt::check_hresult(factory->CreateDevice(dxgi.get(), m_d2d.put()));

    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(),
        reinterpret_cast<IInspectable**>(winrt::put_abi(m_device))));

    auto interop = compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>();
    winrt::check_hresult(interop->CreateGraphicsDevice(m_d2d.get(),
        reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(m_graphics))));

    winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IWICImagingFactory), m_wic.put_void()));
}

wuc::CompositionDrawingSurface Snapshot::Capture(HWND hwnd, RECT const& frame)
{
    try
    {
        auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        GraphicsCaptureItem item{ nullptr };
        winrt::check_hresult(factory->CreateForWindow(hwnd, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));

        auto itemSize = item.Size();
        auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(m_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, itemSize);
        auto session = pool.CreateCaptureSession(item);
        try { session.IsCursorCaptureEnabled(false); } catch (...) {}
        try { session.IsBorderRequired(false); } catch (...) {}
        session.StartCapture();

        Direct3D11CaptureFrame captured{ nullptr };
        for (int i = 0; i < 100 && !captured; ++i)
        {
            captured = pool.TryGetNextFrame();
            if (!captured)
                Sleep(2);
        }
        if (!captured)
        {
            session.Close();
            pool.Close();
            return nullptr;
        }

        // The capture covers the window rect including invisible resize borders; crop to the visible frame.
        RECT wr;
        GetWindowRect(hwnd, &wr);
        float fw = static_cast<float>(frame.right - frame.left);
        float fh = static_cast<float>(frame.bottom - frame.top);
        float ox = 0, oy = 0;
        if (itemSize.Width != static_cast<int>(fw) || itemSize.Height != static_cast<int>(fh))
        {
            ox = static_cast<float>(frame.left - wr.left);
            oy = static_cast<float>(frame.top - wr.top);
        }
        auto content = captured.ContentSize();
        D2D1_RECT_F src{ ox, oy, std::min(ox + fw, static_cast<float>(content.Width)), std::min(oy + fh, static_cast<float>(content.Height)) };

        float tw = std::min(kMaxWidth, fw);
        float th = tw * fh / fw;
        auto surface = m_graphics.CreateDrawingSurface({ tw, th }, DirectXPixelFormat::B8G8R8A8UIntNormalized, DirectXAlphaMode::Premultiplied);

        auto access = captured.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<IDXGISurface> dxgiSurface;
        winrt::check_hresult(access->GetInterface(__uuidof(IDXGISurface), dxgiSurface.put_void()));

        Draw(surface, [&](ID2D1DeviceContext* dc) {
            winrt::com_ptr<ID2D1Bitmap1> bitmap;
            auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            if (SUCCEEDED(dc->CreateBitmapFromDxgiSurface(dxgiSurface.get(), &props, bitmap.put())))
            {
                D2D1_RECT_F dst{ 0, 0, tw, th };
                dc->DrawBitmap(bitmap.get(), &dst, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src, nullptr);
            }
        });

        captured.Close();
        session.Close();
        pool.Close();
        return surface;
    }
    catch (...)
    {
        return nullptr;
    }
}

wuc::CompositionDrawingSurface Snapshot::Icon(HWND hwnd, int px)
{
    bool owned = false;
    HICON icon = LoadAppIcon(hwnd, px, owned);
    auto surface = m_graphics.CreateDrawingSurface({ static_cast<float>(px), static_cast<float>(px) },
        DirectXPixelFormat::B8G8R8A8UIntNormalized, DirectXAlphaMode::Premultiplied);

    winrt::com_ptr<IWICBitmap> wicBitmap;
    winrt::com_ptr<IWICFormatConverter> converter;
    if (SUCCEEDED(m_wic->CreateBitmapFromHICON(icon, wicBitmap.put())) &&
        SUCCEEDED(m_wic->CreateFormatConverter(converter.put())) &&
        SUCCEEDED(converter->Initialize(wicBitmap.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
            nullptr, 0.f, WICBitmapPaletteTypeCustom)))
    {
        Draw(surface, [&](ID2D1DeviceContext* dc) {
            winrt::com_ptr<ID2D1Bitmap> bitmap;
            if (SUCCEEDED(dc->CreateBitmapFromWicBitmap(converter.get(), nullptr, bitmap.put())))
            {
                D2D1_RECT_F dst{ 0, 0, static_cast<float>(px), static_cast<float>(px) };
                dc->DrawBitmap(bitmap.get(), &dst, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
            }
        });
    }
    if (owned)
        DestroyIcon(icon);
    return surface;
}
