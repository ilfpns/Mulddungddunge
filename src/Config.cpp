#include "Config.h"
#include "WindowTracker.h"

namespace
{
    constexpr wchar_t kKey[] = L"Software\\StageManager\\Settings";

    int ReadInt(wchar_t const* name, int fallback, int lo, int hi)
    {
        DWORD value = 0, size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
            return fallback;
        return std::clamp(static_cast<int>(value), lo, hi);
    }

    void WriteInt(wchar_t const* name, int value)
    {
        DWORD v = static_cast<DWORD>(value);
        RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_DWORD, &v, sizeof(v));
    }
}

namespace
{
    std::vector<std::wstring> ReadList(wchar_t const* name)
    {
        std::vector<std::wstring> out;
        DWORD bytes = 0;
        if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &bytes) == ERROR_SUCCESS && bytes)
        {
            std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
            if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_MULTI_SZ, nullptr, buffer.data(), &bytes) == ERROR_SUCCESS)
                for (wchar_t const* p = buffer.c_str(); *p; p += wcslen(p) + 1)
                    out.emplace_back(p);
        }
        return out;
    }

    void WriteList(wchar_t const* name, std::vector<std::wstring> const& list)
    {
        std::wstring buffer;
        for (auto& app : list)
            buffer.append(app).push_back(L'\0');
        buffer.push_back(L'\0');
        RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_MULTI_SZ, buffer.data(),
            static_cast<DWORD>(buffer.size() * sizeof(wchar_t)));
    }
}

void Config::Load()
{
    cards = ReadInt(L"Cards", cards, 1, 6);
    quality = ReadInt(L"Quality", quality, 0, 2);
    iconsOnly = ReadInt(L"IconsOnly", iconsOnly, 0, 1) != 0;
    hoverTrace = ReadInt(L"HoverTrace", hoverTrace, 0, 1) != 0;
    cardStyle = ReadInt(L"CardStyle", cardStyle, 0, 1);
    traceColor = ReadInt(L"TraceColor", traceColor, 0, kTraceColorCount - 1);
    traceWidth = ReadInt(L"TraceWidth", traceWidth, 1, 5);
    alerts = ReadInt(L"Alerts", alerts, 0, 1) != 0;
    sounds = ReadInt(L"Sounds", sounds, 0, 1) != 0;
    hotkeys = ReadInt(L"Hotkeys", hotkeys, 0, 1) != 0;
    hotkeyMod = ReadInt(L"HotkeyModifier", hotkeyMod, 0, 2);
    speed = ReadInt(L"Speed", speed, 0, 3);
    tilt = ReadInt(L"Tilt", tilt, 0, 60);
    size = ReadInt(L"Size", size, 80, 130);
    autoTuck = ReadInt(L"AutoTuck", autoTuck, 0, 1) != 0;
    edgeReveal = ReadInt(L"EdgeReveal", edgeReveal, 0, 1) != 0;
    keepTray = ReadInt(L"KeepTray", keepTray, 0, 1) != 0;
    fitSnapped = ReadInt(L"FitSnapped", fitSnapped, 0, 1) != 0;
    right = ReadInt(L"Right", right, 0, 1) != 0;

    wchar_t name[64]{};
    DWORD bytes = sizeof(name);
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, L"Monitor", RRF_RT_REG_SZ, nullptr, name, &bytes) == ERROR_SUCCESS)
        monitor = name;

    excluded = ReadList(L"Excluded");
    quitApps = ReadList(L"QuitApps");
    // Apps the sidebar leaves alone out of the box (once: turned back on in Settings, they stay on).
    // Store apps are compared without their version folder, so any version matches.
    static constexpr wchar_t const* kUnmanaged[] = {
        L"C:\\Program Files\\WindowsApps\\OpenAI.ChatGPT-Desktop_1.0.0.0_x64__2p2nqsd0c76g0\\app\\ChatGPT Classic.exe",
    };
    if (!ReadInt(L"DefaultsApplied", 0, 0, 1))
    {
        for (auto app : kUnmanaged)
            if (std::none_of(excluded.begin(), excluded.end(), [&](auto& e) { return wt::SameApp(e, app); }))
                excluded.emplace_back(app);
        WriteList(L"Excluded", excluded);
        WriteInt(L"DefaultsApplied", 1);
    }
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kKey, L"QuitOnClose");     // the old all-apps switch
}

void Config::Save() const
{
    WriteInt(L"Cards", cards);
    WriteInt(L"Quality", quality);
    WriteInt(L"IconsOnly", iconsOnly);
    WriteInt(L"HoverTrace", hoverTrace);
    WriteInt(L"CardStyle", cardStyle);
    WriteInt(L"TraceColor", traceColor);
    WriteInt(L"TraceWidth", traceWidth);
    WriteInt(L"Alerts", alerts);
    WriteInt(L"Sounds", sounds);
    WriteInt(L"Hotkeys", hotkeys);
    WriteInt(L"HotkeyModifier", hotkeyMod);
    WriteInt(L"Speed", speed);
    WriteInt(L"Tilt", tilt);
    WriteInt(L"Size", size);
    WriteInt(L"AutoTuck", autoTuck);
    WriteInt(L"EdgeReveal", edgeReveal);
    WriteInt(L"KeepTray", keepTray);
    WriteInt(L"FitSnapped", fitSnapped);
    WriteInt(L"Right", right);
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey, L"Monitor", REG_SZ, monitor.c_str(),
        static_cast<DWORD>((monitor.size() + 1) * sizeof(wchar_t)));
    WriteList(L"Excluded", excluded);
    WriteList(L"QuitApps", quitApps);
}

UINT Config::HotkeyModifiers() const
{
    switch (hotkeyMod)
    {
    case 1: return MOD_CONTROL | MOD_ALT;
    case 2: return MOD_SHIFT | MOD_ALT;
    default: return MOD_ALT;
    }
}

int Config::Ms(int ms) const
{
    static constexpr float factor[] = { 0.65f, 1.f, 1.5f, 0.f };
    return std::max(1, static_cast<int>(ms * factor[std::clamp(speed, 0, 3)]));
}
