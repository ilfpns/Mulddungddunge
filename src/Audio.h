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
    void Start(HWND notify, UINT msg);
    void Stop();
    std::vector<Playing> Current();                 // after `msg`: apps with an active session
    void SetMute(std::wstring const& cardApp, bool mute);   // every session of the app a card shows

private:
    struct Worker;
    std::shared_ptr<Worker> m_worker;
};
