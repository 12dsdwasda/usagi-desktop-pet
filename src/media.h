#pragma once
#include "common.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

// Windows 미디어 컨트롤(GSMTC)로 스포티파이에서 지금 재생 중인 곡과 재생 위치를 읽는다.
// 로그인·API 키 없이 동작한다. 작업 스레드(MTA)에서 0.4초마다 갱신.
struct MediaSnapshot {
    bool ok = false;          // 세션 있음
    bool spotify = false;     // 스포티파이 세션인가
    bool playing = false;
    std::wstring app, title, artist, album;
    double position = 0;      // 초, sampledAt 시점
    double duration = 0;      // 초 (모르면 0)
    double sampledAt = 0;     // NowSec()
    // 지금 시각의 재생 위치 (재생 중이면 흐른 시간을 더함)
    double PositionAt(double now) const { return position + (playing ? now - sampledAt : 0); }
};

class MediaWatcher {
public:
    void Start(bool spotifyOnly);
    void Stop();
    MediaSnapshot Get();
    // 스포티파이(없으면 현재 세션) 재생 조절: "play_pause", "play", "pause", "next", "previous"
    void Command(const std::wstring& cmd);
    ~MediaWatcher() { Stop(); }

private:
    void Run();
    std::thread th_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false, spotifyOnly_ = true;
    MediaSnapshot snap_;
    std::vector<std::wstring> cmds_;
};
