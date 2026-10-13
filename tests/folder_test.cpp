// End-to-end tests of folders against the running stage-manager, through its WM_COMMAND test hooks and
// its trace file (%TEMP%\stage-manager.trace must exist when it starts):
//   grouping, 이름 바꾸기 (name tag), card numbers while Alt is held, 모두 열기 (split screen),
//   folders and hand-made order kept across a restart of the app.
// Its test windows open minimized, without focus, and become cards. 모두 열기 puts them on stage, and
// the restart closes and starts stage-manager again; whatever had the focus gets it back at the end.
// The saved folders and order (HKCU\Software\StageManager: Folders, Order) are put back as they were.
//
//   folder-test.exe [--shots <folder>] [--force]
//     --shots  sidebar screenshots (name tag, numbers, alert) as BMP files there
//     --force  run even if the PC was used within the last minute (otherwise skipped)
//     --tray   only: a window hiding itself on X (as KakaoTalk signed in) keeps its card and comes back
//     --uwp    only: switch start times with Calculator (UWP) leaving, and closing as a switch begins
//              (opens Calculator and closes it again)
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <algorithm>

constexpr UINT ID_EXIT = 1, ID_UNFOLD = 9, ID_RENAME = 10, ID_OPEN_ALL = 11;
constexpr UINT ID_TEST_MARK = 30, ID_TEST_GROUP = 31, ID_TEST_MOVE = 32;
constexpr wchar_t kKey[] = L"Software\\StageManager";

static int g_run = 0, g_failed = 0;

static void Expect(char const* name, bool ok, std::string const& detail = {})
{
    ++g_run;
    g_failed += !ok;
    printf("  [%s] %-58s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
}

// ---- the app ------------------------------------------------------------------

static HWND Sidebar() { return FindWindowW(L"StageManagerSidebar", nullptr); }

static void Command(UINT id, HWND lp)
{
    SendMessageTimeoutW(Sidebar(), WM_COMMAND, id, reinterpret_cast<LPARAM>(lp), SMTO_ABORTIFHUNG, 2000, nullptr);
}

static std::wstring TracePath()
{
    wchar_t p[MAX_PATH];
    GetTempPathW(MAX_PATH, p);
    return std::wstring(p) + L"stage-manager.trace";
}

static long long TraceSize()
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(TracePath().c_str(), GetFileExInfoStandard, &a))
        return -1;
    return (static_cast<long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
}

// The trace written since `from` (bytes), as text.
static std::wstring TraceSince(long long from)
{
    HANDLE f = CreateFileW(TracePath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER at{};
    at.QuadPart = from;
    SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
    std::string bytes;
    char buf[65536];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got)
        bytes.append(buf, got);
    CloseHandle(f);
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), n);
    return text;
}

static bool WaitFor(std::function<bool()> done, DWORD ms)
{
    for (DWORD t = GetTickCount(); GetTickCount() - t < ms; Sleep(50))
        if (done())
            return true;
    return done();
}

static bool TraceHas(long long from, std::wstring const& what, DWORD ms)
{
    return WaitFor([&] { return TraceSince(from).find(what) != std::wstring::npos; }, ms);
}

static std::wstring LastLayout(long long from)
{
    auto t = TraceSince(from);
    auto at = t.rfind(L"layout: ");
    return at == std::wstring::npos ? L"" : t.substr(at, t.find(L'\n', at) - at);
}

static std::string Narrow(std::wstring const& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::vector<std::wstring> ReadStrings(wchar_t const* name, bool* present = nullptr)
{
    std::vector<std::wstring> out;
    DWORD size = 0;
    bool ok = RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS;
    if (present)
        *present = ok;
    if (!ok || !size)
        return out;
    std::wstring buffer(size / sizeof(wchar_t), L'\0');
    RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_MULTI_SZ, nullptr, buffer.data(), &size);
    for (wchar_t const* p = buffer.c_str(); *p; p += wcslen(p) + 1)
        out.emplace_back(p);
    return out;
}

static void WriteStrings(wchar_t const* name, std::vector<std::wstring> const& strings, bool present)
{
    if (!present)
    {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kKey, name);
        return;
    }
    std::wstring buffer;
    for (auto& s : strings)
        buffer.append(s).push_back(L'\0');
    buffer.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_MULTI_SZ, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(wchar_t)));
}

// ---- test windows ---------------------------------------------------------------

constexpr UINT kDestroy = WM_APP + 2;             // a test window goes for good (from another thread)

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE && GetPropW(h, L"hidesOnClose"))
    {
        ShowWindow(h, SW_HIDE);                     // X: to the tray, as KakaoTalk or Discord signed in
        return 0;
    }
    if (m == kDestroy)
    {
        DestroyWindow(h);
        return 0;
    }
    if (m == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HBRUSH b = CreateSolidBrush(static_cast<COLORREF>(GetWindowLongPtrW(h, GWLP_USERDATA)));
        FillRect(dc, &r, b);
        DeleteObject(b);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND MakeWindow(wchar_t const* title, COLORREF color, bool minimized = true)
{
    std::atomic<HWND> made{ nullptr };
    std::thread([&made, title, color, minimized] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MulddungFolderTest";
        RegisterClassW(&wc);
        HWND h = CreateWindowW(wc.lpszClassName, title, WS_OVERLAPPEDWINDOW, 300, 200, 900, 640, nullptr, nullptr,
            wc.hInstance, nullptr);
        SetWindowLongPtrW(h, GWLP_USERDATA, color);
        ShowWindow(h, minimized ? SW_SHOWMINNOACTIVE : SW_SHOWNORMAL);  // minimized: straight into the sidebar
        made = h;
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }).detach();
    while (!made)
        Sleep(5);
    return made;
}

static RECT Frame(HWND h)
{
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
        GetWindowRect(h, &r);
    return r;
}

static bool Near(RECT const& a, RECT const& b, LONG tol)
{
    return std::abs(a.left - b.left) <= tol && std::abs(a.top - b.top) <= tol && std::abs(a.right - b.right) <= tol &&
           std::abs(a.bottom - b.bottom) <= tol;
}

static std::string Rect(RECT const& r)
{
    char s[64];
    sprintf_s(s, "(%ld,%ld)-(%ld,%ld)", r.left, r.top, r.right, r.bottom);
    return s;
}

// The sidebar as it is on screen, to look at.
static void Shot(std::wstring const& dir, wchar_t const* name)
{
    if (dir.empty())
        return;
    HWND view = FindWindowW(L"StageManagerView", nullptr);
    RECT r{};
    if (!view || !GetWindowRect(view, &r))
        return;
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);
    BITMAPINFOHEADER bi{ sizeof(bi), w, -h, 1, 24, BI_RGB };
    int stride = (w * 3 + 3) & ~3;
    std::vector<BYTE> px(static_cast<size_t>(stride) * h);
    GetDIBits(mem, bmp, 0, h, px.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
    BITMAPFILEHEADER bf{ 0x4D42, static_cast<DWORD>(sizeof(bf) + sizeof(bi) + px.size()), 0, 0, sizeof(bf) + sizeof(bi) };
    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir + L"\\" + name).c_str(), L"wb") == 0 && f)
    {
        fwrite(&bf, sizeof(bf), 1, f);
        fwrite(&bi, sizeof(bi), 1, f);
        fwrite(px.data(), 1, px.size(), f);
        fclose(f);
    }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

static void Key(WORD vk, bool up)
{
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof(in));
}

// The window that had the focus comes back as from the taskbar (it may be a card by now).
static void GiveBack(HWND user)
{
    if (!user || !IsWindow(user))
        return;
    Key(VK_MENU, false);                            // a key press lifts the foreground lock
    Key(VK_MENU, true);
    if (IsIconic(user))
        ShowWindow(user, SW_RESTORE);
    SetForegroundWindow(user);
}

static std::wstring AppPath()
{
    HWND s = Sidebar();
    DWORD pid = 0;
    GetWindowThreadProcessId(s, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    wchar_t path[MAX_PATH]{};
    DWORD n = MAX_PATH;
    if (p)
    {
        QueryFullProcessImageNameW(p, 0, path, &n);
        CloseHandle(p);
    }
    return path;
}

constexpr UINT ID_TEST_OPEN = 33;

// A window that hides itself on X (as KakaoTalk or Discord signed in) keeps its card, and its card shows
// it again.
static void TrayCase()
{
    puts("closing to the tray");
    HWND w = MakeWindow(L"tray test", RGB(250, 200, 40), false);
    SetPropW(w, L"hidesOnClose", reinterpret_cast<HANDLE>(1));
    GiveBack(w);                                    // focused: on stage
    Sleep(2000);
    long long t = TraceSize();
    PostMessageW(w, WM_CLOSE, 0, 0);
    bool hidden = WaitFor([&] { return !IsWindowVisible(w); }, 2000);
    Expect("X hides it (still a window)", hidden && IsWindow(w));
    bool carded = TraceHas(t, L"minimize (", 3000) && TraceSince(t).find(L"'tray test'") != std::wstring::npos;
    Sleep(800);
    Expect("hidden to the tray, it goes into a card", carded);
    Command(ID_TEST_OPEN, w);
    Expect("its card shows it again", WaitFor([&] { return IsWindowVisible(w) && !IsIconic(w); }, 3000));
    Sleep(1000);
    PostMessageW(w, kDestroy, 0, 0);
    Sleep(500);
}

// Switch start times around a UWP window (Calculator): one leaving the stage, and one closing just as a
// switch begins (its card is made after the switch, not in its way). All must stay under 170 ms.
static void UwpCase()
{
    puts("UWP window (Calculator)");
    auto findCalc = [] {
        for (HWND h = FindWindowW(L"ApplicationFrameWindow", nullptr); h; h = FindWindowExW(nullptr, h, L"ApplicationFrameWindow", nullptr))
        {
            wchar_t title[64]{};
            GetWindowTextW(h, title, ARRAYSIZE(title));
            if ((wcscmp(title, L"계산기") == 0 || wcscmp(title, L"Calculator") == 0) && IsWindowVisible(h))
                return h;
        }
        return static_cast<HWND>(nullptr);
    };
    auto tookSince = [](long long t) {
        double worst = -1;
        auto s = TraceSince(t);
        for (size_t at = 0; (at = s.find(L"switch start took ", at)) != std::wstring::npos; ++at)
            worst = (std::max)(worst, _wtof(s.c_str() + at + 18));
        return worst;
    };
    HWND m = MakeWindow(L"uwp test card", RGB(120, 120, 220));
    WaitFor([&] { return IsIconic(m); }, 3000);
    Sleep(1500);
    for (int round = 0; round < 2; ++round)
    {
        bool closing = round == 1;
        ShellExecuteW(nullptr, L"open", L"calc.exe", nullptr, nullptr, SW_SHOWNORMAL);
        HWND calc = nullptr;
        WaitFor([&] { return (calc = findCalc()) != nullptr; }, 10000);
        if (!calc)
        {
            Expect("Calculator opened", false);
            break;
        }
        GiveBack(calc);
        Sleep(3000);                                // on stage, photographed
        long long t = TraceSize();
        if (closing)
        {
            // Hidden, and the switch asked for at once: a sent message is handled before the queued
            // notice of the hiding, so the switch finds it gone unseen (with no card yet).
            PostMessageW(calc, WM_CLOSE, 0, 0);
            WaitFor([&] { return !IsWindowVisible(calc); }, 3000);
        }
        Command(ID_TEST_OPEN, m);
        bool up = WaitFor([&] { return IsWindowVisible(m) && !IsIconic(m); }, 3000);
        TraceHas(t, L"switch end", 2000);
        double took = tookSince(t);
        char name[96];
        sprintf_s(name, closing ? "switch as Calculator closes: start %.1f ms (< 170)" : "Calculator leaving the stage: start %.1f ms (< 170)", took);
        Expect(name, up && took >= 0 && took < 170.0);
        if (closing)
            Expect("  (it was the gone-unseen path, card made after the switch)",
                TraceSince(t).find(L"made after the switch") != std::wstring::npos);
        Sleep(1000);
        if (!closing)
        {
            if (HWND c = findCalc())
                PostMessageW(c, WM_CLOSE, 0, 0);
            ShowWindow(m, SW_MINIMIZE);             // back to a card for the next round
            WaitFor([&] { return IsIconic(m); }, 3000);
            Sleep(1500);
        }
    }
    PostMessageW(m, kDestroy, 0, 0);
    Sleep(500);
}

int wmain(int argc, wchar_t** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::wstring shots;
    bool force = false, tray = false, uwp = false;
    for (int i = 1; i < argc; ++i)
    {
        if (wcscmp(argv[i], L"--shots") == 0 && i + 1 < argc)
            shots = argv[i + 1];
        force |= wcscmp(argv[i], L"--force") == 0;
        tray |= wcscmp(argv[i], L"--tray") == 0;
        uwp |= wcscmp(argv[i], L"--uwp") == 0;
    }
    // It sends keys, takes the focus and restarts the app: not while someone is using the PC.
    LASTINPUTINFO last{ sizeof(last) };
    if (!force && GetLastInputInfo(&last) && GetTickCount() - last.dwTime < 60000)
    {
        puts("[SKIP] the PC is in use (input within the last minute); --force runs it anyway");
        return 0;
    }
    if (!Sidebar() || TraceSize() < 0)
    {
        puts("stage-manager must be running, started with %TEMP%\\stage-manager.trace present");
        return 2;
    }
    std::wstring app = AppPath();
    HWND user = GetForegroundWindow();
    if (tray || uwp)
    {
        if (tray)
            TrayCase();
        if (uwp)
            UwpCase();
        GiveBack(user);
        printf("\n%d checks, %d failed\n", g_run, g_failed);
        return g_failed ? 1 : 0;
    }
    bool hadFolders = false, hadOrder = false;
    auto savedFolders = ReadStrings(L"Folders", &hadFolders), savedOrder = ReadStrings(L"Order", &hadOrder);
    long long start = TraceSize();

    puts("cards");
    HWND a = MakeWindow(L"folder test A", RGB(200, 80, 60));
    HWND b = MakeWindow(L"folder test B", RGB(60, 160, 90));
    HWND c = MakeWindow(L"folder test C", RGB(60, 110, 200));
    HWND d = MakeWindow(L"folder test D", RGB(150, 150, 40));
    // The sidebar shows only a few cards; "(of N)" in the layout line counts them all.
    auto cardCount = [](std::wstring const& layout) {
        auto at = layout.rfind(L"(of ");
        return at == std::wstring::npos ? -1 : _wtoi(layout.c_str() + at + 4);
    };
    int before = cardCount(LastLayout(start > 200000 ? start - 200000 : 0));
    bool carded = WaitFor([&] { return before >= 0 && cardCount(LastLayout(start)) >= before + 4; }, 6000);
    Expect("minimized test windows become 4 more cards", carded, Narrow(LastLayout(start)));
    if (!carded)
    {
        for (HWND h : { a, b, c, d })
            PostMessageW(h, WM_CLOSE, 0, 0);
        printf("\n%d checks, %d failed (stopped: no cards to test with)\n", g_run, g_failed);
        return 1;
    }

    puts("grouping");
    long long t = TraceSize();
    Command(ID_TEST_MARK, a);
    Command(ID_TEST_GROUP, b);
    Command(ID_TEST_GROUP, c);
    bool grouped = WaitFor([&] {
        auto s = TraceSince(t);
        size_t n = 0;
        for (size_t at = 0; (at = s.find(L"grouped into a folder", at)) != std::wstring::npos; ++at)
            ++n;
        return n == 2;
    }, 3000);
    Expect("A, B, C grouped into one folder", grouped, Narrow(LastLayout(start)));
    auto folders = ReadStrings(L"Folders");
    size_t tabs = folders.empty() ? 0 : std::count(folders.back().begin(), folders.back().end(), L'\t');
    Expect("the folder is saved with its 3 windows' apps", folders.size() == savedFolders.size() + 1 && tabs == 5);

    puts("이름 바꾸기");
    t = TraceSize();
    Command(ID_RENAME, a);
    HWND edit = nullptr;
    WaitFor([&] {
        DWORD pid = 0;
        GetWindowThreadProcessId(Sidebar(), &pid);
        for (HWND w = FindWindowW(L"Edit", nullptr); w; w = FindWindowExW(nullptr, w, L"Edit", nullptr))
        {
            DWORD p = 0;
            GetWindowThreadProcessId(w, &p);
            if (p == pid && IsWindowVisible(w))
            {
                edit = w;
                return true;
            }
        }
        return false;
    }, 2000);
    Expect("the name box opens over the folder's card", edit != nullptr);
    if (edit)
    {
        SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"  테스트 폴더  "));
        PostMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
    }
    // Traced as "folder renamed: <name> <null window> ''": untrimmed spaces would show before or after.
    Expect("Enter keeps the name (spaces trimmed)", TraceHas(t, L"folder renamed: 테스트 폴더 0", 2000));
    folders = ReadStrings(L"Folders");
    Expect("the name is saved", !folders.empty() && folders.back().find(L"\t테스트 폴더\t") != std::wstring::npos);
    Sleep(400);
    Shot(shots, L"1-name-tag.bmp");

    puts("card numbers (Alt held)");
    t = TraceSize();
    Key(VK_MENU, false);
    bool shown = TraceHas(t, L"card numbers shown", 1500);
    Sleep(150);
    Shot(shots, L"2-numbers.bmp");
    Key(VK_MENU, true);
    Expect("holding Alt shows the numbers", shown);
    t = TraceSize();
    Key(VK_MENU, false);
    Sleep(150);
    Key(VK_MENU, true);
    Sleep(600);
    Expect("a quick Alt tap shows nothing", TraceSince(t).find(L"card numbers shown") == std::wstring::npos);

    puts("모두 열기");
    t = TraceSize();
    Command(ID_OPEN_ALL, a);
    bool opened = TraceHas(t, L"folder opened whole", 2000) && TraceHas(t, L"switch end", 3000);
    Sleep(600);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(Sidebar(), MONITOR_DEFAULTTONEAREST), &mi);
    RECT w = mi.rcWork;
    LONG x = (w.left + w.right) / 2, y = (w.top + w.bottom) / 2;
    RECT ra = Frame(a), rb = Frame(b), rc = Frame(c);
    Expect("switch ran", opened);
    Expect("A on the left half", !IsIconic(a) && Near(ra, { w.left, w.top, x, w.bottom }, 3), Rect(ra));
    Expect("B on the right top quarter", !IsIconic(b) && Near(rb, { x, w.top, w.right, y }, 3), Rect(rb));
    Expect("C on the right bottom quarter", !IsIconic(c) && Near(rc, { x, y, w.right, w.bottom }, 3), Rect(rc));
    Expect("D stays a card", IsIconic(d));

    puts("moving a card by hand");
    // The folder's windows minimized, as with their minimize buttons: back into the folder's card.
    // (Giving the focus back instead could put that window next to them on stage.)
    for (HWND h : { c, b, a })
    {
        ShowWindow(h, SW_MINIMIZE);
        Sleep(700);
    }
    bool back = WaitFor([&] { return IsIconic(a) && IsIconic(b) && IsIconic(c); }, 3000);
    Sleep(800);
    Expect("minimized, they go back to their cards", back);
    t = TraceSize();
    Command(ID_TEST_MARK, a);
    Command(ID_TEST_MOVE, d);                       // D dropped on the folder's card
    bool moved = TraceHas(t, L"card moved", 2000);
    auto order = ReadStrings(L"Order");
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);    // the test windows' app
    size_t mine = std::count_if(order.begin(), order.end(), [&](auto& e) { return _wcsicmp(e.c_str(), self) == 0; });
    Expect("D moved, the order saved with the test windows in it", moved && mine == 4, std::to_string(mine) + " of 4");

    puts("알림 표시");
    t = TraceSize();
    FLASHWINFO flash{ sizeof(flash), d, FLASHW_TRAY, 3, 0 };    // as an app with a new message does
    FlashWindowEx(&flash);
    bool alerted = TraceHas(t, L"flash", 2000) && TraceHas(t, L"trace: alert", 2000);
    Sleep(400);
    Shot(shots, L"3-alert.bmp");
    Expect("a flashing window's card gets the dot and the running outline", alerted);

    puts("restart of stage-manager");
    t = TraceSize();
    DWORD pid = 0;
    GetWindowThreadProcessId(Sidebar(), &pid);
    HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(Sidebar(), WM_COMMAND, ID_EXIT, 0);
    bool exited = proc && WaitForSingleObject(proc, 8000) == WAIT_OBJECT_0;
    if (proc)
        CloseHandle(proc);
    Expect("stage-manager quits", exited);
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    bool started = CreateProcessW(app.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    if (started)
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    bool formed = started && TraceHas(t, L"saved folder formed again: 테스트 폴더", 10000);
    Expect("the folder forms again with its name after the restart", formed);
    Expect("the saved card order is applied", TraceHas(t, L"saved card order applied", 2000));
    Sleep(800);
    Shot(shots, L"4-after-restart.bmp");

    // C closes: the folder keeps A and B, and D (same app, a card of its own) is not pulled in to
    // take C's place.
    t = TraceSize();
    PostMessageW(c, WM_CLOSE, 0, 0);
    Sleep(1500);
    Expect("a closed window's place is not refilled by another", TraceSince(t).find(L"saved folder filled up") == std::wstring::npos);

    puts("cleanup");
    t = TraceSize();
    // Just started, it may still be busy setting up (or mid-animation, when commands wait): again until done.
    // (The windows are all one app's: after the restart any three of them may have formed it.)
    bool unfolded = WaitFor([&] {
        for (HWND h : { a, b, d })
            Command(ID_UNFOLD, h);
        return TraceHas(t, L"folder unfolded", 500);
    }, 10000);
    Expect("폴더 풀기 forgets the saved folder", unfolded && ReadStrings(L"Folders").size() == savedFolders.size());
    for (HWND h : { a, b, c, d })
        PostMessageW(h, WM_CLOSE, 0, 0);
    WriteStrings(L"Folders", savedFolders, hadFolders);
    WriteStrings(L"Order", savedOrder, hadOrder);
    Sleep(800);
    GiveBack(user);
    printf("\n%d checks, %d failed\n", g_run, g_failed);
    return g_failed ? 1 : 0;
}
