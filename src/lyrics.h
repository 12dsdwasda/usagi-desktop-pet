#pragma once
#include "common.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>

// 시간이 맞춰진 가사(LRC)를 LRCLIB(https://lrclib.net, 무료·키 없음)에서 받아 온다.
// 곡 제목·가수·앨범·길이만 보낸다.
struct LyricLine {
    double t;           // 초
    std::wstring text;  // 빈 줄 = 간주
};

struct LyricsEntry {
    enum Status { Pending, Ready, None };
    std::atomic<Status> status{Pending};  // Ready 가 된 뒤에만 lines 를 읽는다
    std::vector<LyricLine> lines;
};

class LyricsFetcher {
public:
    // 처음 부르면 작업 스레드에서 받기 시작하고 Pending 을 돌려준다
    std::shared_ptr<const LyricsEntry> Get(const std::wstring& title, const std::wstring& artist, const std::wstring& album,
                                           double duration);

private:
    std::mutex mu_;
    std::map<std::wstring, std::shared_ptr<LyricsEntry>> cache_;
};

// "[01:02.34] 가사" 형식 파싱 (테스트용으로 공개)
std::vector<LyricLine> ParseLrc(const std::wstring& lrc);
