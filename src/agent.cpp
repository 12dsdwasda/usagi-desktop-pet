#include "agent.h"
#include "gemini.h"
#include "json.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <ctime>
#include <map>
#include <memory>
#include <thread>

namespace {

constexpr UINT WM_AGENT_RESULT = WM_APP + 40;
constexpr int IDC_LOG = 101, IDC_INPUT = 102, IDC_NEW = 103, IDC_KEYBTN = 104;
constexpr UINT_PTR TIMER_BASE = 5000;
constexpr int kMaxRounds = 6;      // 한 번 물어볼 때 도구 호출을 주고받는 최대 횟수
constexpr size_t kMaxHistory = 40;  // 기억하는 대화 조각 수
const wchar_t* kChatClass = L"UsagiDesktopPet.Chat";
const wchar_t* kAgentMsgClass = L"UsagiDesktopPet.AgentMsg";
const wchar_t* kKeyClass = L"UsagiDesktopPet.Key";

struct AsyncResult {
    int gen = 0;
    HttpResult http;
    std::string model;
};

struct State {
    HINSTANCE inst{};
    AgentHost host;
    std::vector<std::string> models;
    HWND msgWnd{}, chat{}, log{}, input{}, sendBtn{}, newBtn{}, keyBtn{};
    HFONT font{};
    UINT fontDpi = 0;
    Json history = Json::Arr_();
    bool busy = false;
    int gen = 0, rounds = 0;
    UINT_PTR nextTimer = TIMER_BASE;
    std::map<UINT_PTR, std::wstring> timers;
} A;

// ---------- 도구 선언 (Gemini function calling) ----------
const char* kTools = R"JSON([{"functionDeclarations":[
{"name":"open_url","description":"사용자의 기본 브라우저로 웹 주소(http/https)를 연다.","parameters":{"type":"object","properties":{"url":{"type":"string"}},"required":["url"]}},
{"name":"search_web","description":"브라우저에서 구글 검색 결과를 연다. 검색 결과 내용은 돌려받지 못한다.","parameters":{"type":"object","properties":{"query":{"type":"string"}},"required":["query"]}},
{"name":"open_app","description":"자주 쓰는 Windows 앱을 실행한다.","parameters":{"type":"object","properties":{"app":{"type":"string","enum":["notepad","calculator","paint","file_explorer","settings","task_manager","screen_snip","browser"]}},"required":["app"]}},
{"name":"open_folder","description":"사용자 폴더를 파일 탐색기로 연다.","parameters":{"type":"object","properties":{"folder":{"type":"string","enum":["desktop","documents","downloads","pictures","music","videos"]}},"required":["folder"]}},
{"name":"set_timer","description":"타이머/알림. 시간이 되면 우사기가 말풍선과 소리로 알려준다. 프로그램이 꺼지면 사라진다.","parameters":{"type":"object","properties":{"seconds":{"type":"number","description":"1~86400초"},"message":{"type":"string","description":"알릴 때 할 말"}},"required":["seconds","message"]}},
{"name":"list_timers","description":"대기 중인 타이머 목록.","parameters":{"type":"object","properties":{}}},
{"name":"get_status","description":"현재 날짜와 시각, 배터리, 메모리 사용률, 켜진 시간, 우사기 상태를 알려준다.","parameters":{"type":"object","properties":{}}},
{"name":"list_windows","description":"우사기가 끌고 갈 수 있는 열린 창들의 제목 목록.","parameters":{"type":"object","properties":{}}},
{"name":"drag_window","description":"우사기가 걸어가서 제목에 이 글자가 들어간 창을 150~250 DIP 정도 끌고 간다.","parameters":{"type":"object","properties":{"title_contains":{"type":"string"}},"required":["title_contains"]}},
{"name":"set_music_mode","description":"음악이 나올 때 우사기의 반응.","parameters":{"type":"object","properties":{"mode":{"type":"string","enum":["dance","rap","off"]}},"required":["mode"]}},
{"name":"media_control","description":"재생 중인 미디어와 시스템 볼륨을 조절한다.","parameters":{"type":"object","properties":{"action":{"type":"string","enum":["play_pause","next","previous","volume_up","volume_down","mute"]},"steps":{"type":"number","description":"볼륨 조절 횟수(1번에 2%), 기본 5"}},"required":["action"]}},
{"name":"write_note","description":"메모장을 열어 우사기가 직접 글을 타이핑한다. 메모장이 이미 열려 있으면 문서 폴더에 텍스트 파일로 저장해 연다.","parameters":{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}},
{"name":"copy_to_clipboard","description":"텍스트를 클립보드에 복사한다.","parameters":{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}},
{"name":"read_clipboard","description":"클립보드의 텍스트를 읽는다. 사용자가 클립보드 내용을 보라고 분명히 요청했을 때만 쓴다.","parameters":{"type":"object","properties":{}}},
{"name":"pet_action","description":"우사기의 장난 동작. prank=창 끌기나 메모장 장난, grab_cursor=마우스 포인터 가져가기, cancel=하던 일 멈추기.","parameters":{"type":"object","properties":{"action":{"type":"string","enum":["prank","grab_cursor","cancel"]}},"required":["action"]}}
]}])JSON";

std::wstring NowText() {
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    static const wchar_t* days[] = {L"일", L"월", L"화", L"수", L"목", L"금", L"토"};
    wchar_t buf[96];
    swprintf_s(buf, L"%04d-%02d-%02d (%s) %02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, days[lt.tm_wday], lt.tm_hour,
               lt.tm_min);
    return buf;
}

std::string SystemPrompt() {
    std::wstring p =
        L"너는 사용자의 Windows 바탕화면 위를 돌아다니는 작은 토끼 캐릭터 '우사기'이고, 동시에 사용자를 돕는 에이전트다.\n"
        L"- 항상 한국어 반말로, 밝고 장난스럽게 말해. 말버릇(야하!, 우라!, 푸루!, 하아?, 이~)을 가끔 섞어.\n"
        L"- 답은 말풍선에 나오니 보통 1~2문장, 80자 안쪽으로 짧게. 사용자가 자세한 설명을 원할 때만 길게.\n"
        L"- 마크다운, 표, 이모지는 쓰지 마. 말풍선에 그대로 보인다.\n"
        L"- 컴퓨터에서 할 일은 도구로 직접 해. 도구로 못 하는 일(파일 삭제, 설치, 결제, 로그인, 메일 보내기 등)은 못 한다고 솔직히 말해.\n"
        L"- 하지 않은 일을 했다고 말하지 마. 도구 결과에 오류가 있으면 그대로 알려.\n"
        L"- 웹 검색은 브라우저에 결과를 띄우는 것까지만 할 수 있다. 검색 결과 내용을 읽은 척하지 마.\n"
        L"- 지금 시각: " + NowText() + L"\n";
    return ToUtf8(p);
}

Json TextPart(const std::string& t) {
    Json p = Json::Obj_();
    p.Set("text", Json::Str_(t));
    return p;
}

Json Ok(const std::wstring& msg) {
    Json r = Json::Obj_();
    r.Set("result", Json::Str_(ToUtf8(msg)));
    return r;
}
Json Err(const std::wstring& msg) {
    Json r = Json::Obj_();
    r.Set("error", Json::Str_(ToUtf8(msg)));
    return r;
}

// ---------- 대화창 ----------
void Append(const std::wstring& line) {
    if (!A.log) return;
    std::wstring t;
    for (wchar_t c : line) {
        if (c == L'\n') t += L"\r\n";
        else if (c != L'\r') t += c;
    }
    t += L"\r\n";
    int n = GetWindowTextLengthW(A.log);
    if (n > 60000) {  // 너무 길어지면 앞부분을 버림
        SendMessageW(A.log, EM_SETSEL, 0, 20000);
        SendMessageW(A.log, EM_REPLACESEL, FALSE, LPARAM(L""));
        n = GetWindowTextLengthW(A.log);
    }
    SendMessageW(A.log, EM_SETSEL, n, n);
    SendMessageW(A.log, EM_REPLACESEL, FALSE, LPARAM(t.c_str()));
    SendMessageW(A.log, EM_SCROLLCARET, 0, 0);
}

void UpdateUi() {
    if (!A.chat) return;
    EnableWindow(A.sendBtn, !A.busy);
    SetWindowTextW(A.chat, A.busy ? L"우사기와 대화 — 생각 중…" : L"우사기와 대화");
}

int Dip(int v) { return MulDiv(v, int(A.fontDpi ? A.fontDpi : 96), 96); }

void ApplyFont(UINT dpi) {
    if (A.font && A.fontDpi == dpi) return;
    HFONT old = A.font;
    A.fontDpi = dpi;
    A.font = CreateFontW(-MulDiv(10, int(dpi), 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                         L"Malgun Gothic");
    for (HWND c : {A.log, A.input, A.sendBtn, A.newBtn, A.keyBtn})
        if (c) SendMessageW(c, WM_SETFONT, WPARAM(A.font), TRUE);
    if (old) DeleteObject(old);
}

void Layout() {
    RECT rc;
    GetClientRect(A.chat, &rc);
    int m = Dip(8), h = Dip(30), bw = Dip(60), kw = Dip(56);
    int W = rc.right, H = rc.bottom;
    MoveWindow(A.log, m, m, W - 2 * m, std::max(10, H - 3 * m - h), TRUE);
    int y = H - m - h;
    int inputW = std::max(40, W - 2 * m - bw - 2 * kw - 3 * Dip(6));
    MoveWindow(A.input, m, y, inputW, h, TRUE);
    int x = m + inputW + Dip(6);
    MoveWindow(A.sendBtn, x, y, bw, h, TRUE);
    x += bw + Dip(6);
    MoveWindow(A.newBtn, x, y, kw, h, TRUE);
    x += kw + Dip(6);
    MoveWindow(A.keyBtn, x, y, kw, h, TRUE);
}

void StartRequest();

void NewChat() {
    A.history = Json::Arr_();
    A.gen++;
    A.busy = false;
    A.rounds = 0;
    if (A.log) SetWindowTextW(A.log, L"");
    Append(L"(새 대화) 우사기한테 뭐든 말해 봐. 예: \"유튜브 열어줘\", \"10분 뒤에 물 마시라고 알려줘\", \"메모장에 장보기 목록 써줘\"");
    A.host.thinking(false);
    UpdateUi();
}

void TrimHistory() {
    // 앞에서부터 버리되, 사용자가 직접 쓴 말로 시작하도록 (도구 호출/응답 짝이 깨지지 않게)
    auto isUserText = [](const Json& c) {
        if (c.StrOr("role") != "user") return false;
        const Json* parts = c.Get("parts");
        return parts && !parts->a.empty() && parts->a[0].Get("text");
    };
    while (A.history.a.size() > kMaxHistory) {
        A.history.a.erase(A.history.a.begin());
        while (!A.history.a.empty() && !isUserText(A.history.a.front())) A.history.a.erase(A.history.a.begin());
    }
}

}  // namespace

namespace Agent {

void Submit(const std::wstring& raw) {
    std::wstring text = raw;
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    if (text.empty()) return;
    if (A.busy) { Append(L"(아직 생각 중이야. 잠깐만!)"); return; }
    std::string key;
    if (!LoadGeminiKey(key)) {
        Append(L"(Gemini API 키가 없어. '키' 버튼으로 먼저 넣어 줘.)");
        KeyDialog();
        if (!LoadGeminiKey(key)) return;
    }
    Append(L"나: " + text);
    Log(L"agent: 사용자 메시지 %zu자", text.size());
    TrimHistory();
    Json c = Json::Obj_();
    c.Set("role", Json::Str_("user"));
    Json parts = Json::Arr_();
    parts.Push(TextPart(ToUtf8(text)));
    c.Set("parts", parts);
    A.history.Push(c);
    A.rounds = 0;
    StartRequest();
}

}  // namespace Agent

namespace {

void StartRequest() {
    std::string key;
    if (!LoadGeminiKey(key)) return;
    Json req = Json::Obj_();
    Json sys = Json::Obj_();
    Json sp = Json::Arr_();
    sp.Push(TextPart(SystemPrompt()));
    sys.Set("parts", sp);
    req.Set("systemInstruction", sys);
    req.Set("contents", A.history);
    Json tools;
    Json::Parse(kTools, tools);
    req.Set("tools", tools);
    std::string body = req.Dump();

    A.busy = true;
    UpdateUi();
    A.host.thinking(true);
    int gen = A.gen;
    HWND target = A.msgWnd;
    std::vector<std::string> models = A.models;
    std::thread([gen, target, models, key, body]() mutable {
        auto r = std::make_unique<AsyncResult>();
        r->gen = gen;
        for (auto& m : models) {
            double t0 = NowSec();
            r->http = HttpsPostJson(L"generativelanguage.googleapis.com", L"/v1beta/models/" + FromUtf8(m) + L":generateContent", key,
                                    body, 60000);
            r->model = m;
            Log(L"agent: %S → HTTP %d (%.1f초)", m.c_str(), r->http.status, NowSec() - t0);
            int st = r->http.status;
            // 과부하·한도·모델 없음·연결 실패면 다음 모델로
            if (!(st == 0 || st == 404 || st == 429 || st == 500 || st == 503 || st == 504)) break;
        }
        SecureZeroMemory(key.data(), key.size());
        if (PostMessageW(target, WM_AGENT_RESULT, 0, LPARAM(r.get()))) r.release();
    }).detach();
}

Json RunTool(const std::string& name, const Json& args);

void OnResult(std::unique_ptr<AsyncResult> r) {
    if (r->gen != A.gen) return;  // '새 대화' 전에 보낸 요청
    A.busy = false;
    A.host.thinking(false);
    auto fail = [&](const std::wstring& why) {
        Log(L"agent: 실패 %s", why.c_str());
        Append(L"(문제가 생겼어: " + why + L")");
        A.host.say(L"하아?", 2);
        // 응답을 받지 못한 마지막 사용자 말은 기록에서 뺀다 (다음 요청이 꼬이지 않게)
        while (!A.history.a.empty() && A.history.a.back().StrOr("role") == "user") A.history.a.pop_back();
        UpdateUi();
    };
    const HttpResult& h = r->http;
    Json d;
    bool parsed = Json::Parse(h.body, d);
    if (h.status != 200) {
        std::wstring why = h.status == 0 ? h.err : L"HTTP " + std::to_wstring(h.status);
        if (parsed)
            if (const Json* e = d.Get("error")) why += L" " + FromUtf8(e->StrOr("message").substr(0, 200));
        if (h.status == 400 || h.status == 401 || h.status == 403) why += L" — API 키를 확인해 줘.";
        return fail(why);
    }
    if (!parsed) return fail(L"응답을 읽지 못함");
    const Json* cands = d.Get("candidates");
    if (!cands || cands->t != Json::Arr || cands->a.empty()) {
        std::string reason;
        if (const Json* pf = d.Get("promptFeedback")) reason = pf->StrOr("blockReason");
        return fail(L"답이 없음 " + FromUtf8(reason));
    }
    const Json& cand = cands->a[0];
    const Json* content = cand.Get("content");
    const Json* parts = content ? content->Get("parts") : nullptr;
    if (!parts || parts->t != Json::Arr || parts->a.empty()) return fail(L"빈 답 (" + FromUtf8(cand.StrOr("finishReason")) + L")");

    A.history.Push(*content);  // thoughtSignature 까지 그대로 돌려줘야 한다
    std::wstring text;
    std::vector<const Json*> calls;
    for (auto& p : parts->a) {
        if (const Json* th = p.Get("thought"); th && th->t == Json::Bool && th->b) continue;
        if (const Json* t = p.Get("text"); t && t->t == Json::Str) text += FromUtf8(t->s);
        if (const Json* fc = p.Get("functionCall")) calls.push_back(fc);
    }
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    if (!text.empty()) {
        Append(L"우사기: " + text);
        A.host.say(text, std::clamp(2.5 + text.size() * 0.07, 3.0, 14.0));
    }
    if (!calls.empty()) {
        if (A.rounds >= kMaxRounds) return fail(L"도구를 너무 많이 써서 멈춤");
        A.rounds++;
        Json resp = Json::Obj_();
        resp.Set("role", Json::Str_("user"));
        Json rparts = Json::Arr_();
        for (const Json* fc : calls) {
            std::string name = fc->StrOr("name");
            Json args = Json::Obj_();
            if (const Json* a = fc->Get("args")) args = *a;
            Json result = RunTool(name, args);
            std::wstring summary = FromUtf8(result.Get("error") ? "실패: " + result.StrOr("error") : result.StrOr("result"));
            if (summary.size() > 120) summary = summary.substr(0, 117) + L"...";
            Append(L"  [" + FromUtf8(name) + L"] " + summary);
            Log(L"agent: 도구 %S → %s", name.c_str(), result.Get("error") ? L"실패" : L"성공");
            Json fr = Json::Obj_();
            fr.Set("name", Json::Str_(name));
            if (const Json* id = fc->Get("id")) fr.Set("id", *id);
            fr.Set("response", result);
            Json part = Json::Obj_();
            part.Set("functionResponse", fr);
            rparts.Push(part);
        }
        resp.Set("parts", rparts);
        A.history.Push(resp);
        StartRequest();
        return;
    }
    if (text.empty()) {
        Append(L"우사기: 이~");
        A.host.say(L"이~", 2);
    }
    UpdateUi();
}

// ---------- 도구 실행 ----------
std::wstring Arg(const Json& a, const char* k) { return FromUtf8(a.StrOr(k)); }

bool Shell(const std::wstring& target, const wchar_t* verb = L"open") {
    return INT_PTR(ShellExecuteW(nullptr, verb, target.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

std::wstring UrlEncode(const std::wstring& w) {
    std::string s = ToUtf8(w), o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
        else if (c == ' ') o += '+';
        else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return FromUtf8(o);
}

void Key(WORD vk) {
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

std::wstring FmtDuration(double sec) {
    long long s = (long long)(sec + 0.5);
    wchar_t b[64];
    if (s >= 3600) swprintf_s(b, L"%lld시간 %lld분", s / 3600, s % 3600 / 60);
    else if (s >= 60) swprintf_s(b, L"%lld분 %lld초", s / 60, s % 60);
    else swprintf_s(b, L"%lld초", s);
    return b;
}

std::map<UINT_PTR, double> g_timerDue;

Json RunTool(const std::string& name, const Json& a) {
    if (name == "open_url") {
        std::wstring u = Arg(a, "url");
        if (u.rfind(L"http://", 0) != 0 && u.rfind(L"https://", 0) != 0) return Err(L"http/https 주소만 열 수 있음");
        return Shell(u) ? Ok(L"열었음: " + u) : Err(L"열지 못함");
    }
    if (name == "search_web") {
        std::wstring q = Arg(a, "query");
        if (q.empty()) return Err(L"검색어가 비어 있음");
        return Shell(L"https://www.google.com/search?q=" + UrlEncode(q)) ? Ok(L"브라우저에 검색 결과를 띄움 (내용은 모름)")
                                                                         : Err(L"브라우저를 열지 못함");
    }
    if (name == "open_app") {
        static const std::map<std::string, const wchar_t*> apps = {
            {"notepad", L"notepad.exe"},     {"calculator", L"calc.exe"},       {"paint", L"mspaint.exe"},
            {"file_explorer", L"explorer.exe"}, {"settings", L"ms-settings:"}, {"task_manager", L"taskmgr.exe"},
            {"screen_snip", L"ms-screenclip:"}, {"browser", L"https://www.google.com/"}};
        auto it = apps.find(a.StrOr("app"));
        if (it == apps.end()) return Err(L"모르는 앱");
        return Shell(it->second) ? Ok(L"실행함") : Err(L"실행하지 못함");
    }
    if (name == "open_folder") {
        static const std::map<std::string, const KNOWNFOLDERID*> f = {
            {"desktop", &FOLDERID_Desktop},   {"documents", &FOLDERID_Documents}, {"downloads", &FOLDERID_Downloads},
            {"pictures", &FOLDERID_Pictures}, {"music", &FOLDERID_Music},         {"videos", &FOLDERID_Videos}};
        auto it = f.find(a.StrOr("folder"));
        if (it == f.end()) return Err(L"모르는 폴더");
        PWSTR p = nullptr;
        std::wstring path;
        if (SUCCEEDED(SHGetKnownFolderPath(*it->second, 0, nullptr, &p))) path = p;
        CoTaskMemFree(p);
        return !path.empty() && Shell(path, L"explore") ? Ok(L"열었음") : Err(L"열지 못함");
    }
    if (name == "set_timer") {
        double sec = a.NumOr("seconds", 0);
        if (!(sec >= 1 && sec <= 86400)) return Err(L"1초~24시간 사이만 가능");
        std::wstring msg = Arg(a, "message");
        if (msg.empty()) msg = L"시간 됐어!";
        UINT_PTR id = A.nextTimer++;
        if (!SetTimer(A.msgWnd, id, UINT(sec * 1000), nullptr)) return Err(L"타이머를 만들지 못함");
        A.timers[id] = msg;
        g_timerDue[id] = NowSec() + sec;
        return Ok(FmtDuration(sec) + L" 뒤에 알려줄게");
    }
    if (name == "list_timers") {
        if (A.timers.empty()) return Ok(L"대기 중인 타이머 없음");
        std::wstring s;
        for (auto& [id, msg] : A.timers) s += msg + L" (" + FmtDuration(std::max(0.0, g_timerDue[id] - NowSec())) + L" 남음)\n";
        return Ok(s);
    }
    if (name == "get_status") {
        std::wstring s = L"지금: " + NowText();
        SYSTEM_POWER_STATUS ps{};
        if (GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryLifePercent <= 100)
            s += L"\n배터리: " + std::to_wstring(ps.BatteryLifePercent) + L"%" + (ps.ACLineStatus == 1 ? L" (충전기 연결)" : L"");
        else s += L"\n배터리: 없음(데스크톱)";
        MEMORYSTATUSEX ms{sizeof(ms)};
        if (GlobalMemoryStatusEx(&ms)) s += L"\n메모리 사용률: " + std::to_wstring(ms.dwMemoryLoad) + L"%";
        s += L"\nPC 켜진 지: " + FmtDuration(GetTickCount64() / 1000.0);
        s += L"\n우사기: " + A.host.status();
        return Ok(s);
    }
    if (name == "list_windows") {
        auto v = A.host.listWindows();
        if (v.empty()) return Ok(L"끌 수 있는 창 없음");
        std::wstring s;
        for (size_t i = 0; i < v.size() && i < 25; ++i) s += v[i] + L"\n";
        return Ok(s);
    }
    if (name == "drag_window") {
        std::wstring r = A.host.dragWindow(Arg(a, "title_contains"));
        return r.rfind(L"오류:", 0) == 0 ? Err(r.substr(3)) : Ok(r);
    }
    if (name == "set_music_mode") return Ok(A.host.setMusicMode(Arg(a, "mode")));
    if (name == "media_control") {
        std::string act = a.StrOr("action");
        int steps = std::clamp(int(a.NumOr("steps", 5)), 1, 25);
        if (act == "play_pause") Key(VK_MEDIA_PLAY_PAUSE);
        else if (act == "next") Key(VK_MEDIA_NEXT_TRACK);
        else if (act == "previous") Key(VK_MEDIA_PREV_TRACK);
        else if (act == "mute") Key(VK_VOLUME_MUTE);
        else if (act == "volume_up") for (int i = 0; i < steps; ++i) Key(VK_VOLUME_UP);
        else if (act == "volume_down") for (int i = 0; i < steps; ++i) Key(VK_VOLUME_DOWN);
        else return Err(L"모르는 동작");
        return Ok(L"보냄 (실제로 반응했는지는 확인할 수 없음)");
    }
    if (name == "write_note") {
        std::wstring t = Arg(a, "text");
        if (t.empty()) return Err(L"내용이 비어 있음");
        if (t.size() > 4000) t.resize(4000);
        std::wstring r = A.host.writeNote(t);
        return r.rfind(L"오류:", 0) == 0 ? Err(r.substr(3)) : Ok(r);
    }
    if (name == "copy_to_clipboard") {
        std::wstring t = Arg(a, "text");
        if (!OpenClipboard(A.msgWnd)) return Err(L"클립보드를 열지 못함");
        EmptyClipboard();
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
        bool ok = false;
        if (hg) {
            memcpy(GlobalLock(hg), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(hg);
            ok = SetClipboardData(CF_UNICODETEXT, hg) != nullptr;
            if (!ok) GlobalFree(hg);
        }
        CloseClipboard();
        return ok ? Ok(L"복사함") : Err(L"복사하지 못함");
    }
    if (name == "read_clipboard") {
        if (!OpenClipboard(A.msgWnd)) return Err(L"클립보드를 열지 못함");
        std::wstring t;
        if (HANDLE hd = GetClipboardData(CF_UNICODETEXT)) {
            if (auto* p = static_cast<const wchar_t*>(GlobalLock(hd))) { t = p; GlobalUnlock(hd); }
        }
        CloseClipboard();
        if (t.empty()) return Ok(L"클립보드에 텍스트 없음");
        if (t.size() > 6000) t = t.substr(0, 6000) + L"...(잘림)";
        return Ok(t);
    }
    if (name == "pet_action") return Ok(A.host.petAction(Arg(a, "action")));
    return Err(L"없는 도구");
}

void FireTimer(UINT_PTR id) {
    KillTimer(A.msgWnd, id);
    auto it = A.timers.find(id);
    if (it == A.timers.end()) return;
    std::wstring msg = it->second;
    A.timers.erase(it);
    g_timerDue.erase(id);
    Log(L"agent: 타이머 울림");
    MessageBeep(MB_ICONASTERISK);
    Append(L"우사기: 띵! " + msg);
    A.host.say(L"띵! " + msg, 10);
}

LRESULT CALLBACK AgentMsgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_AGENT_RESULT) {
        OnResult(std::unique_ptr<AsyncResult>(reinterpret_cast<AsyncResult*>(l)));
        return 0;
    }
    if (m == WM_TIMER && w >= TIMER_BASE) { FireTimer(w); return 0; }
    return DefWindowProcW(h, m, w, l);
}

void SendFromInput() {
    int n = GetWindowTextLengthW(A.input);
    std::wstring t(size_t(n) + 1, L'\0');
    GetWindowTextW(A.input, t.data(), n + 1);
    t.resize(size_t(n));
    if (A.busy) return;
    SetWindowTextW(A.input, L"");
    Agent::Submit(t);
}

LRESULT CALLBACK ChatProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE: {
            A.chat = h;
            DWORD es = WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL;
            A.log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", es, 0, 0, 10, 10, h, HMENU(INT_PTR(IDC_LOG)), A.inst, nullptr);
            A.input = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10,
                                      10, h, HMENU(INT_PTR(IDC_INPUT)), A.inst, nullptr);
            A.sendBtn = CreateWindowExW(0, L"BUTTON", L"보내기", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 10, 10, h,
                                        HMENU(INT_PTR(IDOK)), A.inst, nullptr);
            A.newBtn = CreateWindowExW(0, L"BUTTON", L"새 대화", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, h,
                                       HMENU(INT_PTR(IDC_NEW)), A.inst, nullptr);
            A.keyBtn = CreateWindowExW(0, L"BUTTON", L"키", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, h,
                                       HMENU(INT_PTR(IDC_KEYBTN)), A.inst, nullptr);
            SendMessageW(A.input, EM_SETCUEBANNER, TRUE, LPARAM(L"우사기한테 말하기 (Enter)"));
            SendMessageW(A.input, EM_LIMITTEXT, 2000, 0);
            ApplyFont(GetDpiForWindow(h));
            Layout();
            return 0;
        }
        case WM_SIZE: Layout(); return 0;
        case WM_DPICHANGED: {
            ApplyFont(HIWORD(w));
            auto* r = reinterpret_cast<RECT*>(l);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(w) != WA_INACTIVE) SetFocus(A.input);
            return 0;
        case WM_CTLCOLORSTATIC:
            if (HWND(l) == A.log) {
                SetBkColor(HDC(w), GetSysColor(COLOR_WINDOW));
                return LRESULT(GetSysColorBrush(COLOR_WINDOW));
            }
            break;
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case IDOK: SendFromInput(); return 0;
                case IDC_NEW: NewChat(); SetFocus(A.input); return 0;
                case IDC_KEYBTN: Agent::KeyDialog(); return 0;
                case IDCANCEL: ShowWindow(h, SW_HIDE); return 0;
            }
            break;
        case WM_CLOSE: ShowWindow(h, SW_HIDE); return 0;
        case WM_DESTROY: A.chat = A.log = A.input = A.sendBtn = A.newBtn = A.keyBtn = nullptr; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// 다른 앱이 앞에 있을 때도 대화창을 앞으로 (입력을 받은 스레드에 잠깐 붙어서)
void BringToFront(HWND w) {
    ShowWindow(w, SW_SHOWNORMAL);
    if (GetForegroundWindow() == w) return;
    SetForegroundWindow(w);
    if (GetForegroundWindow() == w) return;
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    if (fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE)) {
        BringWindowToTop(w);
        SetForegroundWindow(w);
        AttachThreadInput(me, fgThread, FALSE);
    }
    if (GetForegroundWindow() != w) Log(L"agent: 대화창을 앞으로 가져오지 못함 (작업 표시줄 깜빡임)");
}

// ---------- API 키 입력 창 ----------
struct KeyDlg {
    HWND wnd{}, edit{};
    bool done = false;
} K;

LRESULT CALLBACK KeyProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_COMMAND:
            if (LOWORD(w) == IDOK) {
                int n = GetWindowTextLengthW(K.edit);
                std::wstring t(size_t(n) + 1, L'\0');
                GetWindowTextW(K.edit, t.data(), n + 1);
                t.resize(size_t(n));
                std::string k = ToUtf8(t);
                bool ok = SaveGeminiKey(k);
                SecureZeroMemory(t.data(), t.size() * sizeof(wchar_t));
                SecureZeroMemory(k.data(), k.size());
                Log(L"agent: API 키 %s", ok ? (n ? L"저장" : L"삭제") : L"저장 실패");
                Append(ok ? (n ? L"(API 키를 저장했어. 이 PC의 이 사용자 계정에서만 풀 수 있게 암호화해 뒀어.)" : L"(API 키를 지웠어.)")
                          : L"(API 키를 저장하지 못했어.)");
                DestroyWindow(h);
                return 0;
            }
            if (LOWORD(w) == IDCANCEL) { DestroyWindow(h); return 0; }
            break;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY: K.done = true; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

namespace Agent {

void Init(HINSTANCE inst, const AgentHost& host, const std::vector<std::string>& models) {
    A.inst = inst;
    A.host = host;
    A.models = models;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = AgentMsgProc;
    wc.hInstance = inst;
    wc.lpszClassName = kAgentMsgClass;
    RegisterClassExW(&wc);
    A.msgWnd = CreateWindowExW(0, kAgentMsgClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);

    WNDCLASSEXW cc{sizeof(cc)};
    cc.lpfnWndProc = ChatProc;
    cc.hInstance = inst;
    cc.lpszClassName = kChatClass;
    cc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    cc.hIcon = HICON(SendMessageW(FindWindowW(L"UsagiDesktopPet.Msg", nullptr), WM_GETICON, ICON_SMALL, 0));
    RegisterClassExW(&cc);
    WNDCLASSEXW kc = cc;
    kc.lpfnWndProc = KeyProc;
    kc.lpszClassName = kKeyClass;
    RegisterClassExW(&kc);
}

HWND ChatWindow() { return A.chat; }

void Open() {
    bool fresh = !A.chat;
    if (fresh) {
        POINT p = A.host.anchor();
        HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        UINT dpi = 96, dy = 96;
        GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dy);
        int w = MulDiv(440, int(dpi), 96), h = MulDiv(360, int(dpi), 96);
        RECT wr = mi.rcWork;
        // 우사기 옆에 (머리 위 말풍선과 겹치지 않게), 공간이 더 넓은 쪽으로
        int gap = MulDiv(170, int(dpi), 96);
        int x = (wr.right - p.x >= p.x - wr.left) ? int(p.x) + gap : int(p.x) - gap - w;
        x = std::clamp(x, int(wr.left), std::max(int(wr.left), int(wr.right) - w));
        int y = std::max(int(wr.top), int(wr.bottom) - h - MulDiv(12, int(dpi), 96));
        CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_CONTROLPARENT, kChatClass, L"우사기와 대화",
                        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, x, y, w, h, nullptr, nullptr, A.inst, nullptr);
        if (!A.chat) return;
        NewChat();
    }
    BringToFront(A.chat);
    SetFocus(A.input);
    UpdateUi();
}

void KeyDialog() {
    if (K.wnd && IsWindow(K.wnd)) { BringToFront(K.wnd); return; }
    UINT dpi = A.chat ? GetDpiForWindow(A.chat) : GetDpiForSystem();
    auto D = [&](int v) { return MulDiv(v, int(dpi), 96); };
    int w = D(460), h = D(190);
    RECT wr;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wr, 0);
    if (A.chat) { RECT cr; GetWindowRect(A.chat, &cr); wr = cr; }
    int x = (wr.left + wr.right - w) / 2, y = (wr.top + wr.bottom - h) / 2;
    K = {};
    K.wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME, kKeyClass, L"Gemini API 키",
                            WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, A.chat, nullptr, A.inst, nullptr);
    if (!K.wnd) return;
    RECT rc;
    GetClientRect(K.wnd, &rc);
    HFONT f = CreateFontW(-MulDiv(10, int(dpi), 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                          L"Malgun Gothic");
    int m = D(12), W = rc.right - 2 * m;
    HWND lbl = CreateWindowExW(0, L"STATIC",
                               L"Google AI Studio 에서 받은 Gemini API 키를 붙여 넣어. 이 PC의 이 사용자 계정으로만 풀 수 있게 "
                               L"암호화해 저장해. 비우고 저장하면 지워져.",
                               WS_CHILD | WS_VISIBLE, m, m, W, D(56), K.wnd, nullptr, A.inst, nullptr);
    K.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, m,
                             m + D(62), W, D(28), K.wnd, nullptr, A.inst, nullptr);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"저장", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, rc.right - m - D(170),
                              rc.bottom - m - D(30), D(80), D(30), K.wnd, HMENU(INT_PTR(IDOK)), A.inst, nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"취소", WS_CHILD | WS_VISIBLE | WS_TABSTOP, rc.right - m - D(80),
                                  rc.bottom - m - D(30), D(80), D(30), K.wnd, HMENU(INT_PTR(IDCANCEL)), A.inst, nullptr);
    for (HWND c : {lbl, K.edit, ok, cancel}) SendMessageW(c, WM_SETFONT, WPARAM(f), TRUE);
    std::string cur;
    SendMessageW(K.edit, EM_SETCUEBANNER, TRUE, LPARAM(LoadGeminiKey(cur) ? L"(저장된 키 있음 — 바꾸려면 새로 입력)" : L"키 붙여 넣기"));
    SecureZeroMemory(cur.data(), cur.size());
    if (A.chat) EnableWindow(A.chat, FALSE);
    BringToFront(K.wnd);
    SetFocus(K.edit);
    // 모달 루프
    MSG msg{};
    while (!K.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(K.wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (msg.message == WM_QUIT) PostQuitMessage(int(msg.wParam));
    K.wnd = nullptr;
    DeleteObject(f);
    if (A.chat) { EnableWindow(A.chat, TRUE); BringToFront(A.chat); }
}

bool PreTranslate(MSG* m) {
    if (A.chat && IsWindowVisible(A.chat) && (m->hwnd == A.chat || IsChild(A.chat, m->hwnd))) return IsDialogMessageW(A.chat, m) != FALSE;
    return false;
}

void Shutdown() {
    A.gen++;
    if (A.chat) DestroyWindow(A.chat);
    if (A.font) DeleteObject(A.font);
}

}  // namespace Agent
