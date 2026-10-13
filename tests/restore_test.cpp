// Tests for bringing a window back from its card (wt::Restore, wt::RestoreIgnored, wt::RestoreDirectly):
// the window must come back exactly as it was minimized: maximized stays maximized, a normal window
// keeps its size and place, a snapped one stays snapped.
//
// Build and run: tests\run-tests.bat [--snap] [--e2e]
//   (default)  test windows of our own, each on its own thread so restores reach them as asynchronously
//              as another app's window. They show up on screen for a moment.
//   --snap     also snaps a test window with Win+Left (sends keys; the window has focus meanwhile).
//   --e2e      only this: swaps two test windows through the running stage-manager with Alt+1 (sends
//              keys; whatever is on stage goes to its card and is brought back at the end).
//   --kakao    only this: KakaoTalk's card through the running stage-manager (started and ended again
//              if it was not running).
//   --force    --snap/--e2e/--kakao even if the PC was used within the last minute (otherwise skipped).
#include "../src/WindowTracker.h"
#include <sddl.h>
#include <cstdio>
#include <string>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <vector>

// ---- test windows -----------------------------------------------------------

struct Behavior
{
    std::atomic<bool> ignoreRestore{ false };       // an app that ignores the taskbar's restore command
    std::atomic<int> stallOnRestoreMs{ 0 };         // busy when the command arrives
    std::atomic<int> stallNowMs{ 0 };               // busy (then hung) from now on
};

constexpr UINT kStallMsg = WM_APP + 1;

static LRESULT CALLBACK TestProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto b = reinterpret_cast<Behavior*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (b && m == kStallMsg)
    {
        Sleep(b->stallNowMs.exchange(0));
        return 0;
    }
    if (b && m == WM_SYSCOMMAND && (w & 0xFFF0) == SC_RESTORE)
    {
        if (int ms = b->stallOnRestoreMs.exchange(0))
            Sleep(ms);
        if (b->ignoreRestore)
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

struct TestWindow
{
    HWND hwnd{};
    Behavior* behavior = new Behavior;              // outlives the window thread; leaked on purpose

    TestWindow(wchar_t const* title, RECT r, bool show = true)
    {
        std::atomic<HWND> made{ nullptr };
        std::thread([&made, title, r, show, b = behavior] {
            WNDCLASSW wc{};
            wc.lpfnWndProc = TestProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"MulddungRestoreTest";
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            RegisterClassW(&wc);
            HWND h = CreateWindowW(wc.lpszClassName, title, WS_OVERLAPPEDWINDOW, r.left, r.top, r.right - r.left,
                r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(b));
            if (show)
                ShowWindow(h, SW_SHOWNORMAL);
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
        hwnd = made;
    }
    ~TestWindow()
    {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        Sleep(100);
    }
    void Show(int cmd, int settleMs = 400) const { ShowWindowAsync(hwnd, cmd); Sleep(settleMs); }
    RECT Rect() const { RECT r{}; GetWindowRect(hwnd, &r); return r; }
};

// What Stage::BringBack does with a minimized window, timer included.
static void BringBack(HWND h)
{
    if (wt::Restore(h))
    {
        Sleep(wt::kRestoreAnswerMs);
        if (wt::RestoreIgnored(h))
            wt::RestoreDirectly(h);
    }
}

// The code before the fix, to show the tests catch what went wrong.
static void OldBringBack(HWND h)
{
    if (IsIconic(h))
    {
        ShowWindowAsync(h, SW_RESTORE);
        PostMessageW(h, WM_SYSCOMMAND, SC_RESTORE, 0);
    }
}

// ---- checks -----------------------------------------------------------------

static int g_run = 0, g_failed = 0;

static std::string Describe(HWND h)
{
    RECT r{};
    GetWindowRect(h, &r);
    char s[128];
    sprintf_s(s, "maximized=%d minimized=%d rect=(%ld,%ld)-(%ld,%ld)", IsZoomed(h) ? 1 : 0, IsIconic(h) ? 1 : 0,
        r.left, r.top, r.right, r.bottom);
    return s;
}

static void Expect(char const* name, bool ok, HWND h)
{
    ++g_run;
    if (!ok)
        ++g_failed;
    printf("  [%s] %-62s %s\n", ok ? "PASS" : "FAIL", name, h ? Describe(h).c_str() : "");
}

static bool Maximized(HWND h) { return IsZoomed(h) && !IsIconic(h); }

static bool Same(RECT const& a, RECT const& b) { return EqualRect(&a, &b) != FALSE; }

constexpr RECT kNormal{ 240, 160, 1040, 760 };

// ---- cases ------------------------------------------------------------------

static void MaximizedStaysMaximized()
{
    puts("maximized window");
    TestWindow w(L"restore test: maximized", kNormal);
    w.Show(SW_MAXIMIZE);
    for (int i = 1; i <= 3; ++i)
    {
        w.Show(SW_MINIMIZE);
        BringBack(w.hwnd);
        Sleep(400);
        char name[64];
        sprintf_s(name, "comes back maximized (swap %d)", i);
        Expect(name, Maximized(w.hwnd), w.hwnd);
    }
}

static void NormalKeepsRect()
{
    puts("normal window");
    TestWindow w(L"restore test: normal", kNormal);
    RECT before = w.Rect();
    for (int i = 1; i <= 2; ++i)
    {
        w.Show(SW_MINIMIZE);
        BringBack(w.hwnd);
        Sleep(400);
        char name[64];
        sprintf_s(name, "same size and place (swap %d)", i);
        Expect(name, Same(w.Rect(), before) && !IsZoomed(w.hwnd) && !IsIconic(w.hwnd), w.hwnd);
    }
}

static void MovedWhileMinimized()
{
    // As "fit to the saved size" and dragging a card onto the stage do: the placement changes while
    // the window is minimized, and it must come back there (not where it was before).
    puts("placement changed while minimized");
    TestWindow w(L"restore test: moved", kNormal);
    w.Show(SW_MAXIMIZE);
    w.Show(SW_MINIMIZE);
    WINDOWPLACEMENT wp{ sizeof(wp) };
    GetWindowPlacement(w.hwnd, &wp);
    wp.flags &= ~WPF_RESTORETOMAXIMIZED;
    wp.showCmd = SW_SHOWMINNOACTIVE;
    wp.rcNormalPosition = { 300, 200, 1100, 700 };
    SetWindowPlacement(w.hwnd, &wp);
    BringBack(w.hwnd);
    Sleep(400);
    RECT r = w.Rect();
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(w.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT want = wp.rcNormalPosition;
    OffsetRect(&want, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
    Expect("comes back at the new placement, not maximized", Same(r, want) && !IsZoomed(w.hwnd), w.hwnd);
}

static void IgnoredCommand()
{
    puts("app ignoring the restore command");
    TestWindow w(L"restore test: ignores SC_RESTORE", kNormal);
    w.behavior->ignoreRestore = true;
    w.Show(SW_MAXIMIZE);
    w.Show(SW_MINIMIZE);
    bool posted = wt::Restore(w.hwnd);
    Sleep(wt::kRestoreAnswerMs);
    bool ignored = wt::RestoreIgnored(w.hwnd);
    Expect("command sent, and seen as ignored", posted && ignored, w.hwnd);
    if (ignored)
        wt::RestoreDirectly(w.hwnd);
    Sleep(400);
    Expect("restored directly, maximized", Maximized(w.hwnd), w.hwnd);

    w.Show(SW_RESTORE);
    SetWindowPos(w.hwnd, nullptr, kNormal.left, kNormal.top, kNormal.right - kNormal.left, kNormal.bottom - kNormal.top,
        SWP_NOZORDER | SWP_NOACTIVATE);
    Sleep(200);
    RECT before = w.Rect();
    w.Show(SW_MINIMIZE);
    BringBack(w.hwnd);
    Sleep(400);
    Expect("normal one restored directly, same size and place", Same(w.Rect(), before) && !IsIconic(w.hwnd), w.hwnd);
}

static void SlowApp()
{
    puts("slow app (busy 300 ms when the command arrives)");
    TestWindow w(L"restore test: slow", kNormal);
    w.Show(SW_MAXIMIZE);
    w.Show(SW_MINIMIZE);
    w.behavior->stallOnRestoreMs = 300;
    BringBack(w.hwnd);                              // the fallback fires while the app is still busy
    Sleep(700);
    Expect("maximized one stays maximized", Maximized(w.hwnd), w.hwnd);

    w.Show(SW_RESTORE);
    SetWindowPos(w.hwnd, nullptr, kNormal.left, kNormal.top, kNormal.right - kNormal.left, kNormal.bottom - kNormal.top,
        SWP_NOZORDER | SWP_NOACTIVATE);
    Sleep(200);
    RECT before = w.Rect();
    w.Show(SW_MINIMIZE);
    w.behavior->stallOnRestoreMs = 300;
    BringBack(w.hwnd);
    Sleep(700);
    Expect("normal one keeps size and place", Same(w.Rect(), before) && !IsIconic(w.hwnd), w.hwnd);
}

static void HungApp()
{
    puts("hung app (not answering for over 5 s before the click)");
    TestWindow w(L"restore test: hung", kNormal);
    w.Show(SW_MAXIMIZE);
    w.Show(SW_MINIMIZE);
    w.behavior->stallNowMs = 6500;
    PostMessageW(w.hwnd, kStallMsg, 0, 0);
    Sleep(5600);                                    // Windows calls it hung after 5 s
    bool posted = wt::Restore(w.hwnd);
    Sleep(wt::kRestoreAnswerMs);
    bool ignored = wt::RestoreIgnored(w.hwnd);
    Expect("not restored a second time while hung", posted && !ignored, nullptr);
    Sleep(1500);                                    // it wakes up and handles the queued command
    Expect("maximized once it answers", Maximized(w.hwnd), w.hwnd);
}

static void NotMinimized()
{
    puts("window already on screen");
    TestWindow w(L"restore test: on screen", kNormal);
    w.Show(SW_MAXIMIZE);
    Expect("Restore does nothing", !wt::Restore(w.hwnd), nullptr);
    Sleep(400);
    Expect("stays maximized", Maximized(w.hwnd), w.hwnd);
}

static void Destroyed()
{
    puts("window closed meanwhile");
    HWND h;
    {
        TestWindow w(L"restore test: closed", kNormal);
        w.Show(SW_MINIMIZE);
        h = w.hwnd;
    }
    Sleep(300);
    Expect("Restore and RestoreIgnored are false", !IsWindow(h) && !wt::Restore(h) && !wt::RestoreIgnored(h), nullptr);
}

// A window of a higher integrity level (UIPI): a low integrity copy of this program restores ours, as
// we would an elevated app's window. UIPI blocks most posted messages there (WM_APP: access denied),
// but lets the restore command through, so it is normally Restore's first path that does it.
static bool RunLowIntegrity(std::wstring args, DWORD* exitCode)
{
    HANDLE token = nullptr, low = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY, &token))
        return false;
    bool ok = DuplicateTokenEx(token, 0, nullptr, SecurityImpersonation, TokenPrimary, &low);
    CloseHandle(token);
    if (!ok)
        return false;
    PSID sid = nullptr;
    ConvertStringSidToSidW(L"S-1-16-4096", &sid);   // low mandatory level
    TOKEN_MANDATORY_LABEL label{ { sid, SE_GROUP_INTEGRITY } };
    ok = SetTokenInformation(low, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(sid));
    LocalFree(sid);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" " + args;
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    ok = ok && CreateProcessAsUserW(low, nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    CloseHandle(low);
    if (!ok)
        return false;
    WaitForSingleObject(pi.hProcess, 10000);
    GetExitCodeProcess(pi.hProcess, exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static void CannotMessage()
{
    puts("window of a higher integrity level (elevated app)");
    TestWindow w(L"restore test: elevated", kNormal);
    w.Show(SW_MAXIMIZE);
    w.Show(SW_MINIMIZE);
    wchar_t args[64];
    swprintf_s(args, L"--restore-child %p", w.hwnd);
    DWORD code = 99;
    if (!RunLowIntegrity(args, &code))
    {
        Expect("low integrity helper started", false, nullptr);
        return;
    }
    char name[96];
    sprintf_s(name, "restore went out (%s)", code == 1 ? "command to the app" : code == 0 ? "blocked: restored directly" : "helper failed");
    Expect(name, code == 0 || code == 1, nullptr);
    Sleep(500);
    Expect("comes back maximized", Maximized(w.hwnd), w.hwnd);
}

static void OldCodeFails()
{
    // Not a check of the fix: shows these tests catch the bug (the old double restore).
    puts("old code (SW_RESTORE + SC_RESTORE), for reference");
    TestWindow w(L"restore test: old code", kNormal);
    w.Show(SW_MAXIMIZE);
    w.Show(SW_MINIMIZE);
    OldBringBack(w.hwnd);
    Sleep(400);
    bool lost = !IsZoomed(w.hwnd);
    printf("  [INFO] old code %s the maximize                              %s\n", lost ? "loses" : "keeps", Describe(w.hwnd).c_str());
}

// ---- interactive cases --------------------------------------------------------

static void Keys(std::initializer_list<std::pair<WORD, bool>> keys)
{
    std::vector<INPUT> in;
    for (auto [vk, up] : keys)
    {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = vk;
        i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        in.push_back(i);
    }
    SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT));
}

// Focus as stage-manager's ForceForeground takes it: a synthetic Alt tap counts as input and lifts the
// foreground lock.
static void Activate(HWND h)
{
    Keys({ { VK_MENU, false }, { VK_MENU, true } });
    SetForegroundWindow(h);
}

static void Snapped()
{
    puts("snapped window (Win+Left)");
    for (int round = 0; round < 2; ++round)
    {
        bool fixed = round == 1;
        TestWindow w(fixed ? L"restore test: snapped" : L"restore test: snapped (old code)", kNormal);
        Activate(w.hwnd);
        Sleep(300);
        if (GetForegroundWindow() != w.hwnd)
        {
            // Win+Left would snap whatever has focus instead.
            Expect("setup: test window has focus", false, w.hwnd);
            return;
        }
        Keys({ { VK_LWIN, false }, { VK_LEFT, false }, { VK_LEFT, true }, { VK_LWIN, true } });
        Sleep(700);
        Keys({ { VK_ESCAPE, false }, { VK_ESCAPE, true } });   // close Snap Assist
        Sleep(400);
        RECT snapped = w.Rect();
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(w.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        LONG halfW = (mi.rcWork.right - mi.rcWork.left) / 2;
        bool isSnapped = std::abs((snapped.right - snapped.left) - halfW) < 40;
        if (!fixed && !isSnapped)
        {
            Expect("window got snapped (setup)", false, w.hwnd);
            return;
        }
        w.Show(SW_MINIMIZE);
        fixed ? BringBack(w.hwnd) : OldBringBack(w.hwnd);
        Sleep(500);
        if (fixed)
            Expect("stays snapped, same size and place", Same(w.Rect(), snapped) && !IsIconic(w.hwnd), w.hwnd);
        else
            printf("  [INFO] old code %s the snap                                  %s\n",
                Same(w.Rect(), snapped) ? "keeps" : "loses", Describe(w.hwnd).c_str());
    }
}

// KakaoTalk minimizes its own way and closes to the tray; its card must still bring it back.
// Clicks its card through the running stage-manager (WM_COMMAND 33). Started if it isn't running, and
// then ended again afterwards.
static void KakaoTalk()
{
    puts("KakaoTalk card (minimizes its own way, closes to the tray)");
    HWND sidebar = FindWindowW(L"StageManagerSidebar", nullptr);
    if (!sidebar)
    {
        Expect("stage-manager is running", false, nullptr);
        return;
    }
    auto findKakao = [] {
        struct Ctx { HWND found; } ctx{ nullptr };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            wchar_t title[32]{};
            GetWindowTextW(h, title, ARRAYSIZE(title));
            if (wcscmp(title, L"카카오톡") != 0 || GetWindow(h, GW_OWNER))
                return TRUE;
            auto path = wt::ProcessPath(h);
            if (path.size() < 13 || _wcsicmp(path.c_str() + path.size() - 13, L"KakaoTalk.exe") != 0)
                return TRUE;
            reinterpret_cast<Ctx*>(lp)->found = h;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&ctx));
        return ctx.found;
    };
    auto waitFor = [](auto done, DWORD ms) {
        for (DWORD t = GetTickCount(); GetTickCount() - t < ms; Sleep(50))
            if (done())
                return true;
        return static_cast<bool>(done());
    };
    auto open = [&](HWND h) {
        SendMessageTimeoutW(sidebar, WM_COMMAND, 33, reinterpret_cast<LPARAM>(h), SMTO_ABORTIFHUNG, 2000, nullptr);
        return waitFor([&] { return IsWindowVisible(h) && !IsIconic(h); }, 3000);
    };
    // KakaoTalk processes (its launcher starts the real one: ending what we started means all of them).
    auto kakaoPids = [] {
        std::vector<DWORD> pids;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe{ sizeof(pe) };
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
            if (_wcsicmp(pe.szExeFile, L"KakaoTalk.exe") == 0)
                pids.push_back(pe.th32ProcessID);
        CloseHandle(snap);
        return pids;
    };
    HWND user = GetForegroundWindow();
    HWND k = findKakao();
    bool wasRunning = !kakaoPids().empty();
    if (!k)
    {
        wchar_t exe[MAX_PATH];
        ExpandEnvironmentStringsW(L"%ProgramFiles%\\Kakao\\KakaoTalk\\KakaoTalk.exe", exe, MAX_PATH);
        ShellExecuteW(nullptr, L"open", exe, nullptr, nullptr, SW_SHOWNORMAL);
        waitFor([&] { return (k = findKakao()) != nullptr && IsWindowVisible(k); }, 20000);
        // Its first "카카오톡" window is a splash that goes in a moment: the one that stays is the one.
        Sleep(5000);
        k = findKakao();
        if (k && !IsWindowVisible(k))
            k = nullptr;
    }
    if (!k)
    {
        Expect("KakaoTalk window found (installed and started)", false, nullptr);
        return;
    }
    Sleep(2500);                                    // settled: on stage, photographed

    ShowWindow(k, SW_MINIMIZE);                     // as its minimize button
    bool minimized = waitFor([&] { return IsIconic(k) || !IsWindowVisible(k); }, 3000);
    Sleep(1500);
    Expect("minimized: into its card", minimized, k);
    Expect("its card brings it back", open(k), k);
    Sleep(1500);

    // Closed as with its X or Alt+F4 (the system close command; a bare WM_CLOSE makes KakaoTalk quit).
    PostMessageW(k, WM_SYSCOMMAND, SC_CLOSE, 0);
    waitFor([&] { return !IsWindow(k) || !IsWindowVisible(k) || IsIconic(k); }, 3000);
    Sleep(1500);
    if (!IsWindow(k))
    {
        // It quit, or its sign-in window closed for good: nothing in the tray to come back from.
        printf("  [SKIP] %-62s\n", "closed: the window was destroyed (it quit), no tray case");
        if (wasRunning && kakaoPids().empty())
        {
            // It was running before the test: started again, as the user had it.
            wchar_t exe[MAX_PATH];
            ExpandEnvironmentStringsW(L"%ProgramFiles%\\Kakao\\KakaoTalk\\KakaoTalk.exe", exe, MAX_PATH);
            ShellExecuteW(nullptr, L"open", exe, nullptr, nullptr, SW_SHOWMINNOACTIVE);
            puts("  (KakaoTalk had quit: started again)");
        }
    }
    else
    {
        Expect("closed with X: hidden to the tray, still a window", !IsWindowVisible(k) || IsIconic(k), k);
        Expect("its card shows it again", open(k), k);
        Sleep(1000);
    }

    if (!wasRunning)
    {
        for (DWORD pid : kakaoPids())               // it was not running before the test
            if (HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, pid))
            {
                TerminateProcess(p, 0);
                CloseHandle(p);
            }
    }
    else if (IsWindow(k))
        ShowWindow(k, SW_MINIMIZE);                 // back to a card, as it was
    Sleep(800);
    if (user && IsWindow(user))
    {
        Activate(user);
        if (IsIconic(user))
            ShowWindow(user, SW_RESTORE);
    }
}

static void EndToEnd()
{
    puts("end to end: swapped through the running stage-manager (Alt+1)");
    if (!FindWindowW(L"StageManagerSidebar", nullptr))
    {
        Expect("stage-manager is running", false, nullptr);
        return;
    }
    HWND user = GetForegroundWindow();
    {
        // Shown one at a time, each taking focus as a click would: a window that can't take it (this
        // program may not have the right to) leaves focus to the taskbar, and the stage with it.
        TestWindow a(L"restore test e2e: A maximized", kNormal, false);
        TestWindow b(L"restore test e2e: B normal", { 500, 200, 1500, 900 }, false);
        a.Show(SW_SHOWMAXIMIZED, 100);
        Activate(a.hwnd);
        Sleep(1500);
        Expect("setup: A on stage, maximized", Maximized(a.hwnd), a.hwnd);
        b.Show(SW_SHOWNORMAL, 100);
        Activate(b.hwnd);
        Sleep(1500);
        RECT bRect = b.Rect();
        Expect("setup: B on stage, A on its card", IsIconic(a.hwnd) && !IsIconic(b.hwnd), b.hwnd);
        bool ready = !g_failed;                     // otherwise the swaps below would only repeat it
        for (int i = 1; ready && i <= 3; ++i)
        {
            char name[64];
            Keys({ { VK_MENU, false }, { '1', false }, { '1', true }, { VK_MENU, true } });
            Sleep(1500);
            sprintf_s(name, "A back maximized (swap %d)", i);
            Expect(name, Maximized(a.hwnd) && IsIconic(b.hwnd), a.hwnd);
            Keys({ { VK_MENU, false }, { '1', false }, { '1', true }, { VK_MENU, true } });
            Sleep(1500);
            sprintf_s(name, "B back, same size and place (swap %d)", i);
            Expect(name, Same(b.Rect(), bRect) && !IsIconic(b.hwnd) && IsIconic(a.hwnd), b.hwnd);
        }
    }
    Sleep(800);
    if (user)
    {
        if (IsIconic(user))
            ShowWindowAsync(user, SW_RESTORE);
        SetForegroundWindow(user);
    }
}

// ---- main ---------------------------------------------------------------------

// Someone typed or moved the mouse within the last minute: the cases that send keys and take the focus
// would get in their way (and their Alt+number would mix with ours). Skipped unless --force.
static bool SomeoneAtThePc()
{
    LASTINPUTINFO last{ sizeof(last) };
    return GetLastInputInfo(&last) && GetTickCount() - last.dwTime < 60000;
}

int wmain(int argc, wchar_t** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    bool snap = false, e2e = false, kakao = false, force = false;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a == L"--restore-child" && i + 1 < argc)
        {
            // Exit code: 0 the command was blocked (restored directly), 1 it went to the app, 2 not minimized.
            HWND h = reinterpret_cast<HWND>(static_cast<UINT_PTR>(wcstoull(argv[i + 1], nullptr, 16)));
            if (!IsIconic(h))
                return 2;
            return wt::Restore(h) ? 1 : 0;
        }
        snap |= a == L"--snap";
        e2e |= a == L"--e2e";
        kakao |= a == L"--kakao";
        force |= a == L"--force";
    }
    if ((snap || e2e || kakao) && !force && SomeoneAtThePc())
    {
        puts("[SKIP] the PC is in use (input within the last minute): --snap/--e2e/--kakao left out (--force runs them anyway)\n");
        snap = false;
        if (e2e || kakao)
            return 0;
    }
    if (e2e || kakao)
    {
        if (e2e)
            EndToEnd();
        if (kakao)
            KakaoTalk();
        printf("\n%d checks, %d failed\n", g_run, g_failed);
        return g_failed ? 1 : 0;
    }
    HWND user = GetForegroundWindow();
    if (FindWindowW(L"StageManagerSidebar", nullptr))
        puts("note: stage-manager is running and manages these test windows too; quit it for clean results\n");

    MaximizedStaysMaximized();
    NormalKeepsRect();
    MovedWhileMinimized();
    IgnoredCommand();
    SlowApp();
    HungApp();
    NotMinimized();
    Destroyed();
    CannotMessage();
    OldCodeFails();
    if (snap)
        Snapped();

    if (user)
        SetForegroundWindow(user);
    printf("\n%d checks, %d failed\n", g_run, g_failed);
    return g_failed ? 1 : 0;
}
