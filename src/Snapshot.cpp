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
        // So is a browser web app whose icon file wasn't found: the window has the app's icon.
        bool frameHost = path.size() >= 24 && _wcsicmp(path.c_str() + path.size() - 24, L"ApplicationFrameHost.exe") == 0;
        if (!path.empty() && !frameHost && wt::AppId(hwnd) == path)
        {
            auto file = wt::IconFile(hwnd);           // a terminal's shell rather than the terminal
            HICON icon = nullptr;
            if (SUCCEEDED(SHDefExtractIconW(file.c_str(), 0, 0, &icon, nullptr, MAKELONG(px, 16))) && icon)
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

void Snapshot::CreateDevices()
{
    m_d3d = nullptr;
    m_d2d = nullptr;
    m_device = nullptr;
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
}

bool Snapshot::Recover()
{
    if (!m_d3d || m_d3d->GetDeviceRemovedReason() == S_OK)
        return false;
    try
    {
        CreateDevices();
        auto interop = m_graphics.as<ABI::Windows::UI::Composition::ICompositionGraphicsDeviceInterop>();
        winrt::check_hresult(interop->SetRenderingDevice(m_d2d.get()));
        return true;
    }
    catch (...)
    {
        return false;                               // the GPU isn't back yet: tried again next time
    }
}

void Snapshot::Init(wuc::Compositor const& compositor)
{
    CreateDevices();
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
        if (m_d3d && m_d3d->GetDeviceRemovedReason() != S_OK)
            return nullptr;                         // Stage::RecoverDevice makes a new one first
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
        if (!GetWindowRect(job.hwnd, &wr))
            return nullptr;                         // gone meanwhile: no offsets to crop with
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

        float tw = std::min(m_maxWidth, fw);
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
        std::shared_ptr<Job> job;
        Direct3D11CaptureFrame captured{ nullptr };
        // An exception escaping this thread would end the whole process: a window closing mid-capture
        // just means no picture.
        try
        {
            job = Start(hwnd, frame);
            if (job)
            {
                // Shared with the handler, so a frame arriving after it was removed (the pool's thread
                // may already be inside it) still signals a live event.
                auto arrived = std::make_shared<winrt::handle>(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                job->arrivedToken = job->pool.FrameArrived([arrived](auto&&, auto&&) { SetEvent(arrived->get()); });
                job->session.StartCapture();
                if (WaitForSingleObject(arrived->get(), 400) == WAIT_OBJECT_0)
                    captured = job->pool.TryGetNextFrame();
                job->pool.FrameArrived(job->arrivedToken);
                job->arrivedToken = {};
            }
        }
        catch (...)
        {
            captured = nullptr;
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
        // A browser web app (YouTube, GitHub...): its own icon, not the browser's.
        auto webIcon = wt::WebAppIcon(hwnd);
        if (!webIcon.empty())
        {
            winrt::com_ptr<IWICBitmapDecoder> decoder;
            winrt::com_ptr<IWICBitmapFrameDecode> frame;
            winrt::com_ptr<IWICBitmap> bitmap;
            winrt::com_ptr<IWICFormatConverter> converter;
            // Decoded once into memory, so the file isn't held open while the icon is cached.
            if (SUCCEEDED(wic->CreateDecoderFromFilename(webIcon.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put())) &&
                SUCCEEDED(decoder->GetFrame(0, frame.put())) &&
                SUCCEEDED(wic->CreateBitmapFromSource(frame.get(), WICBitmapCacheOnLoad, bitmap.put())) &&
                SUCCEEDED(wic->CreateFormatConverter(converter.put())) &&
                SUCCEEDED(converter->Initialize(bitmap.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                    nullptr, 0.f, WICBitmapPaletteTypeCustom)))
                return converter;
        }
        auto path = wt::ProcessPath(hwnd);
        auto file = wt::IconFile(hwnd);               // a terminal's shell rather than the terminal
        // Store apps live under the locked-down WindowsApps folder, and the shell draws a padlock over
        // their file icons; the icon taken from the executable itself (LoadAppIcon) has none.
        bool packaged = file.find(L"\\WindowsApps\\") != std::wstring::npos;
        if (!path.empty() && !packaged && !IsFrameHost(path) && wt::AppId(hwnd) == path)
        {
            winrt::com_ptr<IShellItemImageFactory> images;
            HBITMAP hbmp = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(images.put()))) &&
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
    auto path = wt::AppId(hwnd);
    // A UWP frame's icon comes from the window and may not be set yet right after it opens: asked
    // again each time rather than caching a generic one.
    if (path.empty() || IsFrameHost(wt::ProcessPath(hwnd)))
        return IconSource(m_wic.get(), hwnd, 128);
    // Cached by the file the icon comes from: a terminal's windows differ by the shell they show.
    if (path.find(L'#') == std::wstring::npos)
        path = wt::IconFile(hwnd);
    for (auto& [app, source] : m_icons)
        if (_wcsicmp(app.c_str(), path.c_str()) == 0)
            return source;
    auto source = IconSource(m_wic.get(), hwnd, 128);
    if (source)
        m_icons.emplace_back(path, source);
    return source;
}

namespace
{
    void ToHsv(float r, float g, float b, float& h, float& s, float& v)
    {
        float hi = std::max({ r, g, b }), lo = std::min({ r, g, b }), d = hi - lo;
        v = hi;
        s = hi > 0.f ? d / hi : 0.f;
        if (d <= 0.f)
            h = 0.f;
        else if (hi == r)
            h = 60.f * std::fmod((g - b) / d + 6.f, 6.f);
        else if (hi == g)
            h = 60.f * ((b - r) / d + 2.f);
        else
            h = 60.f * ((r - g) / d + 4.f);
    }

    void FromHsv(float h, float s, float v, float& r, float& g, float& b)
    {
        float c = v * s, x = c * (1.f - std::fabs(std::fmod(h / 60.f, 2.f) - 1.f)), m = v - c;
        int k = static_cast<int>(h / 60.f) % 6;
        float rgb[6][3] = { { c, x, 0 }, { x, c, 0 }, { 0, c, x }, { 0, x, c }, { x, 0, c }, { c, 0, x } };
        r = rgb[k][0] + m;
        g = rgb[k][1] + m;
        b = rgb[k][2] + m;
    }

    // The hue most of the logo's colorful pixels share (a many-colored logo gives its biggest color,
    // not a muddy mix), averaged over those pixels, then made more saturated and bright.
    uint32_t MainColor(IWICBitmapSource* icon)
    {
        UINT w = 0, h = 0;
        if (!icon || FAILED(icon->GetSize(&w, &h)) || !w || !h || w > 256 || h > 256)
            return 0;
        std::vector<BYTE> px(static_cast<size_t>(w) * h * 4);
        if (FAILED(icon->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data())))
            return 0;
        constexpr int kBins = 36;                   // 10 degrees of hue each
        float weight[kBins]{}, sum[kBins][3]{};
        int opaque = 0, colorful = 0;
        for (size_t i = 0; i < px.size(); i += 4)
        {
            BYTE a = px[i + 3];
            if (a < 128)
                continue;
            ++opaque;
            // Premultiplied BGRA.
            float b = px[i] / static_cast<float>(a), g = px[i + 1] / static_cast<float>(a), r = px[i + 2] / static_cast<float>(a);
            float hue, sat, val;
            ToHsv(r, g, b, hue, sat, val);
            if (sat < 0.3f || val < 0.35f)
                continue;                           // gray, white, black, shadow, dark tints
            ++colorful;
            int bin = std::min(kBins - 1, static_cast<int>(hue / (360.f / kBins)));
            float wgt = sat * val;
            weight[bin] += wgt;
            sum[bin][0] += r * wgt;
            sum[bin][1] += g * wgt;
            sum[bin][2] += b * wgt;
        }
        if (!opaque || colorful < opaque / 12)
            return 0;                               // hardly any color: a black/white/gray logo
        int best = 0;
        float bestWeight = -1.f;
        for (int k = 0; k < kBins; ++k)
        {
            float around = weight[(k + kBins - 1) % kBins] + weight[k] + weight[(k + 1) % kBins];
            if (around > bestWeight)
                best = k, bestWeight = around;
        }
        float total = 0.f, r = 0.f, g = 0.f, b = 0.f;
        for (int k : { (best + kBins - 1) % kBins, best, (best + 1) % kBins })
        {
            total += weight[k];
            r += sum[k][0];
            g += sum[k][1];
            b += sum[k][2];
        }
        if (total <= 0.f)
            return 0;
        float hue, sat, val;
        ToHsv(r / total, g / total, b / total, hue, sat, val);
        sat = std::min(1.f, sat + 0.15f);           // a little more vivid
        val = std::max(val, 0.9f);                  // and bright enough to show as a hairline
        FromHsv(hue, sat, val, r, g, b);
        auto to8 = [](float c) { return static_cast<uint32_t>(std::lround(std::clamp(c, 0.f, 1.f) * 255.f)); };
        return 0xFF000000u | to8(r) << 16 | to8(g) << 8 | to8(b);
    }
}

uint32_t Snapshot::LogoColor(HWND hwnd)
{
    auto path = wt::AppId(hwnd);
    bool cacheable = !path.empty() && !IsFrameHost(wt::ProcessPath(hwnd));
    if (cacheable && path.find(L'#') == std::wstring::npos)
        path = wt::IconFile(hwnd);
    if (cacheable)
        for (auto& [app, color] : m_logoColors)
            if (_wcsicmp(app.c_str(), path.c_str()) == 0)
                return color;
    auto source = CachedIcon(hwnd);
    uint32_t color = MainColor(source.get());
    if (cacheable && source)
        m_logoColors.emplace_back(path, color);
    return color;
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

wuc::CompositionDrawingSurface Snapshot::Placeholder(HWND hwnd, float w, float h, float displayWidth, bool glass)
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
        dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, glass ? 0.92f : 0.72f), ink.put());
        // Grayscale text: the card is tilted and warped by perspective, which turns ClearType's
        // colored subpixel edges into visible fringes.
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (!glass)
            dc->FillRectangle({ 0, 0, pw, ph }, fill.get());
        else
        {
            // Glass over the blurred backdrop: a light veil for contrast, a sheen fading down from the
            // top edge, and a bright rim that is strongest at the top.
            auto gradient = [&](D2D1_POINT_2F from, D2D1_POINT_2F to, std::initializer_list<D2D1_GRADIENT_STOP> stops) {
                winrt::com_ptr<ID2D1GradientStopCollection> collection;
                dc->CreateGradientStopCollection(stops.begin(), static_cast<UINT32>(stops.size()), collection.put());
                winrt::com_ptr<ID2D1LinearGradientBrush> brush;
                dc->CreateLinearGradientBrush({ from, to }, collection.get(), brush.put());
                return brush;
            };
            dc->FillRectangle({ 0, 0, pw, ph }, gradient({ 0, 0 }, { 0, visible }, {
                { 0.f, D2D1::ColorF(1.f, 1.f, 1.f, 0.16f) }, { 1.f, D2D1::ColorF(0.05f, 0.05f, 0.08f, 0.22f) } }).get());
            dc->FillRectangle({ 0, 0, pw, visible * 0.42f }, gradient({ 0, 0 }, { 0, visible * 0.42f }, {
                { 0.f, D2D1::ColorF(1.f, 1.f, 1.f, 0.20f) }, { 1.f, D2D1::ColorF(1.f, 1.f, 1.f, 0.f) } }).get());
            float rim = std::max(1.f, 1.5f * u);
            dc->DrawRectangle({ rim / 2, rim / 2, pw - rim / 2, visible - rim / 2 }, gradient({ 0, 0 }, { pw * 0.35f, visible }, {
                { 0.f, D2D1::ColorF(1.f, 1.f, 1.f, 0.70f) }, { 0.55f, D2D1::ColorF(1.f, 1.f, 1.f, 0.18f) },
                { 1.f, D2D1::ColorF(1.f, 1.f, 1.f, 0.38f) } }).get(), rim);
        }
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

void Snapshot::Repaint(wuc::CompositionDrawingSurface const& surface, std::function<void(ID2D1DeviceContext*)> const& draw)
{
    Draw(surface, draw);
}

void Snapshot::DrawAppIcon(ID2D1DeviceContext* dc, HWND hwnd, D2D1_RECT_F const& dst)
{
    auto source = CachedIcon(hwnd);
    DrawIcon(dc, source.get(), dst);
}

void Snapshot::SetQuality(int quality)
{
    static constexpr float widths[] = { 300.f, 440.f, 640.f };
    m_maxWidth = widths[std::clamp(quality, 0, 2)];
}

UINT64 Snapshot::GpuMemory() const
{
    winrt::com_ptr<IDXGIAdapter> adapter;
    if (!m_d3d || FAILED(m_d3d.as<IDXGIDevice>()->GetAdapter(adapter.put())))
        return 0;
    auto adapter3 = adapter.try_as<IDXGIAdapter3>();
    if (!adapter3)
        return 0;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
    adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
    adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
    return local.CurrentUsage + shared.CurrentUsage;
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
