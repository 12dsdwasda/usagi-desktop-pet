#include "lyrics.h"
#include "gemini.h"
#include "json.h"
#include <algorithm>
#include <cmath>
#include <thread>

std::vector<LyricLine> ParseLrc(const std::wstring& lrc) {
    std::vector<LyricLine> out;
    size_t pos = 0;
    while (pos <= lrc.size()) {
        size_t end = lrc.find(L'\n', pos);
        if (end == std::wstring::npos) end = lrc.size();
        std::wstring line = lrc.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        // 한 줄 앞에 [mm:ss.xx] 가 여러 개 붙을 수 있다
        std::vector<double> times;
        size_t i = 0;
        while (i < line.size() && line[i] == L'[') {
            size_t close = line.find(L']', i);
            if (close == std::wstring::npos) break;
            int mm = 0;
            double ss = 0;
            if (swscanf_s(line.substr(i + 1, close - i - 1).c_str(), L"%d:%lf", &mm, &ss) == 2) times.push_back(mm * 60 + ss);
            i = close + 1;
        }
        if (times.empty()) continue;
        std::wstring text = line.substr(i);
        while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
        while (!text.empty() && iswspace(text.back())) text.pop_back();
        for (double t : times) out.push_back({t, text});
        if (end == lrc.size()) break;
    }
    std::stable_sort(out.begin(), out.end(), [](const LyricLine& a, const LyricLine& b) { return a.t < b.t; });
    return out;
}

namespace {

// "Song - Remastered 2011", "Song (feat. X)" 같은 꼬리표를 떼어 검색이 잘 되게
std::wstring CleanTitle(std::wstring t) {
    for (const wchar_t* sep : {L" - ", L" (", L" ["}) {
        size_t p = t.find(sep);
        if (p != std::wstring::npos && p > 0) t = t.substr(0, p);
    }
    return t;
}

std::wstring Q(const wchar_t* k, const std::wstring& v) { return std::wstring(k) + L"=" + UrlEncodeQ(v); }

// 결과 하나에서 synced 가사를 꺼낸다 (길이가 너무 다르면 다른 버전이라 버림)
bool Take(const Json& j, double duration, std::vector<LyricLine>& lines) {
    std::string lrc = j.StrOr("syncedLyrics");
    if (lrc.empty()) return false;
    double d = j.NumOr("duration", 0);
    if (duration > 0 && d > 0 && std::abs(d - duration) > 8) return false;
    lines = ParseLrc(FromUtf8(lrc));
    return !lines.empty();
}

void Fetch(std::shared_ptr<LyricsEntry> e, std::wstring title, std::wstring artist, std::wstring album, double duration) {
    const wchar_t* host = L"lrclib.net";
    std::vector<LyricLine> lines;
    // 1) 정확히 찾기
    std::wstring path = L"/api/get?" + Q(L"track_name", title) + L"&" + Q(L"artist_name", artist);
    if (!album.empty()) path += L"&" + Q(L"album_name", album);
    if (duration > 0) path += L"&duration=" + std::to_wstring(int(std::lround(duration)));
    HttpResult r = HttpsGet(host, path, 15000);
    Json j;
    bool found = r.status == 200 && Json::Parse(r.body, j) && Take(j, duration, lines);
    // 2) 검색 (꼬리표 뗀 제목으로)
    if (!found) {
        for (const std::wstring& t : {title, CleanTitle(title)}) {
            HttpResult s = HttpsGet(host, L"/api/search?" + Q(L"track_name", t) + L"&" + Q(L"artist_name", artist), 15000);
            Json arr;
            if (s.status == 200 && Json::Parse(s.body, arr) && arr.t == Json::Arr)
                for (auto& it : arr.a)
                    if (Take(it, duration, lines)) { found = true; break; }
            if (found || t == CleanTitle(title)) break;
        }
    }
    Log(L"lyrics: '%s' - %s → %s (%zu줄, HTTP %d)", title.c_str(), artist.c_str(), found ? L"찾음" : L"없음", lines.size(), r.status);
    // e 는 공유 포인터: 메인 스레드는 status 가 바뀐 뒤에만 lines 를 읽는다
    e->lines = std::move(lines);
    e->status = found ? LyricsEntry::Ready : LyricsEntry::None;
}

}  // namespace

std::shared_ptr<const LyricsEntry> LyricsFetcher::Get(const std::wstring& title, const std::wstring& artist,
                                                      const std::wstring& album, double duration) {
    std::wstring key = artist + L"\n" + title;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    auto e = std::make_shared<LyricsEntry>();
    cache_[key] = e;
    std::thread(Fetch, e, title, artist, album, duration).detach();
    return e;
}
