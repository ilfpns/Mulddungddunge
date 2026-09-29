#include "Snapshot.h"
#include "WindowTracker.h"

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

    HICON LoadAppIcon(HWND hwnd, int px, bool& owned)
    {
        owned = false;
        auto path = wt::ProcessPath(hwnd);
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

struct Snapshot::Job
{
    HWND hwnd{};
    RECT frame{};
    winrt::Windows::Graphics::SizeInt32 size{};
    GraphicsCaptureItem item{ nullptr };
    Direct3D11CaptureFramePool pool{ nullptr };
    GraphicsCaptureSession session{ nullptr };
    winrt::event_token arrivedToken{};

    // Also breaks the job <-> handler reference cycles.
    void Close()
    {
        if (pool)
        {
            if (arrivedToken)
                pool.FrameArrived(arrivedToken);
            pool.Close();
            pool = nullptr;
        }
        if (session)
        {
            session.Close();
            session = nullptr;
        }
        item = nullptr;
    }
};

std::shared_ptr<Snapshot::Job> Snapshot::Start(HWND hwnd, RECT const& frame)
{
    try
    {
        auto job = std::make_shared<Job>();
        job->hwnd = hwnd;
        job->frame = frame;
        auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(factory->CreateForWindow(hwnd, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(job->item)));
        job->size = job->item.Size();
        job->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(m_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, job->size);
        job->session = job->pool.CreateCaptureSession(job->item);
        try { job->session.IsCursorCaptureEnabled(false); } catch (...) {}
        try { job->session.IsBorderRequired(false); } catch (...) {}
        return job;
    }
    catch (...)
    {
        return nullptr;
    }
}

wuc::CompositionDrawingSurface Snapshot::Render(Job const& job, Direct3D11CaptureFrame const& captured)
{
    try
    {
        // The capture covers the window rect including invisible resize borders; crop to the visible frame.
        RECT const& frame = job.frame;
        RECT wr;
        GetWindowRect(job.hwnd, &wr);
        float fw = static_cast<float>(frame.right - frame.left);
        float fh = static_cast<float>(frame.bottom - frame.top);
        float ox = 0, oy = 0;
        if (job.size.Width != static_cast<int>(fw) || job.size.Height != static_cast<int>(fh))
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
        return surface;
    }
    catch (...)
    {
        return nullptr;
    }
}

void Snapshot::CaptureAsync(HWND hwnd, RECT const& frame, Done done)
{
    auto queue = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
    // Setting up a capture session blocks for ~100ms, so it happens on a short-lived worker thread;
    // the UI thread keeps animating. The capture objects are agile, and only the final drawing into a
    // composition surface comes back to the UI thread.
    std::thread([this, hwnd, frame, queue, done = std::move(done)]() mutable {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto job = Start(hwnd, frame);
        Direct3D11CaptureFrame captured{ nullptr };
        if (job)
        {
            winrt::handle arrived{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
            HANDLE ev = arrived.get();
            job->arrivedToken = job->pool.FrameArrived([ev](auto&&, auto&&) { SetEvent(ev); });
            job->session.StartCapture();
            if (WaitForSingleObject(ev, 400) == WAIT_OBJECT_0)
                captured = job->pool.TryGetNextFrame();
            // `ev` is closed when this block ends; a late frame must not signal it afterwards.
            job->pool.FrameArrived(job->arrivedToken);
            job->arrivedToken = {};
        }
        queue.TryEnqueue([this, job, captured, done = std::move(done)] {
            wuc::CompositionDrawingSurface surface{ nullptr };
            if (job && captured)
                surface = Render(*job, captured);
            if (captured)
                captured.Close();
            if (job)
                job->Close();
            done(surface);
        });
        winrt::uninit_apartment();
    }).detach();
}

namespace
{
    // The window's app icon as a premultiplied WIC source D2D can draw.
    bool IsFrameHost(std::wstring const& path)
    {
        return path.size() >= 24 && _wcsicmp(path.c_str() + path.size() - 24, L"ApplicationFrameHost.exe") == 0;
    }

    // The app icon as a premultiplied WIC source, as large as the app provides (up to `px`), so it can
    // be scaled down crisply to whatever size it is shown at.
    winrt::com_ptr<IWICFormatConverter> IconSource(IWICImagingFactory* wic, HWND hwnd, int px)
    {
        auto path = wt::ProcessPath(hwnd);
        if (!path.empty() && !IsFrameHost(path))
        {
            winrt::com_ptr<IShellItemImageFactory> images;
            HBITMAP hbmp = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(images.put()))) &&
                SUCCEEDED(images->GetImage({ px, px }, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbmp)))
            {
                winrt::com_ptr<IWICBitmap> bitmap;
                winrt::com_ptr<IWICFormatConverter> converter;
                bool ok = SUCCEEDED(wic->CreateBitmapFromHBITMAP(hbmp, nullptr, WICBitmapUsePremultipliedAlpha, bitmap.put())) &&
                          SUCCEEDED(wic->CreateFormatConverter(converter.put())) &&
                          SUCCEEDED(converter->Initialize(bitmap.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                              nullptr, 0.f, WICBitmapPaletteTypeCustom));
                DeleteObject(hbmp);
                if (ok)
                    return converter;
            }
        }
        bool owned = false;
        HICON icon = LoadAppIcon(hwnd, px, owned);
        winrt::com_ptr<IWICBitmap> bitmap;
        winrt::com_ptr<IWICFormatConverter> converter;
        bool ok = SUCCEEDED(wic->CreateBitmapFromHICON(icon, bitmap.put())) &&
                  SUCCEEDED(wic->CreateFormatConverter(converter.put())) &&
                  SUCCEEDED(converter->Initialize(bitmap.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                      nullptr, 0.f, WICBitmapPaletteTypeCustom));
        if (owned)
            DestroyIcon(icon);
        return ok ? converter : nullptr;
    }

    void DrawIcon(ID2D1DeviceContext* dc, IWICFormatConverter* source, D2D1_RECT_F const& dst)
    {
        winrt::com_ptr<ID2D1Bitmap> bitmap;
        if (source && SUCCEEDED(dc->CreateBitmapFromWicBitmap(source, nullptr, bitmap.put())))
            dc->DrawBitmap(bitmap.get(), &dst, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
    }
}

winrt::com_ptr<IWICFormatConverter> Snapshot::CachedIcon(HWND hwnd)
{
    auto path = wt::ProcessPath(hwnd);
    // UWP windows all belong to ApplicationFrameHost; their icons differ per window, so no caching.
    if (path.empty() || IsFrameHost(path))
        return IconSource(m_wic.get(), hwnd, 128);
    for (auto& [app, source] : m_icons)
        if (_wcsicmp(app.c_str(), path.c_str()) == 0)
            return source;
    auto source = IconSource(m_wic.get(), hwnd, 128);
    if (source)
        m_icons.emplace_back(path, source);
    return source;
}

wuc::CompositionDrawingSurface Snapshot::Icon(HWND hwnd, int px)
{
    auto source = CachedIcon(hwnd);
    float size = static_cast<float>(px);
    return Paint(size, size, [&](ID2D1DeviceContext* dc) { DrawIcon(dc, source.get(), { 0, 0, size, size }); });
}

IDWriteFactory* Snapshot::Text()
{
    if (!m_dwrite)
        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwrite.put())));
    return m_dwrite.get();
}

wuc::CompositionDrawingSurface Snapshot::Placeholder(HWND hwnd, float w, float h, float displayWidth)
{
    // Only the top 3:2 part of a card is visible in the sidebar; center the content there.
    float pw = std::round(displayWidth), ph = std::round(pw * h / w);
    float u = pw / 320.f;                           // layout below was designed at 320px wide
    float visible = std::min(ph, pw / 1.5f);
    auto source = CachedIcon(hwnd);

    wchar_t title[128]{};
    GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    winrt::com_ptr<IDWriteTextFormat> format;
    Text()->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 17.f * u, L"ko-kr", format.put());
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    winrt::com_ptr<IDWriteInlineObject> ellipsis;
    Text()->CreateEllipsisTrimmingSign(format.get(), ellipsis.put());
    format->SetTrimming(&trim, ellipsis.get());

    return Paint(pw, ph, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> fill, ink;
        dc->CreateSolidColorBrush(D2D1::ColorF(0.17f, 0.17f, 0.19f), fill.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.72f), ink.put());
        // Grayscale text: the card is tilted and warped by perspective, which turns ClearType's
        // colored subpixel edges into visible fringes.
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        dc->FillRectangle({ 0, 0, pw, ph }, fill.get());
        float cy = std::round(visible / 2.f - 12.f * u), half = std::round(32.f * u);
        DrawIcon(dc, source.get(), { pw / 2 - half, cy - half, pw / 2 + half, cy + half });
        dc->DrawTextW(title, static_cast<UINT32>(wcslen(title)), format.get(),
            { 20 * u, cy + 44 * u, pw - 20 * u, cy + 70 * u }, ink.get());
    });
}

wuc::CompositionDrawingSurface Snapshot::Paint(float w, float h, std::function<void(ID2D1DeviceContext*)> const& draw)
{
    auto surface = m_graphics.CreateDrawingSurface({ w, h }, DirectXPixelFormat::B8G8R8A8UIntNormalized, DirectXAlphaMode::Premultiplied);
    Draw(surface, draw);
    return surface;
}

void Snapshot::CaptureMinimized(HWND hwnd, Done done)
{
    static ATOM hostClass = [] {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"StageManagerThumbnailHost";
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        return RegisterClassExW(&wc);
    }();
    auto queue = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
    auto fail = [queue, done] { queue.TryEnqueue([done] { done(nullptr); }); };
    if (!hostClass)
        return fail();

    HWND host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, L"StageManagerThumbnailHost",
        L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HTHUMBNAIL thumb = nullptr;
    SIZE size{};
    if (!host || FAILED(DwmRegisterThumbnail(host, hwnd, &thumb)) || FAILED(DwmQueryThumbnailSourceSize(thumb, &size)) ||
        size.cx < 32 || size.cy < 32)
    {
        if (thumb)
            DwmUnregisterThumbnail(thumb);
        if (host)
            DestroyWindow(host);
        return fail();
    }
    size.cx = std::min<LONG>(size.cx, 3840);
    size.cy = std::min<LONG>(size.cy, 2400);

    // Parked just outside the visible desktop: never seen, but still composed by DWM for the capture.
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN) - size.cx - 64;
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    SetWindowPos(host, HWND_BOTTOM, x, y, size.cx, size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    DWM_THUMBNAIL_PROPERTIES props{};
    props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
    props.rcDestination = { 0, 0, size.cx, size.cy };
    props.fVisible = TRUE;
    props.opacity = 255;
    props.fSourceClientAreaOnly = FALSE;
    DwmUpdateThumbnailProperties(thumb, &props);
    DwmFlush();

    RECT frame;
    GetWindowRect(host, &frame);
    CaptureAsync(host, frame, [host, thumb, done](wuc::CompositionDrawingSurface const& surface) {
        DwmUnregisterThumbnail(thumb);
        DestroyWindow(host);
        done(surface);
    });
}
