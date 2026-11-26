#include "media.h"
#undef GetCurrentTime
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <chrono>

#pragma comment(lib, "windowsapp.lib")

using namespace winrt::Windows::Media::Control;

namespace {
bool IsSpotify(const std::wstring& aumid) {
    std::wstring l = aumid;
    for (auto& c : l) c = towlower(c);
    return l.find(L"spotify") != std::wstring::npos;
}
double Sec(winrt::Windows::Foundation::TimeSpan t) { return std::chrono::duration<double>(t).count(); }
}  // namespace

void MediaWatcher::Start(bool spotifyOnly) {
    if (th_.joinable()) return;
    spotifyOnly_ = spotifyOnly;
    stop_ = false;
    th_ = std::thread([this] { Run(); });
}

void MediaWatcher::Stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

MediaSnapshot MediaWatcher::Get() {
    std::lock_guard<std::mutex> lk(mu_);
    return snap_;
}

void MediaWatcher::Command(const std::wstring& cmd) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        cmds_.push_back(cmd);
    }
    cv_.notify_all();
}

void MediaWatcher::Run() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    GlobalSystemMediaTransportControlsSessionManager mgr{nullptr};
    std::wstring lastKey;
    bool logged = false;
    while (true) {
        std::vector<std::wstring> cmds;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(400), [this] { return stop_ || !cmds_.empty(); });
            if (stop_) break;
            cmds.swap(cmds_);
        }
        MediaSnapshot s;
        try {
            if (!mgr) mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            GlobalSystemMediaTransportControlsSession pick{nullptr};
            for (auto const& ses : mgr.GetSessions())
                if (IsSpotify(ses.SourceAppUserModelId().c_str())) { pick = ses; break; }
            if (!pick && !spotifyOnly_) pick = mgr.GetCurrentSession();
            if (pick) {
                for (auto& c : cmds) {
                    if (c == L"play_pause") pick.TryTogglePlayPauseAsync().get();
                    else if (c == L"play") pick.TryPlayAsync().get();
                    else if (c == L"pause") pick.TryPauseAsync().get();
                    else if (c == L"next") pick.TrySkipNextAsync().get();
                    else if (c == L"previous") pick.TrySkipPreviousAsync().get();
                }
                s.ok = true;
                s.app = pick.SourceAppUserModelId().c_str();
                s.spotify = IsSpotify(s.app);
                auto props = pick.TryGetMediaPropertiesAsync().get();
                s.title = props.Title().c_str();
                s.artist = props.Artist().c_str();
                s.album = props.AlbumTitle().c_str();
                s.playing = pick.GetPlaybackInfo().PlaybackStatus() ==
                            GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                auto tl = pick.GetTimelineProperties();
                s.duration = Sec(tl.EndTime() - tl.StartTime());
                double pos = Sec(tl.Position() - tl.StartTime());
                // 위치는 마지막 갱신 시각 기준이므로 그 뒤 흐른 시간을 더한다
                if (s.playing) {
                    double since = Sec(winrt::clock::now() - tl.LastUpdatedTime());
                    if (since > 0 && since < 3600) pos += since;
                }
                if (s.duration > 0) pos = std::min(pos, s.duration);
                s.position = std::max(0.0, pos);
            }
            logged = false;
        } catch (winrt::hresult_error const& e) {
            if (!logged) { Log(L"media: 오류 0x%08X %s", unsigned(e.code()), e.message().c_str()); logged = true; }
            mgr = nullptr;
        } catch (...) {
            if (!logged) { Log(L"media: 알 수 없는 오류"); logged = true; }
            mgr = nullptr;
        }
        s.sampledAt = NowSec();
        std::wstring key = s.ok ? s.artist + L" - " + s.title : L"";
        if (key != lastKey) {
            lastKey = key;
            Log(L"media: %s %s", s.ok ? s.app.c_str() : L"(세션 없음)", key.c_str());
        }
        std::lock_guard<std::mutex> lk(mu_);
        snap_ = s;
    }
}
