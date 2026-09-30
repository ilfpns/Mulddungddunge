#pragma once
#include "pch.h"

// Which apps are playing sound, and muting one. Audio session notifications are only delivered to a
// multithreaded apartment, so a worker thread owns the audio objects; it sleeps until Windows reports
// a change (a session starting or stopping, a mute, the default device changing), then posts `msg`
// to the window. No polling.
class Audio
{
public:
    struct Playing
    {
        std::wstring app;                           // executable path
        std::wstring model;                         // AppUserModelID of a packaged app (UWP cards go by it)
        bool muted;
    };
    // What Windows' media controls know is playing (the volume flyout's media panel): a browser
    // reports each web app under its own id ("Chrome._crx_<id>") and a tab with its media title, which
    // tells apart windows that all play through one browser process.
    struct Media
    {
        std::wstring model;                         // the session's source AppUserModelID
        std::wstring title;
    };
    void Start(HWND notify, UINT msg);
    void Stop();
    std::vector<Playing> Current();                 // after `msg`: apps with an active session
    std::vector<Media> CurrentMedia();              // after `msg`: media sessions playing now
    void SetMute(std::wstring const& cardApp, bool mute);   // every session of the app a card shows

private:
    struct Worker;
    std::shared_ptr<Worker> m_worker;
};
