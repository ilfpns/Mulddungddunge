#include "Audio.h"
#include "WindowTracker.h"
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <mutex>
#include <unordered_map>
#include <winrt/Windows.Media.Control.h>

using namespace winrt::Windows::Media::Control;

namespace
{
    // Callbacks from the audio engine's threads: each only wakes the worker.
    struct Sink : winrt::implements<Sink, IAudioSessionNotification, IAudioSessionEvents, IMMNotificationClient>
    {
        HANDLE wake;
        std::atomic<bool>* deviceChanged;
        Sink(HANDLE w, std::atomic<bool>* d) : wake(w), deviceChanged(d) {}
        void Wake() { SetEvent(wake); }

        HRESULT __stdcall OnSessionCreated(IAudioSessionControl*) noexcept override { Wake(); return S_OK; }

        HRESULT __stdcall OnDisplayNameChanged(LPCWSTR, LPCGUID) noexcept override { return S_OK; }
        HRESULT __stdcall OnIconPathChanged(LPCWSTR, LPCGUID) noexcept override { return S_OK; }
        HRESULT __stdcall OnSimpleVolumeChanged(float, BOOL, LPCGUID) noexcept override { Wake(); return S_OK; }
        HRESULT __stdcall OnChannelVolumeChanged(DWORD, float*, DWORD, LPCGUID) noexcept override { return S_OK; }
        HRESULT __stdcall OnGroupingParamChanged(LPCGUID, LPCGUID) noexcept override { return S_OK; }
        HRESULT __stdcall OnStateChanged(AudioSessionState) noexcept override { Wake(); return S_OK; }
        HRESULT __stdcall OnSessionDisconnected(AudioSessionDisconnectReason) noexcept override { Wake(); return S_OK; }

        HRESULT __stdcall OnDeviceStateChanged(LPCWSTR, DWORD) noexcept override { return S_OK; }
        HRESULT __stdcall OnDeviceAdded(LPCWSTR) noexcept override { return S_OK; }
        HRESULT __stdcall OnDeviceRemoved(LPCWSTR) noexcept override { return S_OK; }
        HRESULT __stdcall OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) noexcept override
        {
            if (flow == eRender && role == eConsole)
            {
                *deviceChanged = true;
                Wake();
            }
            return S_OK;
        }
        HRESULT __stdcall OnPropertyValueChanged(LPCWSTR, PROPERTYKEY const) noexcept override { return S_OK; }
    };
}

struct Audio::Worker
{
    HWND notify{};
    UINT msg = 0;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<bool> quit{ false }, deviceChanged{ true };
    std::mutex lock;
    std::vector<Playing> playing;                   // guarded by lock
    std::vector<Media> media;                       // guarded by lock
    std::vector<std::pair<std::wstring, bool>> mutes;   // requests, guarded by lock
    std::thread thread;

    ~Worker() { CloseHandle(wake); }

    struct Session
    {
        std::wstring id;                            // session instance identifier
        winrt::com_ptr<IAudioSessionControl2> control;
    };

    void Run()
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try
        {
            Loop();
        }
        catch (...)
        {
        }
        winrt::uninit_apartment();
    }

    void Loop()
    {
        auto sink = winrt::make_self<Sink>(wake, &deviceChanged);
        // Media sessions: their changes wake the worker like audio ones do.
        GlobalSystemMediaTransportControlsSessionManager mediaManager{ nullptr };
        winrt::event_token sessionsToken{};
        std::vector<std::pair<GlobalSystemMediaTransportControlsSession, std::pair<winrt::event_token, winrt::event_token>>> watched;
        auto unwatch = [&] {
            for (auto& [s, t] : watched)
            {
                try
                {
                    s.PlaybackInfoChanged(t.first);
                    s.MediaPropertiesChanged(t.second);
                }
                catch (...)
                {
                }
            }
            watched.clear();
        };
        try
        {
            mediaManager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            sessionsToken = mediaManager.SessionsChanged([w = wake](auto&&, auto&&) { SetEvent(w); });
        }
        catch (...)
        {
            mediaManager = nullptr;
        }
        auto devices = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
        devices->RegisterEndpointNotificationCallback(sink.get());
        winrt::com_ptr<IAudioSessionManager2> manager;
        std::vector<Session> sessions;
        std::unordered_map<DWORD, std::pair<std::wstring, std::wstring>> images;    // pid -> executable, model id

        auto detach = [&] {
            for (auto& s : sessions)
                s.control->UnregisterAudioSessionNotification(sink.get());
            sessions.clear();
            if (manager)
                manager->UnregisterSessionNotification(sink.get());
            manager = nullptr;
        };

        while (!quit)
        {
            if (deviceChanged.exchange(false))
            {
                detach();
                winrt::com_ptr<IMMDevice> device;
                if (SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, device.put())) &&
                    SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.put_void())))
                {
                    winrt::com_ptr<IAudioSessionEnumerator> first;      // required before notifications start
                    manager->GetSessionEnumerator(first.put());
                    manager->RegisterSessionNotification(sink.get());
                }
            }

            // Mute requests from the UI thread.
            decltype(mutes) todo;
            {
                std::lock_guard g(lock);
                todo.swap(mutes);
            }

            std::vector<Playing> now;
            std::vector<Session> seen;
            winrt::com_ptr<IAudioSessionEnumerator> list;
            int count = 0;
            if (manager && SUCCEEDED(manager->GetSessionEnumerator(list.put())))
                list->GetCount(&count);
            for (int i = 0; i < count; ++i)
            {
                winrt::com_ptr<IAudioSessionControl> c;
                if (FAILED(list->GetSession(i, c.put())))
                    continue;
                auto control = c.try_as<IAudioSessionControl2>();
                if (!control)
                    continue;
                wchar_t* raw = nullptr;
                std::wstring id;
                if (SUCCEEDED(control->GetSessionInstanceIdentifier(&raw)) && raw)
                {
                    id = raw;
                    CoTaskMemFree(raw);
                }
                auto known = std::find_if(sessions.begin(), sessions.end(), [&](auto& s) { return s.id == id; });
                if (known == sessions.end())
                    control->RegisterAudioSessionNotification(sink.get());
                else
                    control = known->control;       // the one our events are registered on
                seen.push_back({ id, control });

                DWORD pid = 0;
                if (FAILED(control->GetProcessId(&pid)) || !pid)
                    continue;                       // system sounds
                auto& known2 = images[pid];
                if (known2.first.empty())
                    known2 = { wt::ImageOf(pid), wt::ModelOf(pid) };
                auto const& [image, model] = known2;
                auto volume = control.try_as<ISimpleAudioVolume>();
                for (auto& [app, mute] : todo)
                    if (volume && wt::IsCardApp(app, image, model))
                        volume->SetMute(mute, nullptr);
                AudioSessionState state = AudioSessionStateInactive;
                control->GetState(&state);
                BOOL muted = FALSE;
                if (volume)
                    volume->GetMute(&muted);
                if (state == AudioSessionStateActive && !image.empty())
                {
                    auto same = std::find_if(now.begin(), now.end(), [&](auto& p) { return wt::SameApp(p.app, image); });
                    if (same == now.end())
                        now.push_back({ image, model, muted != FALSE });
                    else
                        same->muted = same->muted && muted;     // muted only if all of its sessions are
                }
            }
            for (auto& s : sessions)
                if (std::none_of(seen.begin(), seen.end(), [&](auto& n) { return n.id == s.id; }))
                    s.control->UnregisterAudioSessionNotification(sink.get());
            sessions = std::move(seen);
            if (images.size() > 64)
                images.clear();                     // ids are reused; start over now and then

            // Media sessions playing now, watched again each time (the list may have changed).
            std::vector<Media> nowMedia;
            if (mediaManager)
            {
                try
                {
                    unwatch();
                    for (auto const& s : mediaManager.GetSessions())
                    {
                        auto t1 = s.PlaybackInfoChanged([w = wake](auto&&, auto&&) { SetEvent(w); });
                        auto t2 = s.MediaPropertiesChanged([w = wake](auto&&, auto&&) { SetEvent(w); });
                        watched.push_back({ s, { t1, t2 } });
                        auto info = s.GetPlaybackInfo();
                        if (!info || info.PlaybackStatus() != GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
                            continue;
                        std::wstring title;
                        try
                        {
                            if (auto props = s.TryGetMediaPropertiesAsync().get())
                                title = props.Title().c_str();
                        }
                        catch (...)
                        {
                        }
                        nowMedia.push_back({ std::wstring(s.SourceAppUserModelId()), title });
                    }
                }
                catch (...)
                {
                }
            }

            bool changed;
            {
                std::lock_guard g(lock);
                changed = now.size() != playing.size() ||
                          !std::equal(now.begin(), now.end(), playing.begin(), [](auto& a, auto& b) { return a.app == b.app && a.muted == b.muted; }) ||
                          nowMedia.size() != media.size() ||
                          !std::equal(nowMedia.begin(), nowMedia.end(), media.begin(), [](auto& a, auto& b) { return a.model == b.model && a.title == b.title; });
                if (changed)
                {
                    playing = std::move(now);
                    media = std::move(nowMedia);
                }
            }
            if (changed)
                PostMessageW(notify, msg, 0, 0);
            WaitForSingleObject(wake, INFINITE);
        }
        detach();
        unwatch();
        if (mediaManager)
            mediaManager.SessionsChanged(sessionsToken);
        devices->UnregisterEndpointNotificationCallback(sink.get());
    }
};

void Audio::Start(HWND notify, UINT msg)
{
    if (m_worker)
        return;
    m_worker = std::make_shared<Worker>();
    m_worker->notify = notify;
    m_worker->msg = msg;
    m_worker->thread = std::thread([w = m_worker] { w->Run(); });
}

void Audio::Stop()
{
    if (!m_worker)
        return;
    m_worker->quit = true;
    SetEvent(m_worker->wake);
    if (m_worker->thread.joinable())
        m_worker->thread.join();
    m_worker = nullptr;
}

std::vector<Audio::Playing> Audio::Current()
{
    if (!m_worker)
        return {};
    std::lock_guard g(m_worker->lock);
    return m_worker->playing;
}

std::vector<Audio::Media> Audio::CurrentMedia()
{
    if (!m_worker)
        return {};
    std::lock_guard g(m_worker->lock);
    return m_worker->media;
}

void Audio::SetMute(std::wstring const& cardApp, bool mute)
{
    if (!m_worker)
        return;
    {
        std::lock_guard g(m_worker->lock);
        m_worker->mutes.push_back({ cardApp, mute });
    }
    SetEvent(m_worker->wake);
}
