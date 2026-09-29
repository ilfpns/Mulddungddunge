#include "pch.h"
#include "Stage.h"

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\StageManagerForWindows");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    Stage stage;
    if (!stage.Init(inst))
        return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    stage.Shutdown();
    CloseHandle(mutex);
    return 0;
}
