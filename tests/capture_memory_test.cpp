// GPU memory across window pictures (Snapshot::CaptureAsync, the app's own code): taking pictures over
// and over must not keep adding GPU memory. Runs on its own compositor and photographs a tool window of
// its own kept behind everything else, so it moves no focus and touches no other window.
//
// Measured with DXGI (QueryVideoMemoryInfo), not Task Manager's per-process "Dedicated GPU memory" (the
// "GPU Process Memory" counter): that one climbs by a window's size with every capture session and never
// comes down, though the adapter's total (Task Manager's Performance tab) does not move. A known Windows
// counter fault (KB4490156); measured 2026-10-13: +444 MB on the process counter over 74 pictures, the
// adapter total unchanged (1291 -> 1281 MB).
//
//   capture-memory-test.exe [captures] [--icons] [--hold]
//     captures  pictures to take (default 40)
//     --icons   after each picture also make a small surface that is kept, as a card's icon is
//     --hold    print "warm" after the warm-up and "holding" at the end, pausing a few seconds at each,
//               so the process can be measured from outside (Task Manager's GPU memory counter)
#include "../src/Snapshot.h"
#include "../src/WindowTracker.h"
#include <cstdio>
#include <vector>

namespace wuc = winrt::Windows::UI::Composition;

static LRESULT CALLBACK TargetProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HBRUSH b = CreateSolidBrush(RGB(40, 120, 200));
        FillRect(dc, &r, b);
        DeleteObject(b);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND MakeTarget()
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = TargetProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MulddungCaptureTarget";
    RegisterClassW(&wc);
    // A tool window: not on the taskbar, not managed by stage-manager.
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"capture memory test",
        WS_OVERLAPPEDWINDOW, 100, 100, 1280, 900, nullptr, nullptr, wc.hInstance, nullptr);
    SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return h;
}

static void Pump(DWORD ms)
{
    DWORD until = GetTickCount() + ms;
    MSG msg;
    while (static_cast<int>(until - GetTickCount()) > 0)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    }
}

static void Mark(char const* what, bool hold)
{
    if (!hold)
        return;
    printf("%s\n", what);
    fflush(stdout);
    Pump(3000);
}

int wmain(int argc, wchar_t** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int captures = 40;
    bool icons = false, hold = false;
    for (int i = 1; i < argc; ++i)
    {
        if (wcscmp(argv[i], L"--icons") == 0)
            icons = true;
        else if (wcscmp(argv[i], L"--hold") == 0)
            hold = true;
        else
            captures = std::max(7, _wtoi(argv[i]));
    }
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    DispatcherQueueOptions options{ sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA };
    winrt::Windows::System::DispatcherQueueController controller{ nullptr };
    winrt::check_hresult(CreateDispatcherQueueController(options,
        reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(controller))));
    wuc::Compositor compositor;
    Snapshot snap;
    snap.Init(compositor);

    HWND target = MakeTarget();
    Pump(300);

    // Cards keep their latest picture; the sidebar shows a handful. Keep the last 4, as cards would.
    std::vector<wuc::CompositionDrawingSurface> kept, keptIcons;
    std::vector<double> mb;
    int ok = 0;
    constexpr int warm = 5;                         // the first pictures warm up devices and caches
    for (int i = 0; i < captures; ++i)
    {
        bool done = false;
        snap.CaptureAsync(target, wt::FrameRect(target), [&](wuc::CompositionDrawingSurface const& s) {
            if (s)
            {
                ++ok;
                kept.push_back(s);
                if (kept.size() > 4)
                    kept.erase(kept.begin());
            }
            done = true;
        });
        for (int t = 0; t < 200 && !done; ++t)
            Pump(5);
        Pump(50);
        if (icons)
            keptIcons.push_back(snap.Icon(target, 34));
        mb.push_back(snap.GpuMemory() / 1048576.0);
        if (i == warm)
            Mark("warm", hold);
    }
    kept.clear();
    Pump(500);
    double end = snap.GpuMemory() / 1048576.0;

    double growth = mb.back() - mb[warm];
    double perPicture = growth / (captures - 1 - warm);
    printf("pictures %d (succeeded %d)%s\n", captures, ok, icons ? ", each with a kept icon" : "");
    printf("GPU memory: after %d %.1f MB, after %d %.1f MB, pictures released %.1f MB\n", warm + 1, mb[warm], captures, mb.back(), end);
    printf("growth after warm-up: %.1f MB (%.2f MB per picture)\n", growth, perPicture);
    bool pass = ok == captures && perPicture < 1.0;
    printf("[%s] taking pictures does not keep adding GPU memory (< 1 MB per picture)\n", pass ? "PASS" : "FAIL");
    Mark("holding", hold);
    DestroyWindow(target);
    Pump(1000);
    Mark("target closed", hold);
    return pass ? 0 : 1;
}
