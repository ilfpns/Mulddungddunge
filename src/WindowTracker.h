#pragma once
#include "pch.h"

// Decides which top-level windows the stage manages and where they are.
namespace wt
{
    bool IsManageable(HWND hwnd, HMONITOR monitor, bool otherDesktops = false);
    // An ordinary app window, whether or not the user excluded its app (monitor null: any monitor).
    bool IsAppWindow(HWND hwnd, HMONITOR monitor, bool otherDesktops = false);
    void SetExcluded(std::vector<std::wstring> const& apps);    // apps IsManageable turns down
    // A readable name for an app id: the executable's description ("KakaoTalk"), or for a browser web
    // app the title of `sample`, one of its windows (may be null).
    std::wstring AppLabel(std::wstring const& app, HWND sample);
    // Always on top without an app window's frame (a desktop pet, a PiP player): left alone.
    bool IsOverlay(HWND hwnd);
    bool IsFullscreen(HWND hwnd);
    bool IsCloaked(HWND hwnd);                              // hidden by DWM, e.g. on another virtual desktop
    // The top-most ordinary window on the monitor: skips always-on-top windows (pets, PiP players),
    // tool windows and our own, which may hold focus without being what the user is looking at.
    HWND TopWindow(HMONITOR monitor);
    std::vector<HWND> EnumManageable(HMONITOR monitor, bool otherDesktops = false);   // top of z-order first
    RECT FrameRect(HWND hwnd);                              // visible frame, without invisible resize borders
    RECT RestoreRect(HWND hwnd, bool* maximized = nullptr); // window rect a minimized window comes back to
    // Brings a minimized window back as it was minimized (maximized, snapped, normal) with one restore
    // only, the taskbar's: a second one would land on a window the first already brought back, and
    // Windows takes that as "restore down". Does nothing to a window not minimized. Returns whether
    // the command went to the app, which should then be checked after kRestoreAnswerMs (RestoreIgnored).
    bool Restore(HWND hwnd);
    constexpr UINT kRestoreAnswerMs = 150;
    bool RestoreIgnored(HWND hwnd);                         // still minimized, though not hung: restore it directly
    // Restores without asking the app. SW_SHOWMAXIMIZED for a window minimized from maximized: unlike
    // SW_RESTORE it is harmless should the app's own restore still be on its way.
    void RestoreDirectly(HWND hwnd);
    std::wstring ProcessPath(HWND hwnd);                    // full path of the window's executable
    // Which app a window belongs to: its executable, or for a browser web app (YouTube or GitHub
    // installed from Chrome/Edge) "executable#web app id", so it is not mistaken for the browser,
    // or for a UWP app "uwp:<AppUserModelID>" (stable whether or not its content is in the frame).
    std::wstring AppId(HWND hwnd);
    std::wstring WebAppIcon(HWND hwnd);                     // the web app's own icon file (png), or empty
    // A dialog (file picker, message box...) and the window it blocks belong together: the dialog is
    // not a window of its own on the stage. ModalOwner: the blocked window of a dialog, or null.
    // ModalDialog: the dialog currently blocking a (disabled) window, or null.
    HWND ModalOwner(HWND dialog);
    HWND ModalDialog(HWND owner);
    // Quitting an app outright, background included (closed-to-tray apps keep running otherwise).
    // QuitTarget: the process behind a window, or 0 if it must not or can't be quit (the shell, system
    // hosts, us, a browser web app, an elevated app, a UWP frame whose app can't be found).
    DWORD QuitTarget(HWND hwnd);
    std::wstring ImageOf(DWORD pid);                        // a process's executable path
    // The file whose icon stands for the window: its executable, except for a terminal host (Windows
    // Terminal, conhost), where it is the shell inside: Git Bash, PowerShell, Command Prompt.
    std::wstring IconFile(HWND hwnd);
    std::wstring ModelOf(DWORD pid);                        // a packaged app's AppUserModelID, or empty
    // Whether an app (executable path + model id, as Audio reports it) is the one a card's id names.
    bool IsCardApp(std::wstring const& cardApp, std::wstring const& image, std::wstring const& model);
    DWORD AppProcess(HWND hwnd);                            // the window's process; for a UWP frame, the app's
    bool IsShellWindow(HWND hwnd);                          // taskbar, desktop, shell popups
    // App ids compared without version folders (Discord "app-1.0.9", Store "Name_1.2.3.0_x64__pub").
    std::wstring AppKey(std::wstring const& id);
    bool SameApp(std::wstring const& a, std::wstring const& b);
    bool IsInputPanel(HWND hwnd);                           // emoji panel (Win+.) or touch keyboard
    void CloseWindowsOf(DWORD pid);                         // WM_CLOSE to each of its windows (UWP frames too)
    bool HasVisibleWindow(DWORD pid);
    // Ends the process and its helper processes running the same executable (Chrome, Electron).
    void KillProcess(HANDLE proc);
}
