#include "pch.h"
#include "Stage.h"

// Last resort if something goes wrong: give the user back their minimize animation (we turn it off
// while running and keep the original under HKCU\Software\StageManager), and note where it happened
// so the address can be looked up in obj\stage-manager.map.
static LONG WINAPI OnCrash(EXCEPTION_POINTERS* info)
{
    DWORD saved = 0, size = sizeof(saved);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\StageManager", L"MinAnimate", RRF_RT_REG_DWORD, nullptr, &saved, &size) == ERROR_SUCCESS)
    {
        ANIMATIONINFO ai{ sizeof(ai), static_cast<int>(saved) };
        SystemParametersInfoW(SPI_SETANIMATION, sizeof(ai), &ai, 0);
        RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\StageManager", L"MinAnimate");
    }

    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"stage-manager.log");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a") == 0 && f)
    {
        auto base = reinterpret_cast<ULONG_PTR>(GetModuleHandleW(nullptr));
        auto at = reinterpret_cast<ULONG_PTR>(info->ExceptionRecord->ExceptionAddress);
        fwprintf(f, L"crash code=0x%08X rva=0x%llx\n", info->ExceptionRecord->ExceptionCode,
            static_cast<unsigned long long>(at - base));
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\StageManagerForWindows");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    SetUnhandledExceptionFilter(OnCrash);
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
