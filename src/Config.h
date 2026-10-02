#pragma once
#include "pch.h"

// Colors the line running around a card can take (pointer on it, file held over it, new alert):
// white, orange, blue, green, purple, pink. Bright, so a hairline still shows on any picture.
inline constexpr uint8_t kTraceColors[][3] = {
    { 255, 255, 255 }, { 255, 149, 0 }, { 10, 132, 255 }, { 48, 209, 88 }, { 191, 90, 242 }, { 255, 55, 95 },
};
inline constexpr int kTraceColorCount = static_cast<int>(std::size(kTraceColors));
// One more choice after those: each card in its app's logo color (white for a logo without one).
inline constexpr int kTraceAppColor = kTraceColorCount;

// User settings, kept under HKCU\Software\StageManager\Settings. Read once at startup, written on change.
struct Config
{
    int cards = 4;                  // sidebar cards, 1..9 (a folder is one)
    int quality = 1;                // window pictures: 0 low, 1 normal, 2 high detail
    bool iconsOnly = false;         // no window pictures, app icon cards only: least GPU memory, no captures
    bool hoverTrace = true;         // a thin line runs around the card the pointer is on
    int cardStyle = 1;              // stand-in cards (no picture of the window): 0 gray, 1 frosted glass
    int traceColor = 0;             // the card line's color, one for all its uses (index into kTraceColors, or kTraceAppColor)
    int traceWidth = 2;             // the card line's thickness in px on screen, 1..5
    bool alerts = true;             // a card whose app flashes its taskbar button gets an orange dot
    bool sounds = true;             // a card whose app plays sound gets a speaker (click: mute)
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
    std::vector<std::wstring> quitApps;     // app ids quit outright when closed to the tray (X)

    void Load();
    void Save() const;

    UINT HotkeyModifiers() const;
    // Scales an animation length for the chosen speed; "off" leaves 1ms, so completion logic still runs.
    int Ms(int ms) const;
    float SizeFactor() const { return size / 100.f; }
};
