#pragma once
#include "pch.h"

// User settings, kept under HKCU\Software\StageManager\Settings. Read once at startup, written on change.
struct Config
{
    int cards = 4;                  // sidebar cards, 1..6
    int quality = 1;                // window pictures: 0 low, 1 normal, 2 high detail
    bool iconsOnly = false;         // no window pictures, app icon cards only: least GPU memory, no captures
    bool hotkeys = true;            // modifier + 1..N switches to that card
    int hotkeyMod = 0;              // 0 Alt, 1 Ctrl+Alt, 2 Shift+Alt
    int speed = 1;                  // animations: 0 fast, 1 normal, 2 slow, 3 off
    int tilt = 42;                  // card tilt in degrees, 0..60
    int size = 100;                 // card size in percent, 80..130
    bool autoTuck = true;           // slide away while a window covers the sidebar
    bool edgeReveal = true;         // the pointer at the screen edge brings a tucked sidebar back
    bool keepTray = true;           // windows closed to the tray keep their card
    bool fitSnapped = true;         // a window snapped to the sidebar's side is moved next to it
    bool right = false;             // sidebar on the right edge of the screen
    std::wstring monitor;           // device name (\\.\DISPLAY2); empty: the primary monitor
    std::vector<std::wstring> excluded;     // app ids (see wt::AppId) the sidebar leaves alone

    void Load();
    void Save() const;

    UINT HotkeyModifiers() const;
    // Scales an animation length for the chosen speed; "off" leaves 1ms, so completion logic still runs.
    int Ms(int ms) const;
    float SizeFactor() const { return size / 100.f; }
};
