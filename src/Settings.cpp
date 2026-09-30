// The settings panel: drawn with Direct2D into one composition surface, redrawn only when something
// changes (a click, the pointer moving onto another control). Nothing runs while it is closed.
#include "Stage.h"
#include "WindowTracker.h"
#include "../res/version.h"

namespace
{
    constexpr float kPanelW = 760.f;
    constexpr float kPanelH = 548.f;
    constexpr float kNavW = 176.f;
    constexpr float kRowH = 62.f;
    constexpr float kListRowH = 52.f;

    // Hit targets.
    enum : int { HitClose = 1, HitTab, HitToggle, HitChoice, HitStep, HitApp, HitUnpin, HitUnpinAll, HitGitHub, HitRefresh, HitQuitApp };
    // What a control changes (Hit::arg for toggles; arg / 16 for choices and steps).
    enum : int
    {
        OptAutostart, OptHotkeys, OptHotkeyMod, OptCards, OptSide, OptMonitor, OptTilt, OptSize, OptSpeed,
        OptQuality, OptIcons, OptAutoTuck, OptEdge, OptFit, OptTray, OptTrace, OptAlerts, OptSounds, OptCardStyle,
    };

    wchar_t const* const kTabs[] = { L"일반", L"모양", L"동작", L"앱", L"고정", L"정보" };
    wchar_t const* const kTabGlyphs[] = { L"\xE713", L"\xE790", L"\xE945", L"\xE71D", L"\xE718", L"\xE946" };

    // Virtual desktop number (1-based) of a desktop id, from Explorer's list; 0 if unknown.
    int DesktopNumber(GUID const& id)
    {
        DWORD size = 0;
        wchar_t const* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops";
        if (RegGetValueW(HKEY_CURRENT_USER, key, L"VirtualDesktopIDs", RRF_RT_REG_BINARY, nullptr, nullptr, &size) != ERROR_SUCCESS)
            return 0;
        std::vector<GUID> ids(size / sizeof(GUID));
        if (ids.empty() || RegGetValueW(HKEY_CURRENT_USER, key, L"VirtualDesktopIDs", RRF_RT_REG_BINARY, nullptr, ids.data(), &size) != ERROR_SUCCESS)
            return 0;
        for (size_t i = 0; i < ids.size(); ++i)
            if (ids[i] == id)
                return static_cast<int>(i) + 1;
        return 0;
    }

    HMONITOR MonitorByDevice(std::wstring const& name)
    {
        struct Ctx { std::wstring const* name; HMONITOR found; } ctx{ &name, nullptr };
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto c = reinterpret_cast<Ctx*>(lp);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi) && *c->name == mi.szDevice)
            {
                c->found = mon;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        return ctx.found;
    }

    std::wstring Megabytes(UINT64 bytes)
    {
        wchar_t text[32];
        swprintf_s(text, L"%.1f MB", bytes / (1024.0 * 1024.0));
        return text;
    }
}

void Stage::OpenSettings()
{
    if (m_settings.open)
        return;
    int tab = m_settings.tab;
    m_settings = {};
    m_settings.open = true;
    m_settings.tab = tab;
    float2 screen{ static_cast<float>(m_monitor.right - m_monitor.left), static_cast<float>(m_monitor.bottom - m_monitor.top) };
    float w = std::round(S(kPanelW)), h = std::round(S(kPanelH));
    m_settings.size = { w, h };
    m_settings.origin = { std::round((screen.x - w) / 2.f), std::round((screen.y - h) / 2.f) };

    auto dwrite = m_snap.Text();
    auto format = [&](wchar_t const* family, DWRITE_FONT_WEIGHT weight, float size, winrt::com_ptr<IDWriteTextFormat>& out) {
        dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, S(size), L"ko-kr", out.put());
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    format(L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 20.f, m_settings.title);
    format(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 14.f, m_settings.body);
    format(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 12.f, m_settings.caption);
    format(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 13.f, m_settings.value);
    m_settings.value->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    if (FAILED(dwrite->CreateTextFormat(L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, S(15.f), L"", m_settings.icon.put())))
        dwrite->CreateTextFormat(L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, S(15.f), L"", m_settings.icon.put());
    m_settings.icon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_settings.icon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // Long descriptions are cut with an ellipsis rather than running into the controls.
    for (auto* f : { m_settings.body.get(), m_settings.caption.get() })
    {
        DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        winrt::com_ptr<IDWriteInlineObject> ellipsis;
        dwrite->CreateEllipsisTrimmingSign(f, ellipsis.put());
        f->SetTrimming(&trim, ellipsis.get());
    }

    LoadSettingsLists();
    m_settings.surface = m_snap.Paint(w, h, [](ID2D1DeviceContext*) {});
    PaintSettings();

    auto rounded = [&](float2 size, float radius) {
        auto g = m_compositor.CreateRoundedRectangleGeometry();
        g.Size(size);
        g.CornerRadius({ radius, radius });
        return m_compositor.CreateGeometricClip(g);
    };
    m_settings.root = m_compositor.CreateContainerVisual();
    m_settings.root.Size(screen);

    auto backdrop = m_compositor.CreateSpriteVisual();              // dims everything behind the panel
    backdrop.Size(screen);
    backdrop.Brush(m_compositor.CreateColorBrush({ 90, 0, 0, 0 }));
    m_settings.root.Children().InsertAtTop(backdrop);

    auto panel = m_compositor.CreateContainerVisual();
    panel.Size({ w, h });
    panel.Offset({ m_settings.origin.x, m_settings.origin.y, 0.f });
    panel.CenterPoint({ w / 2.f, h / 2.f, 0.f });
    auto shadow = m_compositor.CreateDropShadow();
    shadow.BlurRadius(S(40));
    shadow.Opacity(0.5f);
    shadow.Offset({ 0.f, S(10), 0.f });
    auto shadowHost = m_compositor.CreateSpriteVisual();
    shadowHost.Size({ w - S(16), h - S(16) });
    shadowHost.Offset({ S(8), S(8), 0.f });
    shadowHost.Shadow(shadow);
    panel.Children().InsertAtTop(shadowHost);
    auto border = m_compositor.CreateSpriteVisual();
    border.Size({ w, h });
    border.Brush(m_compositor.CreateColorBrush({ 40, 255, 255, 255 }));
    border.Clip(rounded({ w, h }, S(14)));
    panel.Children().InsertAtTop(border);
    auto fill = m_compositor.CreateSpriteVisual();
    fill.Size({ w - 2.f, h - 2.f });
    fill.Offset({ 1.f, 1.f, 0.f });
    fill.Brush(m_compositor.CreateColorBrush({ 250, 32, 32, 36 }));
    fill.Clip(rounded({ w - 2.f, h - 2.f }, S(14) - 1.f));
    panel.Children().InsertAtTop(fill);
    auto layer = [&] {
        auto v = m_compositor.CreateContainerVisual();
        v.Size({ w, h });
        panel.Children().InsertAtTop(v);
        return v;
    };
    m_settings.under = layer();
    m_settings.listUnder = layer();
    auto content = m_compositor.CreateSpriteVisual();
    content.Size({ w, h });
    content.Brush(m_compositor.CreateSurfaceBrush(m_settings.surface));
    panel.Children().InsertAtTop(content);
    m_settings.over = layer();
    m_settings.listOver = layer();
    SyncSettingsVisuals();                          // the first paint ran before the layers existed
    m_settings.root.Children().InsertAtTop(panel);
    m_root.Children().InsertAtTop(m_settings.root);

    auto fade = m_compositor.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(0.f, 0.f);
    fade.InsertKeyFrame(1.f, 1.f, m_ease);
    fade.Duration(std::chrono::milliseconds(160));
    m_settings.root.StartAnimation(L"Opacity", fade);
    auto grow = m_compositor.CreateVector3KeyFrameAnimation();
    grow.InsertKeyFrame(0.f, { 0.96f, 0.96f, 1.f });
    grow.InsertKeyFrame(1.f, { 1.f, 1.f, 1.f }, m_ease);
    grow.Duration(std::chrono::milliseconds(200));
    panel.StartAnimation(L"Scale", grow);

    SetHover(-1);
    GrowView();
}

void Stage::CloseSettings()
{
    if (!m_settings.open)
        return;
    auto root = m_settings.root;
    int tab = m_settings.tab;
    m_settings = {};
    m_settings.tab = tab;                           // reopens where it was left
    auto fade = m_compositor.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(1.f, 0.f);
    fade.Duration(std::chrono::milliseconds(120));
    auto batch = m_compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    root.StartAnimation(L"Opacity", fade);
    batch.End();
    batch.Completed([this, root](auto&&, auto&&) {
        m_root.Children().Remove(root);         // releases the drawn contents
        TrimMemory();
        if (!m_busy && !m_dragging && !m_menu.open && !m_settings.open)
        {
            ShrinkView();
            UpdateDock();                       // settings like auto-hide apply now
        }
    });
}

bool Stage::InSettingsPanel(POINT pt) const
{
    return pt.x >= m_settings.origin.x && pt.x <= m_settings.origin.x + m_settings.size.x &&
           pt.y >= m_settings.origin.y && pt.y <= m_settings.origin.y + m_settings.size.y;
}

// Apps with windows open right now (any desktop, any monitor) plus the excluded ones; monitors; usage.
void Stage::LoadSettingsLists()
{
    auto& m = m_settings;
    m.apps.clear();
    struct Ctx { std::vector<SettingsApp>* apps; } ctx{ &m.apps };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto apps = reinterpret_cast<Ctx*>(lp)->apps;
        if (!wt::IsAppWindow(h, nullptr, true))
            return TRUE;
        auto id = wt::AppId(h);
        if (!id.empty() && std::none_of(apps->begin(), apps->end(), [&](auto& a) { return wt::SameApp(a.id, id); }))
            apps->push_back({ id, wt::AppLabel(id, h), h });
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    for (auto& id : m_cfg.excluded)
        if (std::none_of(m.apps.begin(), m.apps.end(), [&](auto& a) { return wt::SameApp(a.id, id); }))
            m.apps.push_back({ id, wt::AppLabel(id, nullptr), nullptr });
    std::sort(m.apps.begin(), m.apps.end(), [](auto& a, auto& b) { return _wcsicmp(a.label.c_str(), b.label.c_str()) < 0; });

    m.monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi))
            reinterpret_cast<std::vector<std::wstring>*>(lp)->push_back(mi.szDevice);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&m.monitors));

    PROCESS_MEMORY_COUNTERS_EX2 pmc{ sizeof(pmc) };
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        m.ram = pmc.PrivateWorkingSetSize;
    m.gpu = m_snap.GpuMemory();
}

void Stage::PaintSettings()
{
    auto& m = m_settings;
    if (!m.open || !m.surface)
        return;
    m.hits.clear();
    m.toggleDescs.clear();
    m.choiceDescs.clear();
    m.listRect = {};
    float w = m.size.x, h = m.size.y;
    float x0 = S(kNavW + 20), x1 = w - S(28);       // content columns

    m_snap.Repaint(m.surface, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> ink, dim, faint, line, hover, control, selected, gold;
        dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.95f), ink.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.58f), dim.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.36f), faint.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.09f), line.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.07f), hover.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.06f), control.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.17f), selected.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 0.87f, 0.58f), gold.put());       // text on gold marks
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        auto text = [&](std::wstring const& s, D2D1_RECT_F r, IDWriteTextFormat* f, ID2D1Brush* b) {
            dc->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), f, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };
        auto round = [&](D2D1_RECT_F r, float radius, ID2D1Brush* b) { dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b); };
        // Registers a target; true if the pointer is on it.
        auto hit = [&](D2D1_RECT_F r, int id, int arg) {
            m.hits.push_back({ r, id, arg });
            return m.hover == id && m.hoverArg == arg;
        };

        // Header.
        text(L"설정", { S(24), S(12), w - S(80), S(58) }, m.title.get(), ink.get());
        D2D1_RECT_F close{ w - S(54), S(13), w - S(14), S(53) };
        if (hit(close, HitClose, 0))
            round(close, S(8), hover.get());
        text(L"\xE8BB", close, m.icon.get(), dim.get());
        dc->FillRectangle({ S(20), S(64), w - S(20), S(65) }, line.get());

        // Navigation.
        for (int i = 0; i < static_cast<int>(std::size(kTabs)); ++i)
        {
            D2D1_RECT_F r{ S(12), S(78) + S(44) * i, S(kNavW), S(78) + S(44) * i + S(38) };
            bool on = hit(r, HitTab, i);
            if (i == m.tab)
                m.navRect = r;                      // the sliding marker sits here
            else if (on)
                round(r, S(7), hover.get());
            text(kTabGlyphs[i], { r.left + S(8), r.top, r.left + S(36), r.bottom }, m.icon.get(), i == m.tab ? gold.get() : dim.get());
            text(kTabs[i], { r.left + S(44), r.top, r.right - S(8), r.bottom }, m.body.get(), i == m.tab ? ink.get() : dim.get());
        }
        dc->FillRectangle({ S(kNavW + 8), S(78), S(kNavW + 9), h - S(20) }, line.get());

        // Controls.
        float y = S(78);
        auto row = [&](wchar_t const* label, wchar_t const* desc, float controlW) {
            float right = x1 - controlW - S(16);
            text(label, { x0, y + S(8), right, y + S(32) }, m.body.get(), ink.get());
            if (desc)
                text(desc, { x0, y + S(32), right, y + S(52) }, m.caption.get(), dim.get());
            float cy = y + S(kRowH) / 2.f - S(2);
            y += S(kRowH);
            return cy;
        };
        // Switches are composition visuals (see SyncSettingsVisuals); only their place is recorded here.
        auto toggleRect = [&](float cy) { return D2D1_RECT_F{ x1 - S(46), cy - S(12), x1, cy + S(12) }; };
        auto toggle = [&](float cy, bool on, int what) {
            D2D1_RECT_F r = toggleRect(cy);
            bool over = hit({ r.left - S(10), cy - S(18), x1 + S(4), cy + S(18) }, HitToggle, what);
            m.toggleDescs.push_back({ what, r, on, over, false });
        };
        auto choice = [&](float cy, std::vector<std::wstring> const& options, int current, int what, float segW) {
            float total = segW * options.size(), left = x1 - total;
            round({ left, cy - S(16), x1, cy + S(16) }, S(8), control.get());
            for (size_t i = 0; i < options.size(); ++i)
            {
                D2D1_RECT_F r{ left + segW * i + S(3), cy - S(13), left + segW * (i + 1) - S(3), cy + S(13) };
                bool over = hit(r, HitChoice, what * 16 + static_cast<int>(i));
                if (static_cast<int>(i) == current)
                    m.choiceDescs.push_back({ what, r });   // the sliding gold pill sits here
                else if (over)
                    round(r, S(6), hover.get());
                text(options[i], r, m.value.get(), static_cast<int>(i) == current ? gold.get() : dim.get());
            }
        };
        auto stepper = [&](float cy, std::wstring const& value, int what, bool canDown, bool canUp) {
            D2D1_RECT_F minus{ x1 - S(132), cy - S(16), x1 - S(100), cy + S(16) };
            D2D1_RECT_F plus{ x1 - S(32), cy - S(16), x1, cy + S(16) };
            bool overMinus = canDown && hit(minus, HitStep, what * 16 + 0);
            bool overPlus = canUp && hit(plus, HitStep, what * 16 + 1);
            round(minus, S(7), overMinus ? selected.get() : control.get());
            round(plus, S(7), overPlus ? selected.get() : control.get());
            text(L"\xE738", minus, m.icon.get(), canDown ? ink.get() : faint.get());
            text(L"\xE710", plus, m.icon.get(), canUp ? ink.get() : faint.get());
            text(value, { minus.right, cy - S(16), plus.left, cy + S(16) }, m.value.get(), ink.get());
        };
        auto button = [&](D2D1_RECT_F r, std::wstring const& label, int id, int arg) {
            bool over = hit(r, id, arg);
            round(r, S(7), over ? selected.get() : control.get());
            text(label, r, m.value.get(), ink.get());
        };
        auto note = [&](std::wstring const& s) {
            text(s, { x0, y, x1, y + S(24) }, m.caption.get(), dim.get());
            y += S(32);
        };

        wchar_t buf[64];
        switch (m.tab)
        {
        case 0:     // General
        {
            toggle(row(L"컴퓨터 켤 때 자동 실행", L"Windows에 로그인하면 알아서 켜집니다", S(44)), StartsWithWindows(), OptAutostart);
            toggle(row(L"단축키로 카드 열기", L"Alt + 숫자키(1~6)를 누르면 사이드바의 그 순서 카드가 바로 열립니다", S(44)), m_cfg.hotkeys, OptHotkeys);
            choice(row(L"단축키에 같이 누를 키", L"다른 프로그램의 단축키와 겹치면 바꿔 주세요", S(252)), { L"Alt", L"Ctrl+Alt", L"Shift+Alt" }, m_cfg.hotkeyMod, OptHotkeyMod, S(84));
            swprintf_s(buf, L"%d장", m_cfg.cards);
            stepper(row(L"카드 개수", L"사이드바에 보여 줄 최근 창의 수 (1~6개)", S(132)), buf, OptCards, m_cfg.cards > 1, m_cfg.cards < 6);
            choice(row(L"사이드바 위치", L"카드 목록을 화면의 어느 쪽에 둘지 고릅니다", S(152)), { L"왼쪽", L"오른쪽" }, m_cfg.right ? 1 : 0, OptSide, S(76));
            std::vector<std::wstring> names;
            int current = 0;
            for (size_t i = 0; i < m.monitors.size() && i < 4; ++i)
            {
                MONITORINFOEXW mi{};
                mi.cbSize = sizeof(mi);
                HMONITOR mon = MonitorByDevice(m.monitors[i]);
                bool primary = mon && GetMonitorInfoW(mon, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY);
                swprintf_s(buf, primary ? L"%zu (주)" : L"%zu", i + 1);
                names.push_back(buf);
                if (mon == m_mon)
                    current = static_cast<int>(i);
            }
            float segW = S(74);
            choice(row(L"표시할 모니터", m.monitors.size() > 1 ? L"사이드바를 띄울 화면을 고릅니다" : L"모니터가 하나만 연결되어 있습니다", segW * names.size()),
                names, current, OptMonitor, segW);
            break;
        }
        case 1:     // Look
        {
            swprintf_s(buf, L"%d°", m_cfg.tilt);
            stepper(row(L"카드 기울기", L"0°로 하면 기울이지 않고 평평하게 보입니다", S(132)), buf, OptTilt, m_cfg.tilt > 0, m_cfg.tilt < 60);
            swprintf_s(buf, L"%d%%", m_cfg.size);
            stepper(row(L"카드 크기", L"크게 하면 사이드바도 함께 넓어집니다", S(132)), buf, OptSize, m_cfg.size > 80, m_cfg.size < 130);
            choice(row(L"애니메이션 속도", L"창이 움직이는 빠르기", S(304)), { L"빠르게", L"보통", L"느리게", L"끄기" }, m_cfg.speed, OptSpeed, S(76));
            choice(row(L"카드 속 화면 화질", L"높을수록 선명하지만 그래픽 메모리를 조금 더 씁니다", S(228)), { L"낮음", L"보통", L"높음" }, m_cfg.quality, OptQuality, S(76));
            toggle(row(L"아이콘만 보기 (절전)", L"창 화면 대신 앱 아이콘만 보여 줍니다. 배터리와 메모리를 가장 적게 씁니다", S(44)), m_cfg.iconsOnly, OptIcons);
            toggle(row(L"마우스 올리면 테두리 빛내기", L"카드에 마우스를 올리면 얇은 빛이 테두리를 따라 흐릅니다", S(44)), m_cfg.hoverTrace, OptTrace);
            choice(row(L"대체 카드 모양", L"창 화면을 보여 줄 수 없을 때(최소화 등) 쓰는 카드", S(152)), { L"회색", L"유리" }, m_cfg.cardStyle, OptCardStyle, S(76));
            break;
        }
        case 2:     // Behavior
            toggle(row(L"창에 가리면 사이드바 비켜 주기", L"창을 최대화하는 등 사이드바를 덮으면 화면 옆으로 숨습니다", S(44)), m_cfg.autoTuck, OptAutoTuck);
            toggle(row(L"화면 끝으로 다시 불러오기", L"숨어 있을 때 마우스를 화면 끝에 대면 다시 나타납니다", S(44)), m_cfg.edgeReveal, OptEdge);
            toggle(row(L"반씩 나눈 창 자리 맞추기", L"화면을 반으로 나눠 붙인 창이 사이드바에 가리지 않게 옆으로 옮겨 줍니다", S(44)), m_cfg.fitSnapped, OptFit);
            toggle(row(L"숨는 앱도 카드 남기기", L"카카오톡처럼 X를 눌러도 꺼지지 않고 숨는 앱도 카드로 남깁니다", S(44)), m_cfg.keepTray, OptTray);
            toggle(row(L"새 알림 표시", L"새 메시지가 와서 작업 표시줄이 깜빡이면 그 카드에 주황 점과 빛나는 테두리가 생깁니다", S(44)), m_cfg.alerts, OptAlerts);
            toggle(row(L"소리 나는 앱 표시", L"소리가 나는 카드에 스피커가 뜹니다. 누르면 그 앱의 소리만 끄고 켭니다", S(44)), m_cfg.sounds, OptSounds);
            break;
        case 3:     // Apps
        case 4:     // Pins
        {
            bool apps = m.tab == 3;
            note(apps ? L"표시: 사이드바에 카드로 넣기 · 완전 종료: X를 누르면 숨지 않고 완전히 끄기"
                      : L"고정은 데스크톱마다 따로 저장됩니다. 카드를 오른쪽 클릭해 고정할 수 있어요");
            if (apps)
            {
                // What each column of switches does, over them.
                // Short and centered over each switch (they sit 70px apart), inside the panel.
                m.caption->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                float quitX = x1 - S(70) - S(23), showX = x1 - S(23);
                text(L"완전 종료", { quitX - S(35), y - S(6), quitX + S(35), y + S(14) }, m.caption.get(), dim.get());
                text(L"표시", { showX - S(35), y - S(6), showX + S(35), y + S(14) }, m.caption.get(), dim.get());
                m.caption->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                y += S(18);
            }
            float top = y, bottom = h - (apps ? S(20) : S(76));
            size_t count = apps ? m.apps.size() : m_pinnedApps.size();
            m.scrollMax = std::max(0.f, count * S(kListRowH) - (bottom - top));
            m.scroll = std::clamp(m.scroll, 0.f, m.scrollMax);
            if (count == 0)
                text(apps ? L"열려 있는 앱이 없습니다." : L"고정한 탭이 없습니다.", { x0, top + S(8), x1, top + S(40) }, m.body.get(), dim.get());
            m.listRect = { x0 - S(8), top, x1 + S(8), bottom };
            dc->PushAxisAlignedClip(m.listRect, D2D1_ANTIALIAS_MODE_ALIASED);
            for (size_t i = 0; i < count; ++i)
            {
                float ry = top + S(kListRowH) * i - m.scroll;
                if (ry + S(kListRowH) < top || ry > bottom)
                    continue;
                float cy = ry + S(kListRowH) / 2.f;
                bool visible = ry >= top - 1.f && ry + S(kListRowH) <= bottom + 1.f;
                if (i > 0)
                    dc->FillRectangle({ x0, ry, x1, ry + 1.f }, line.get());
                if (apps)
                {
                    auto& a = m.apps[i];
                    D2D1_RECT_F iconRect{ x0, cy - S(14), x0 + S(28), cy + S(14) };
                    if (a.hwnd && IsWindow(a.hwnd))
                        m_snap.DrawAppIcon(dc, a.hwnd, iconRect);
                    else
                        text(L"\xE71D", iconRect, m.icon.get(), dim.get());
                    text(a.label, { x0 + S(42), cy - S(12), x1 - S(140), cy + S(12) }, m.body.get(), ink.get());
                    bool shown = std::none_of(m_cfg.excluded.begin(), m_cfg.excluded.end(), [&](auto& e) { return wt::SameApp(e, a.id); });
                    bool quits = std::any_of(m_cfg.quitApps.begin(), m_cfg.quitApps.end(), [&](auto& e) { return wt::SameApp(e, a.id); });
                    // Only fully visible rows take clicks. Left switch: quit on X; right one: managed.
                    D2D1_RECT_F quitRect = toggleRect(cy);
                    quitRect.left -= S(70), quitRect.right -= S(70);
                    bool over = visible && m.hover == HitApp && m.hoverArg == static_cast<int>(i);
                    bool overQuit = visible && m.hover == HitQuitApp && m.hoverArg == static_cast<int>(i);
                    if (visible)
                    {
                        m.hits.push_back({ { x1 - S(56), cy - S(18), x1 + S(4), cy + S(18) }, HitApp, static_cast<int>(i) });
                        m.hits.push_back({ { quitRect.left - S(10), cy - S(18), quitRect.right + S(10), cy + S(18) }, HitQuitApp, static_cast<int>(i) });
                    }
                    m.toggleDescs.push_back({ 1000 + static_cast<int>(i), toggleRect(cy), shown, over, true });
                    m.toggleDescs.push_back({ 2000 + static_cast<int>(i), quitRect, quits, overQuit, true });
                }
                else
                {
                    auto const& entry = m_pinnedApps[i];
                    auto bar = entry.find(L'|');
                    std::wstring app = bar == std::wstring::npos ? entry : entry.substr(bar + 1);
                    HWND sample = nullptr;
                    for (auto& c : m_cards)
                        if (wt::SameApp(c->app, app))
                            sample = c->hwnd;
                    D2D1_RECT_F iconRect{ x0, cy - S(14), x0 + S(28), cy + S(14) };
                    if (sample && IsWindow(sample))
                        m_snap.DrawAppIcon(dc, sample, iconRect);
                    else
                        text(L"\xE718", iconRect, m.icon.get(), dim.get());
                    text(wt::AppLabel(app, sample), { x0 + S(42), cy - S(20), x1 - S(90), cy + S(2) }, m.body.get(), ink.get());
                    std::wstring where = L"모든 데스크톱";
                    if (bar != std::wstring::npos)
                    {
                        GUID id{};
                        int n = SUCCEEDED(CLSIDFromString(entry.substr(0, bar).c_str(), &id)) ? DesktopNumber(id) : 0;
                        where = n ? L"데스크톱 " + std::to_wstring(n) : L"닫힌 데스크톱";
                    }
                    text(where, { x0 + S(42), cy + S(2), x1 - S(90), cy + S(20) }, m.caption.get(), dim.get());
                    D2D1_RECT_F r{ x1 - S(72), cy - S(15), x1, cy + S(15) };
                    size_t before = m.hits.size();
                    button(r, L"해제", HitUnpin, static_cast<int>(i));
                    if (!visible)
                        m.hits.resize(before);
                }
            }
            dc->PopAxisAlignedClip();
            if (m.scrollMax > 0.f)
            {
                // A thin scroll indicator.
                float track = bottom - top, thumb = std::max(S(24), track * track / (track + m.scrollMax));
                float ty = top + (track - thumb) * (m.scroll / m.scrollMax);
                round({ x1 + S(12), ty, x1 + S(15), ty + thumb }, S(1.5f), faint.get());
            }
            if (!apps && !m_pinnedApps.empty())
                button({ x1 - S(120), h - S(60), x1, h - S(26) }, L"모두 해제", HitUnpinAll, 0);
            break;
        }
        case 5:     // About
        {
            text(L"Stage Manager for Windows", { x0, y + S(4), x1, y + S(36) }, m.title.get(), ink.get());
            text(L"버전 " APP_VERSION_STR, { x0, y + S(36), x1, y + S(58) }, m.caption.get(), dim.get());
            y += S(76);
            auto stat = [&](wchar_t const* label, std::wstring const& value, wchar_t const* desc) {
                float cy = row(label, desc, S(180));
                text(value, { x1 - S(180), cy - S(12), x1, cy + S(12) }, m.body.get(), ink.get());
            };
            stat(L"메모리 (RAM)", Megabytes(m.ram), L"작업 관리자의 \"메모리\" 열과 같은 값");
            stat(L"GPU 메모리", Megabytes(m.gpu), L"카드 이미지와 화면 합성에 쓰는 그래픽 메모리");
            stat(L"CPU (대기 중)", L"0%", L"창 이벤트가 있을 때만 잠깐 깨어납니다");
            y += S(10);
            button({ x0, y, x0 + S(140), y + S(34) }, L"GitHub 열기", HitGitHub, 0);
            button({ x0 + S(152), y, x0 + S(272), y + S(34) }, L"새로 고침", HitRefresh, 0);
            break;
        }
        }
    });
    SyncSettingsVisuals();
}

// Champagne gold, lit from the top left.
wuc::CompositionLinearGradientBrush Stage::GoldBrush()
{
    auto brush = m_compositor.CreateLinearGradientBrush();
    brush.StartPoint({ 0.f, 0.f });
    brush.EndPoint({ 1.f, 1.f });
    brush.ColorStops().Append(m_compositor.CreateColorGradientStop(0.f, { 255, 255, 226, 140 }));
    brush.ColorStops().Append(m_compositor.CreateColorGradientStop(0.5f, { 255, 244, 196, 86 }));
    brush.ColorStops().Append(m_compositor.CreateColorGradientStop(1.f, { 255, 222, 156, 52 }));
    return brush;
}

void Stage::SyncSettingsVisuals()
{
    auto& m = m_settings;
    if (!m.open || !m.under)
        return;
    using ms = std::chrono::milliseconds;
    // A little overshoot: things land with a soft bounce.
    auto spring = m_compositor.CreateCubicBezierEasingFunction({ 0.34f, 1.45f }, { 0.64f, 1.f });
    auto glide = m_compositor.CreateCubicBezierEasingFunction({ 0.3f, 1.22f }, { 0.5f, 1.f });
    auto rounded = [&](float2 size, float radius) {
        auto g = m_compositor.CreateRoundedRectangleGeometry();
        g.Size(size);
        g.CornerRadius({ radius, radius });
        return m_compositor.CreateGeometricClip(g);
    };
    auto move = [&](auto const& visual, float3 to, int length, wuc::CompositionEasingFunction const& ease) {
        auto a = m_compositor.CreateVector3KeyFrameAnimation();
        a.InsertKeyFrame(1.f, to, ease);
        a.Duration(ms(m_cfg.Ms(length)));
        visual.StartAnimation(L"Offset", a);
    };
    auto scalar = [&](auto const& target, wchar_t const* prop, float to, int length) {
        auto a = m_compositor.CreateScalarKeyFrameAnimation();
        a.InsertKeyFrame(1.f, to, m_ease);
        a.Duration(ms(m_cfg.Ms(length)));
        target.StartAnimation(prop, a);
    };
    auto height = [](D2D1_RECT_F const& r) { return r.bottom - r.top; };

    // Selected tab: a soft pill and a gold bar that glide to the new tab.
    float3 navAt{ m.navRect.left, m.navRect.top, 0.f };
    float3 barAt{ m.navRect.left, m.navRect.top + S(10), 0.f };
    if (!m.navPill)
    {
        float2 size{ m.navRect.right - m.navRect.left, height(m.navRect) };
        m.navPill = m_compositor.CreateSpriteVisual();
        m.navPill.Size(size);
        m.navPill.Brush(m_compositor.CreateColorBrush({ 30, 255, 255, 255 }));
        m.navPill.Clip(rounded(size, S(8)));
        m.navPill.Offset(navAt);
        m.under.Children().InsertAtBottom(m.navPill);
        m.navBar = m_compositor.CreateSpriteVisual();
        m.navBar.Size({ S(3), size.y - S(20) });
        m.navBar.Brush(GoldBrush());
        m.navBar.Clip(rounded({ S(3), size.y - S(20) }, S(1.5f)));
        m.navBar.CenterPoint({ S(1.5f), (size.y - S(20)) / 2.f, 0.f });
        m.navBar.Offset(barAt);
        m.under.Children().InsertAtTop(m.navBar);
    }
    else if (m.navPill.Offset().y != navAt.y)
    {
        move(m.navPill, navAt, 320, glide);
        move(m.navBar, barAt, 380, spring);
        // The bar stretches while it travels.
        auto stretch = m_compositor.CreateVector3KeyFrameAnimation();
        stretch.InsertKeyFrame(0.45f, { 1.f, 1.9f, 1.f });
        stretch.InsertKeyFrame(1.f, { 1.f, 1.f, 1.f }, m_ease);
        stretch.Duration(ms(m_cfg.Ms(380)));
        m.navBar.StartAnimation(L"Scale", stretch);
    }

    // A new page brings its own switches and choices: built fresh, without animation.
    if (m.builtTab != m.tab)
    {
        m.builtTab = m.tab;
        for (auto& t : m.toggles)
        {
            auto from = t.list ? m.listUnder : m.under;
            from.Children().Remove(t.track);
            from.Children().Remove(t.gold);
            (t.list ? m.listOver : m.over).Children().Remove(t.knob);
        }
        for (auto& c : m.choices)
            m.under.Children().Remove(c.pill);
        m.toggles.clear();
        m.choices.clear();
        float w = m.size.x, h = m.size.y;
        auto clip = m_compositor.CreateInsetClip(m.listRect.left, m.listRect.top,
            std::max(0.f, w - m.listRect.right), std::max(0.f, h - m.listRect.bottom));
        m.listUnder.Clip(clip);
        m.listOver.Clip(clip);
    }

    // Switches: a gold track fades in, the knob springs across (stretching on the way) and darkens.
    winrt::Windows::UI::Color knobOn{ 255, 38, 30, 16 }, knobOff{ 255, 236, 236, 240 };
    auto knobAt = [&](D2D1_RECT_F const& r, bool on) {
        float k = height(r) - S(6);
        return float3{ on ? r.right - S(3) - k : r.left + S(3), r.top + S(3), 0.f };
    };
    std::vector<int> keep;
    for (auto const& d : m.toggleDescs)
    {
        keep.push_back(d.key);
        float2 size{ d.rect.right - d.rect.left, height(d.rect) };
        float k = size.y - S(6);
        auto it = std::find_if(m.toggles.begin(), m.toggles.end(), [&](auto& t) { return t.key == d.key; });
        if (it == m.toggles.end())
        {
            ToggleVis t;
            t.key = d.key;
            t.on = d.on;
            t.hover = d.hover;
            t.list = d.list;
            t.track = m_compositor.CreateSpriteVisual();
            t.track.Size(size);
            t.track.Brush(m_compositor.CreateColorBrush({ 255, 62, 62, 68 }));
            t.track.Clip(rounded(size, size.y / 2.f));
            t.gold = m_compositor.CreateSpriteVisual();
            t.gold.Size(size);
            t.gold.Brush(GoldBrush());
            t.gold.Clip(rounded(size, size.y / 2.f));
            t.gold.CenterPoint({ size.x / 2.f, size.y / 2.f, 0.f });
            t.gold.Opacity(d.on ? 1.f : 0.f);
            t.knob = m_compositor.CreateSpriteVisual();
            t.knob.Size({ k, k });
            t.knobBrush = m_compositor.CreateColorBrush(d.on ? knobOn : knobOff);
            t.knob.Brush(t.knobBrush);
            auto circle = m_compositor.CreateEllipseGeometry();
            circle.Center({ k / 2.f, k / 2.f });
            circle.Radius({ k / 2.f, k / 2.f });
            t.knob.Clip(m_compositor.CreateGeometricClip(circle));
            t.knob.CenterPoint({ k / 2.f, k / 2.f, 0.f });
            t.knob.Scale(d.hover ? float3{ 1.12f, 1.12f, 1.f } : float3{ 1.f, 1.f, 1.f });
            auto under = d.list ? m.listUnder : m.under;
            under.Children().InsertAtTop(t.track);
            under.Children().InsertAtTop(t.gold);
            (d.list ? m.listOver : m.over).Children().InsertAtTop(t.knob);
            it = m.toggles.insert(m.toggles.end(), t);
        }
        auto& t = *it;
        // Follows its row (list scrolling) without animating.
        t.track.Offset({ d.rect.left, d.rect.top, 0.f });
        t.gold.Offset({ d.rect.left, d.rect.top, 0.f });
        if (t.on != d.on)
        {
            t.on = d.on;
            move(t.knob, knobAt(d.rect, d.on), 360, spring);
            auto squash = m_compositor.CreateVector3KeyFrameAnimation();
            squash.InsertKeyFrame(0.35f, { 1.32f, 0.84f, 1.f });
            squash.InsertKeyFrame(1.f, d.hover ? float3{ 1.12f, 1.12f, 1.f } : float3{ 1.f, 1.f, 1.f }, m_ease);
            squash.Duration(ms(m_cfg.Ms(360)));
            t.knob.StartAnimation(L"Scale", squash);
            auto color = m_compositor.CreateColorKeyFrameAnimation();
            color.InsertKeyFrame(1.f, d.on ? knobOn : knobOff, m_ease);
            color.Duration(ms(m_cfg.Ms(240)));
            t.knobBrush.StartAnimation(L"Color", color);
            scalar(t.gold, L"Opacity", d.on ? 1.f : 0.f, 240);
            auto swell = m_compositor.CreateVector3KeyFrameAnimation();
            swell.InsertKeyFrame(0.f, d.on ? float3{ 0.86f, 0.86f, 1.f } : float3{ 1.f, 1.f, 1.f });
            swell.InsertKeyFrame(1.f, { 1.f, 1.f, 1.f }, spring);
            swell.Duration(ms(m_cfg.Ms(320)));
            t.gold.StartAnimation(L"Scale", swell);
        }
        else
            t.knob.Offset(knobAt(d.rect, d.on));
        if (t.hover != d.hover)
        {
            t.hover = d.hover;
            auto grow = m_compositor.CreateVector3KeyFrameAnimation();
            grow.InsertKeyFrame(1.f, d.hover ? float3{ 1.12f, 1.12f, 1.f } : float3{ 1.f, 1.f, 1.f }, m_ease);
            grow.Duration(ms(m_cfg.Ms(160)));
            t.knob.StartAnimation(L"Scale", grow);
        }
    }
    // Rows that left the list.
    std::erase_if(m.toggles, [&](ToggleVis& t) {
        if (std::find(keep.begin(), keep.end(), t.key) != keep.end())
            return false;
        auto under = t.list ? m.listUnder : m.under;
        under.Children().Remove(t.track);
        under.Children().Remove(t.gold);
        (t.list ? m.listOver : m.over).Children().Remove(t.knob);
        return true;
    });

    // Choices: a gold-tinted pill glides to the picked option.
    for (auto const& d : m.choiceDescs)
    {
        float2 size{ d.rect.right - d.rect.left, height(d.rect) };
        auto it = std::find_if(m.choices.begin(), m.choices.end(), [&](auto& c) { return c.what == d.what; });
        if (it == m.choices.end())
        {
            ChoiceVis c;
            c.what = d.what;
            c.rect = d.rect;
            c.pill = m_compositor.CreateSpriteVisual();
            c.pill.Size(size);
            c.pill.Brush(GoldBrush());
            c.pill.Opacity(0.24f);
            c.pill.Clip(rounded(size, S(6)));
            c.pill.Offset({ d.rect.left, d.rect.top, 0.f });
            m.under.Children().InsertAtTop(c.pill);
            m.choices.push_back(c);
            continue;
        }
        if (it->rect.left != d.rect.left || it->rect.top != d.rect.top)
        {
            it->rect = d.rect;
            move(it->pill, { d.rect.left, d.rect.top, 0.f }, 340, glide);
        }
    }
}

void Stage::SettingsMove(POINT pt)
{
    auto& m = m_settings;
    float x = pt.x - m.origin.x, y = pt.y - m.origin.y;
    int id = -1, arg = 0;
    for (auto it = m.hits.rbegin(); it != m.hits.rend(); ++it)
    {
        if (x >= it->rect.left && x <= it->rect.right && y >= it->rect.top && y <= it->rect.bottom)
        {
            id = it->id;
            arg = it->arg;
            break;
        }
    }
    if (id == m.hover && arg == m.hoverArg)
        return;
    m.hover = id;
    m.hoverArg = arg;
    PaintSettings();
}

void Stage::SettingsWheel(int delta)
{
    auto& m = m_settings;
    if (m.tab != 3 && m.tab != 4)
        return;
    float to = std::clamp(m.scroll - delta / static_cast<float>(WHEEL_DELTA) * S(kListRowH) * 1.5f, 0.f, m.scrollMax);
    if (to == m.scroll)
        return;
    m.scroll = to;
    m.hover = -1;
    PaintSettings();
}

void Stage::SettingsClick(POINT pt)
{
    auto& m = m_settings;
    if (!InSettingsPanel(pt))
    {
        CloseSettings();
        return;
    }
    float x = pt.x - m.origin.x, y = pt.y - m.origin.y;
    auto found = std::find_if(m.hits.rbegin(), m.hits.rend(), [&](Hit const& h) {
        return x >= h.rect.left && x <= h.rect.right && y >= h.rect.top && y <= h.rect.bottom;
    });
    if (found == m.hits.rend())
        return;
    Hit target = *found;
    switch (target.id)
    {
    case HitClose:
        CloseSettings();
        return;
    case HitTab:
        m.tab = target.arg;
        m.scroll = 0.f;
        if (m.tab == 5 || m.tab == 3)
            LoadSettingsLists();                    // fresh figures and app list
        break;
    case HitToggle:
        switch (target.arg)
        {
        case OptAutostart: SetStartsWithWindows(!StartsWithWindows()); break;
        case OptHotkeys: m_cfg.hotkeys = !m_cfg.hotkeys; break;
        case OptIcons: m_cfg.iconsOnly = !m_cfg.iconsOnly; break;
        case OptAutoTuck: m_cfg.autoTuck = !m_cfg.autoTuck; break;
        case OptEdge: m_cfg.edgeReveal = !m_cfg.edgeReveal; break;
        case OptFit: m_cfg.fitSnapped = !m_cfg.fitSnapped; break;
        case OptTray: m_cfg.keepTray = !m_cfg.keepTray; break;
        case OptTrace: m_cfg.hoverTrace = !m_cfg.hoverTrace; break;
        case OptAlerts: m_cfg.alerts = !m_cfg.alerts; break;
        case OptSounds: m_cfg.sounds = !m_cfg.sounds; break;
        }
        ApplySetting(target.arg);
        break;
    case HitChoice:
    {
        int what = target.arg / 16, value = target.arg % 16;
        switch (what)
        {
        case OptHotkeyMod: m_cfg.hotkeyMod = value; break;
        case OptSide: m_cfg.right = value == 1; break;
        case OptSpeed: m_cfg.speed = value; break;
        case OptQuality: m_cfg.quality = value; break;
        case OptCardStyle: m_cfg.cardStyle = value; break;
        case OptMonitor:
            if (value >= static_cast<int>(m.monitors.size()))
                return;
            m_cfg.monitor = m.monitors[value];
            break;
        }
        ApplySetting(what);
        if (what == OptMonitor)
            return;                                 // the panel was rebuilt on the other monitor
        break;
    }
    case HitStep:
    {
        int what = target.arg / 16, dir = target.arg % 16 ? 1 : -1;
        if (what == OptCards)
            m_cfg.cards = std::clamp(m_cfg.cards + dir, 1, 6);
        else if (what == OptTilt)
            m_cfg.tilt = std::clamp(m_cfg.tilt + dir * 6, 0, 60);
        else if (what == OptSize)
            m_cfg.size = std::clamp(m_cfg.size + dir * 10, 80, 130);
        ApplySetting(what);
        break;
    }
    case HitQuitApp:
    {
        if (target.arg >= static_cast<int>(m.apps.size()))
            return;
        auto id = m.apps[target.arg].id;
        if (std::none_of(m_cfg.quitApps.begin(), m_cfg.quitApps.end(), [&](auto& e) { return wt::SameApp(e, id); }))
            m_cfg.quitApps.push_back(id);
        else
            std::erase_if(m_cfg.quitApps, [&](auto& e) { return wt::SameApp(e, id); });
        SaveSettings();
        break;
    }
    case HitApp:
    {
        if (target.arg >= static_cast<int>(m.apps.size()))
            return;
        auto id = m.apps[target.arg].id;
        // Every entry for it goes (older versions may have saved one per app version).
        bool excludeNow = std::none_of(m_cfg.excluded.begin(), m_cfg.excluded.end(), [&](auto& e) { return wt::SameApp(e, id); });
        if (excludeNow)
            m_cfg.excluded.push_back(id);
        else
            std::erase_if(m_cfg.excluded, [&](auto& e) { return wt::SameApp(e, id); });
        wt::SetExcluded(m_cfg.excluded);
        if (excludeNow)
        {
            // Its windows are left alone from now on: no cards, not on the stage.
            std::vector<HWND> drop;
            for (auto& c : m_cards)
                if (wt::SameApp(c->app, id))
                    drop.push_back(c->hwnd);
            for (HWND h : drop)
                RemoveCard(h, true);
            std::erase_if(m_offstage, [&](auto& c) { return wt::SameApp(c->app, id); });
            std::vector<HWND> onStage;
            for (HWND h : m_stage)
                if (wt::SameApp(wt::AppId(h), id))
                    onStage.push_back(h);
            for (HWND h : onStage)
                LeaveStage(h);
        }
        else
        {
            // Managed again: minimized windows get cards, visible ones stay where they are.
            for (HWND h : wt::EnumManageable(m_mon))
            {
                if (!wt::SameApp(wt::AppId(h), id) || OnStage(h) ||
                    std::any_of(m_cards.begin(), m_cards.end(), [&](auto& c) { return c->hwnd == h; }))
                    continue;
                if (IsIconic(h))
                    Adopt(h);
                else
                    m_stage.push_back(h);
            }
        }
        Relayout(true);
        SaveSettings();
        break;
    }
    case HitUnpin:
        if (target.arg < static_cast<int>(m_pinnedApps.size()))
            UnpinEntry(m_pinnedApps[target.arg]);
        break;
    case HitUnpinAll:
    {
        auto entries = m_pinnedApps;
        for (auto& e : entries)
            UnpinEntry(e);
        break;
    }
    case HitGitHub:
        ShellExecuteW(nullptr, L"open", L"https://github.com/ilfpns/Mulddungddunge", nullptr, nullptr, SW_SHOWNORMAL);
        CloseSettings();
        return;
    case HitRefresh:
        LoadSettingsLists();
        break;
    }
    if (m_settings.open)
        PaintSettings();
}

// Applies a changed setting right away (cards move behind the panel as a live preview) and saves.
void Stage::ApplySetting(int what)
{
    switch (what)
    {
    case OptHotkeys:
    case OptHotkeyMod:
    case OptCards:
        SetHotkeys(false);                          // registered again with the new keys and count
        SetHotkeys(m_dock != DockState::Hidden);
        if (what == OptCards)
        {
            m_hover = -1;
            Relayout(true);
        }
        break;
    case OptSide:
    case OptSize:
        Dock();
        m_hover = -1;
        Relayout(true);
        break;
    case OptTilt:
        Relayout(true);
        break;
    case OptMonitor:
        SaveSettings();
        CloseSettings();
        Rebuild();
        OpenSettings();
        return;
    case OptQuality:
        m_snap.SetQuality(m_cfg.quality);
        break;
    case OptIcons:
        m_activeSnap = nullptr;
        m_prefetch = nullptr;
        if (m_cfg.iconsOnly)
        {
            // Pictures give way to icon cards (drawn again in Relayout for the cards in view).
            for (auto* list : { &m_cards, &m_offstage })
                for (auto& c : *list)
                {
                    c->snapshot.Surface(nullptr);
                    c->hasSnapshot = false;
                    c->hasPicture = false;
                    if (c->side.holder)
                        ApplySize(c->side, *c);
                }
            Relayout(false);
        }
        else
        {
            // Pictures again: minimized windows in view are photographed through DWM now; the others
            // get theirs the next time they leave the stage.
            for (auto& c : m_visible)
                if (IsIconic(c->hwnd))
                    m_snap.CaptureMinimized(c->hwnd, [this, card = c](auto const& surface) {
                        if (surface)
                            SetSnapshot(*card, card->frame, surface);
                        TrimMemory();
                    });
        }
        break;
    case OptFit:
        if (m_cfg.fitSnapped)
            FitBeside();
        break;
    case OptTrace:
        if (!m_cfg.hoverTrace)
            ClearTrace();
        break;
    case OptCardStyle:
        RestylePlaceholders();
        break;
    case OptAlerts:
    case OptSounds:
        if (m_cfg.sounds)
            m_audio.Start(m_sidebar, WM_APP + 3);
        else
            m_audio.Stop();
        OnAudioChanged();                           // also re-applies the alert dots
        break;
    }
    SaveSettings();
}

void Stage::SaveSettings()
{
    m_cfg.Save();
}

// Starts over on another monitor: the sidebar's cards belong to the windows of the monitor it is on.
void Stage::Rebuild()
{
    for (auto& c : m_cards)
        if (c->side.holder)
            m_sideContent.Children().Remove(c->side.holder);
    m_cards.clear();
    m_visible.clear();
    m_offstage.clear();
    m_stage.clear();
    m_active = nullptr;
    m_hover = -1;
    m_activeSnap = nullptr;
    m_prefetch = nullptr;
    Dock();
    Populate();
    SnapActiveSoon();
    StageChanged();
}

void Stage::UnpinEntry(std::wstring entry)
{
    for (auto& c : m_cards)
    {
        if (c->pinned && (PinMatches(entry, *c) || _wcsicmp(c->pinKey.c_str(), entry.c_str()) == 0))
        {
            c->pinKey = entry;                      // the row clicked is the entry that goes
            SetPinned(*c, false);
            return;
        }
    }
    // No window of it right now: just forget the pin.
    auto it = std::find(m_pinnedApps.begin(), m_pinnedApps.end(), entry);
    if (it != m_pinnedApps.end())
    {
        m_pinnedApps.erase(it);
        SavePins();
    }
}
