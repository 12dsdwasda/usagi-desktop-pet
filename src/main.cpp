// 우사기 데스크톱 펫 — Win32 layered window 오버레이
#include "common.h"
#include "sprites.h"
#include "audio.h"
#include "wintarget.h"
#include "bubble.h"
#include "voice.h"
#include "agent.h"
#include "gemini.h"
#include "fire.h"
#include "media.h"
#include "lyrics.h"
#include <shellapi.h>
#include <shlobj.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>
#include <random>

#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_REPLAY_CLICK = WM_APP + 2;   // 우사기 위 짧은 클릭을 뒤의 앱으로 다시 보냄
constexpr ULONG_PTR REPLAY_MAGIC = 0x55534743; // 우리가 다시 보낸 클릭 표시
constexpr UINT WM_OPEN_CHAT = WM_APP + 3;     // 우사기를 더블클릭 → 대화창
constexpr int HOTKEY_CHAT = 1;                 // Ctrl+Alt+U
constexpr UINT_PTR TIMER_TICK = 1;
constexpr ULONG_PTR COPYDATA_CMD = 0x55534147;  // 'USAG'
const wchar_t* kMsgClass = L"UsagiDesktopPet.Msg";
const wchar_t* kOverlayClass = L"UsagiDesktopPet.Overlay";

enum MenuId : UINT {
    ID_TOGGLE_VIS = 100, ID_PAUSE, ID_MODE_DANCE, ID_MODE_RAP, ID_MODE_OFF, ID_CANCEL, ID_EXIT, ID_STATUS,
    ID_PRANK_OFF, ID_PRANK_SOMETIMES, ID_PRANK_OFTEN, ID_PRANK_NOW, ID_PRANK_NOTEPAD, ID_BUBBLES,
    ID_VOICE_REC, ID_VOICE_TOGGLE, ID_CURSOR_TOGGLE, ID_CHAT, ID_GEMINI_KEY, ID_ANGRY_TOGGLE, ID_SING_TOGGLE, ID_ROAM_TOGGLE,
    ID_CHAR_BASE = 900,
    ID_TARGET_BASE = 1000,
};

enum class State { Walk, Idle, Music, Approach, Drag, Release, Return, PrankLaunch, PrankWalk, PrankType, CursorChase, CursorDrag, Held, Fall, Angry, AngryThrow };
enum PrankMode { PRANK_OFF, PRANK_SOMETIMES, PRANK_OFTEN };
enum MusicMode { MODE_DANCE, MODE_RAP, MODE_OFF };

const wchar_t* StateName(State s) {
    switch (s) {
        case State::Walk: return L"걷기";
        case State::Idle: return L"쉬기";
        case State::Music: return L"음악";
        case State::Approach: return L"창으로 이동";
        case State::Drag: return L"창 끌기";
        case State::Release: return L"놓기";
        case State::Return: return L"복귀";
        case State::PrankLaunch: return L"메모장 여는 중";
        case State::PrankWalk: return L"메모장으로 이동";
        case State::PrankType: return L"메모장 타이핑";
        case State::CursorChase: return L"포인터 쫓기";
        case State::CursorDrag: return L"포인터 끌기";
        case State::Held: return L"마우스에 잡힘";
        case State::Fall: return L"떨어지는 중";
        case State::Angry: return L"화남";
        case State::AngryThrow: return L"포인터 던지기";
    }
    return L"?";
}

// 고를 수 있는 캐릭터. persona 는 Gemini 에이전트에게 주는 소개·말투.
struct CharInfo { const wchar_t* id; const wchar_t* name; const wchar_t* persona; };
const CharInfo kChars[] = {
    {L"usagi", L"우사기", L"작은 토끼 캐릭터 '우사기'다. 밝고 장난스럽게 말하고, 말버릇(야하!, 우라!, 푸루!, 하아?, 이~)을 가끔 섞어."},
    {L"hachiware", L"하치와레", L"머리 위가 파란 고양이 캐릭터 '하치와레'다. 긍정적이고 다정하며 말을 잘한다. 친구를 응원하듯 말해."},
    {L"chiikawa", L"치이카와", L"작고 하얀 캐릭터 '치이카와'다. 수줍고 겁이 많지만 열심히 한다. 아주 짧게 말하고, 가끔 '와…', '흐에…' 같은 소리를 섞어."},
    {L"rakko", L"랏코", L"이마에 X 흉터가 있는 해달 캐릭터 '랏코'다. 아주 강한 토벌가답게 침착하고 듬직하게, 멋있게 말해."},
    {L"shisa", L"시사", L"사자 갈기가 있는 캐릭터 '시사'다. 성실하고 씩씩하며 열심히 일한다. 기운차게 말해."},
    {L"momonga", L"모몽가", L"파란 꼬리가 복슬복슬한 하늘다람쥐 캐릭터 '모몽가'다. 귀여운 척하며 응석 부리고 조금 제멋대로 말해."},
};
constexpr int kCharCount = int(sizeof(kChars) / sizeof(kChars[0]));

struct Settings {
    std::wstring character = L"usagi";  // kChars 의 id
    double sizeDip = 140;       // 384px 프레임을 화면에 표시할 크기(DIP)
    double walkSpeedDip = 45;   // 걷기 속도 (DIP/초)
    double dragSpeedDip = 70;   // 끌기 속도
    double dragMinDip = 150, dragMaxDip = 250;
    double gripOffsetDip = 15;  // 창 위쪽에서 손 위치까지 (제목 표시줄 안)
    bool hideOnFullscreen = true;
    MusicMode mode = MODE_DANCE;
    AudioParams audio;
    PrankMode prank = PRANK_SOMETIMES;
    bool prankNotepad = true;      // 메모장 놀리기 허용
    double notepadIdleSec = 45;    // 사용자가 이만큼 아무 입력도 없을 때만 메모장 장난
    double sleepIdleSec = 180;     // 이만큼 입력이 없으면 졸기
    bool bubbles = true;
    bool voice = true;             // 녹음한 목소리 재생
    bool cursorGrab = true;        // 마우스 포인터 가져가기
    double cursorIntervalSec = 10; // 포인터를 가져가는 간격(초, 지난번이 끝난 뒤부터)
    bool grabbable = true;         // 우사기를 꾹 눌러 잡고 옮기기
    bool roam = true;              // 화면 어디든 돌아다니고, 놓은 자리에 안착 (false = 바닥만 걷고 놓으면 떨어짐)
    std::wstring agentModels = L"gemini-3-flash-preview,gemini-flash-latest,gemini-3.5-flash";
    bool chatHotkey = true;        // Ctrl+Alt+U 로 대화창
    bool angry = true;             // 여러 번 때리면 화내고 포인터를 던짐
    int angryHits = 3;             // 화내기까지 빠르게 때리는 횟수
    double blockSec = 5;           // 던진 뒤 마우스·키보드가 안 먹는 시간 (최대 10초)
    bool sing = true;              // 스포티파이 곡 가사를 말풍선으로 따라 부르기
    bool singSpotifyOnly = true;   // false 면 다른 미디어 앱(브라우저 등)도
    double lyricsOffsetSec = 0;    // 가사 타이밍 보정 (+면 늦게)
};

struct MonInfo {
    RECT work{}, mon{};
    double scale = 1;
    HMONITOR h = nullptr;
};

MonInfo MonFrom(HMONITOR m) {
    MonInfo r;
    r.h = m;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(m, &mi);
    r.work = mi.rcWork;
    r.mon = mi.rcMonitor;
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    r.scale = dx / 96.0;
    return r;
}
MonInfo MonAt(double x, double y) { return MonFrom(MonitorFromPoint(POINT{LONG(std::lround(x)), LONG(std::lround(y))}, MONITOR_DEFAULTTONEAREST)); }

struct App {
    HINSTANCE inst{};
    HWND overlay{}, msg{};
    HICON icon{};
    UINT taskbarCreated = 0;
    bool trayAdded = false;
    Settings cfg;
    SpriteSet sprites;
    ScaledCache cache;
    AudioMonitor audio;
    std::mt19937 rng{std::random_device{}()};

    State state = State::Walk;
    double px = 0, py = 0;  // 몸통 중심-발바닥 기준점 (물리 픽셀, 가상 화면 좌표)
    int facing = 1;         // +1 오른쪽, -1 왼쪽
    bool visible = true, autoHidden = false, paused = false;
    double stateUntil = 0;
    MonInfo mon;
    int sizePx = 140;
    double s = 140.0 / 384;  // 프레임 px -> 화면 px

    // 애니메이션
    int anim = ANIM_WALK, frame = 0;
    double frameT = 0;
    bool beatMode = false;

    // 끌기
    HWND target{};
    int side = 1;  // +1: 창 오른쪽 가장자리를 잡고 오른쪽으로, -1: 왼쪽
    RECT startWin{};
    double dragDist = 0, dragMoved = 0, phaseStart = 0;
    int lastSetX = 0, prevSetX = 0, mismatch = 0;
    std::vector<TargetWindow> menuTargets;

    // 렌더링
    HDC memDC{};
    HBITMAP dib{}, oldBmp{};
    void* bits = nullptr;
    int dibSize = 0;
    struct { int anim = -1, frame = -1, size = -1; bool flip = false; LONG x = LONG_MIN, y = LONG_MIN; int tint = 0; } drawn;

    double lastTick = 0, lastSlow = 0, lastLog = 0, lastTopmost = 0;
    AudioSnapshot snd;

    // 말풍선·장난
    Bubble bubble;
    VoiceBank voices;
    double nextCursor = 0, cursorDist = 0, cursorMoved = 0;
    POINT cursorStart{}, cursorSet{}, cursorSeen{};
    double cursorStillSince = 0;
    // 마우스로 우사기 잡기
    HHOOK mouseHook{};
    int press = 0;                 // 0 없음, 1 눌렀음(아직 클릭인지 모름), 2 잡는 중
    POINT pressPt{}, hookPt{};
    double pressT = 0, lastHookEvent = 0, grabOffX = 0, grabOffY = 0, vx = 0, vy = 0;
    double heldSideUntil = 0, heldT = 0;
    double roamX = 0, roamY = 0;  // 어디든 돌아다니기: 지금 걸어가는 목표점  // 들린 모습: 옆으로 끌려가는 포즈 유지 시각, 흔들림 시간
    struct Sample { double t, x, y; };
    std::vector<Sample> trail;     // 잡고 있는 동안 포인터 궤적 (던지는 속도 계산)
    int bounces = 0;
    bool thrown = false;
    double lastPetClick = -10;     // 더블클릭 감지
    POINT lastPetClickPt{};
    bool npFast = false;           // 에이전트가 시킨 메모: 빠르게 타이핑
    // 때리기 → 화내기 → 포인터 던지기
    int hitCount = 0;
    double chatPendingAt = 0;      // 더블클릭 후 세 번째 클릭이 없으면 대화창
    bool angryPending = false, ouchPending = false;
    int tint = 0;                  // 빨갛게 (0~4)
    double shakeX = 0;             // 부들부들 (화면 px)
    double angryT0 = 0, blockUntil = 0;
    POINT throwFrom{}, throwTo{};
    bool laughed = false;
    long blocked = 0;              // 막은 입력 수 (로그)
    HHOOK keyHook{};
    Fire fire;
    // 스포티파이 따라 부르기
    MediaWatcher media;
    LyricsFetcher lyrics;
    MediaSnapshot song;
    std::shared_ptr<const LyricsEntry> songLyrics;
    std::wstring songKey;
    int lyricIdx = -1;
    bool singing = false;
    bool autoDrag = false, saidUraura = false, sleeping = false, wasPlaying = false;
    double nextPrank = 0, lastSnore = 0, lastFood = -1000;
    HWND lastFoodWnd{};
    HWND npWnd{}, npEdit{}, npPrevFg{};
    std::vector<HWND> npBefore;
    std::wstring npText;
    size_t npPos = 0;
    double npNextChar = 0;
    DWORD prankInputTick = 0;
    bool prankIgnoreInput = false;  // 테스트용: 사용자 입력이 있어도 계속
} g;

double Rand(double a, double b) { return std::uniform_real_distribution<double>(a, b)(g.rng); }

// 말풍선 (같은 말을 연달아 하지 않음)
void PlayedVoice(double dur) { g.audio.SuppressUntil(NowSec() + dur + 0.35); }

void Say(const wchar_t* text, double seconds = 1.8) {
    if (!text) return;
    Log(L"say: %s", text);
    if (g.cfg.bubbles) g.bubble.Say(text, seconds);
    // 캐릭터가 보일 때만 녹음한 목소리를 튼다 (숨김·전체 화면 중에는 조용히)
    if (g.cfg.voice && g.visible && !g.autoHidden) {
        double d = g.voices.Play(text, NowSec());
        if (d > 0) PlayedVoice(d);
    }
}

DWORD LastInputTick() {
    LASTINPUTINFO li{sizeof(li)};
    GetLastInputInfo(&li);
    return li.dwTime;
}
double UserIdleSec() { return (GetTickCount() - LastInputTick()) / 1000.0; }

void ScheduleNextPrank(double now) {
    if (g.cfg.prank == PRANK_OFTEN) g.nextPrank = now + Rand(60, 180);
    else g.nextPrank = now + Rand(300, 600);
}

const CharInfo& CurChar() {
    for (const CharInfo& c : kChars)
        if (g.cfg.character == c.id) return c;
    return kChars[0];
}

// ---------- 기하 ----------
bool Flipped() { return g.anim == ANIM_DRAG ? g.side < 0 : g.facing < 0; }

// 기준점 기준 손(drag 스프라이트)의 화면 오프셋
POINT HandOffset(int side, double s) {
    const AnimMeta& m = g.sprites.meta[ANIM_DRAG];
    double hx = (g.sprites.gripX - m.anchorX) * s;  // 원본: 손이 몸 왼쪽 (음수)
    double hy = (g.sprites.gripY - g.sprites.baseY) * s;
    return POINT{LONG(std::lround(side > 0 ? hx : -hx)), LONG(std::lround(hy))};
}

// 현재 애니메이션 기준 기준점에서 좌우로 차지하는 폭
void Extents(int anim, bool flip, double s, double& leftExt, double& rightExt) {
    const AnimMeta& m = g.sprites.meta[anim];
    double l = (m.anchorX - m.left) * s, r = (m.right - m.anchorX) * s;
    leftExt = flip ? r : l;
    rightExt = flip ? l : r;
}

double Floor(const MonInfo& m) { return double(m.work.bottom - 1); }

void UpdateScaleFor(double x, double y) {
    g.mon = MonAt(x, y);
    g.sizePx = std::max(32, int(std::lround(g.cfg.sizeDip * g.mon.scale)));
    g.s = double(g.sizePx) / g.sprites.frameW;
}

void SetState(State st, const wchar_t* why = L"") {
    if (st == g.state) return;
    Log(L"state: %s -> %s %s", StateName(g.state), StateName(st), why);
    g.state = st;
    g.phaseStart = NowSec();
    g.frameT = 0;
    g.beatMode = false;
}

// ---------- 애니메이션 ----------
void SetAnim(int a) {
    if (g.anim == a) return;
    g.anim = a;
    g.frame = 0;
    g.frameT = 0;
}

void AdvanceDefault(double dt) {
    const auto& d = g.sprites.meta[g.anim].durationsMs;
    g.frameT += dt * 1000.0;
    int guard = 0;
    while (g.frameT >= d[g.frame] && guard++ < 8) {
        g.frameT -= d[g.frame];
        g.frame = (g.frame + 1) % int(d.size());
    }
}

// 박자가 잡히면 주요 포즈(0,2번 프레임)를 박자에 맞춘다. 아니면 기본 속도로 반복.
void AdvanceMusic(double dt, double now) {
    if (g.snd.beatLocked && g.sprites.frames[g.anim].size() == 4) {
        double P = g.snd.period;
        while (P < 0.4) P *= 2;
        while (P >= 0.8) P /= 2;
        double b = (now - g.snd.beatTime) / P;
        double n = std::floor(b), ph = b - n;
        int pose = (long long(n) % 2 == 0) ? 0 : 2;
        g.frame = ph < 0.55 ? pose : pose + 1;
        g.frameT = 0;
        g.beatMode = true;
    } else {
        g.beatMode = false;
        AdvanceDefault(dt);
    }
}

// ---------- 렌더링 ----------
void EnsureDib(int size) {
    if (g.dib && g.dibSize == size) return;
    if (!g.memDC) g.memDC = CreateCompatibleDC(nullptr);
    if (g.dib) { SelectObject(g.memDC, g.oldBmp); DeleteObject(g.dib); }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    g.dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &g.bits, nullptr, 0);
    g.oldBmp = HBITMAP(SelectObject(g.memDC, g.dib));
    g.dibSize = size;
    g.drawn.size = -1;
}

void Render() {
    bool show = g.visible && !g.autoHidden;
    if (!show) {
        if (IsWindowVisible(g.overlay)) ShowWindow(g.overlay, SW_HIDE);
        return;
    }
    bool flip = Flipped();
    const AnimMeta& m = g.sprites.meta[g.anim];
    double ax = flip ? (g.sprites.frameW - m.anchorX) : m.anchorX;
    LONG x = LONG(std::lround(g.px + g.shakeX - ax * g.s));
    LONG y = LONG(std::lround(g.py - g.sprites.baseY * g.s));
    bool content = g.drawn.anim != g.anim || g.drawn.frame != g.frame || g.drawn.flip != flip || g.drawn.size != g.sizePx ||
                   g.drawn.tint != g.tint;
    if (!content && g.drawn.x == x && g.drawn.y == y && IsWindowVisible(g.overlay)) return;
    EnsureDib(g.sizePx);
    if (content || g.drawn.size != g.sizePx) {
        const Bitmap& b = g.cache.Get(g.sprites, g.anim, g.frame, flip, g.sizePx);
        memcpy(g.bits, b.px.data(), b.px.size() * 4);
        if (g.tint > 0) {  // 화나서 빨개짐 (premultiplied BGRA)
            float f = g.tint / 4.0f * 0.6f;
            auto* px = static_cast<uint32_t*>(g.bits);
            for (size_t i = 0, n = b.px.size(); i < n; ++i) {
                uint32_t v = px[i], A = v >> 24;
                if (!A) continue;
                uint32_t R = (v >> 16) & 255, G = (v >> 8) & 255, B = v & 255;
                R = std::min<uint32_t>(A, R + uint32_t((A - std::min(A, R)) * f * 0.6f));
                G = uint32_t(G * (1 - f * 0.85f));
                B = uint32_t(B * (1 - f));
                px[i] = (A << 24) | (R << 16) | (G << 8) | B;
            }
        }
    }
    POINT dst{x, y}, src{0, 0};
    SIZE sz{g.sizePx, g.sizePx};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g.overlay, nullptr, &dst, &sz, g.memDC, &src, 0, &bf, ULW_ALPHA);
    if (!IsWindowVisible(g.overlay)) ShowWindow(g.overlay, SW_SHOWNOACTIVATE);
    g.drawn = {g.anim, g.frame, g.sizePx, flip, x, y, g.tint};
}

// ---------- 걷기 ----------
void StartIdle(double now) {
    SetState(State::Idle);
    g.stateUntil = now + Rand(1.5, 4.0);
}
void PickRoamTarget() {
    const MonInfo& m = g.mon;
    double le, re;
    Extents(ANIM_WALK, false, g.s, le, re);
    double bodyH = (g.sprites.baseY - g.sprites.meta[ANIM_WALK].top) * g.s;
    double x0 = m.work.left + std::max(le, re), x1 = m.work.right - std::max(le, re);
    double y0 = m.work.top + bodyH, y1 = Floor(m);
    // 지금 자리에서 120~500 DIP 떨어진 아무 곳. 가끔은 바닥(작업 표시줄 위)으로
    double r = Rand(120, 500) * m.scale, a = Rand(0, 6.283185);
    g.roamX = std::clamp(g.px + std::cos(a) * r, std::min(x0, x1), std::max(x0, x1));
    g.roamY = Rand(0, 1) < 0.2 ? y1 : std::clamp(g.py + std::sin(a) * r, std::min(y0, y1), y1);
}

void StartWalk(double now) {
    SetState(State::Walk);
    g.stateUntil = now + Rand(4.0, 12.0);
    if (Rand(0, 1) < 0.3) g.facing = -g.facing;
    if (g.cfg.roam) { PickRoamTarget(); g.stateUntil = now + 25; }
}

// 기준점을 모니터 작업 영역 바닥으로 맞추고 좌우를 안쪽으로 제한. 가장자리면 true.
bool ClampToFloor() {
    double le, re;
    Extents(g.anim, Flipped(), g.s, le, re);
    if (g.cfg.roam) {  // 바닥에 붙이지 않고 작업 영역 안으로만
        double bodyH = (g.sprites.baseY - g.sprites.meta[g.anim].top) * g.s;
        double top = g.mon.work.top + bodyH, bot = Floor(g.mon);
        g.py = std::clamp(g.py, std::min(top, bot), bot);
    } else {
        g.py = Floor(g.mon);
    }
    double lo = g.mon.work.left + le, hi = g.mon.work.right - re;
    if (hi < lo) { g.px = (g.mon.work.left + g.mon.work.right) / 2.0; return false; }
    if (g.px <= lo) { g.px = lo; return true; }
    if (g.px >= hi) { g.px = hi; return true; }
    return false;
}

// 2D 걷기로 목표점까지 이동. 도착하면 true.
bool WalkToward(double tx, double ty, double speedPx, double dt) {
    double dx = tx - g.px, dy = ty - g.py, d = std::hypot(dx, dy);
    if (std::abs(dx) > 2) g.facing = dx > 0 ? 1 : -1;
    double step = speedPx * dt;
    if (d <= std::max(step, 1.5)) { g.px = tx; g.py = ty; return true; }
    g.px += dx / d * step;
    g.py += dy / d * step;
    return false;
}

// ---------- 끌기 ----------
void Release(const wchar_t* why, const wchar_t* say = L"하아?") {
    if (g.state != State::Approach && g.state != State::Drag) return;
    Log(L"drag: 놓기 (%s), 이동 %d px", why, int(g.dragMoved));
    Say(say);
    g.target = nullptr;
    SetState(State::Release, why);
    g.stateUntil = NowSec() + 0.6;
}

void BeginApproach(HWND h, bool automatic = false) {
    std::wstring why;
    if (!IsMovableWindow(h, &why)) {
        Log(L"drag: 대상 제외 (%s)", why.c_str());
        return;
    }
    if (g.state == State::Approach || g.state == State::Drag) Release(L"새 대상");
    g.target = h;
    RECT f = VisibleFrame(h);
    MonInfo wm = MonFrom(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST));
    // 공간이 더 많은 쪽 가장자리를 잡는다
    double roomR = wm.work.right - f.right, roomL = f.left - wm.work.left;
    g.side = roomR >= roomL ? 1 : -1;
    g.dragMoved = 0;
    g.visible = true;
    g.autoDrag = automatic;
    g.saidUraura = false;
    g.sleeping = false;
    Say(automatic ? L"야하!" : L"푸루!");  // 스스로 찾았을 때 / 시켜서 "알겠어"
    wchar_t t[200] = {};
    GetWindowTextW(h, t, 200);
    Log(L"drag: 대상 '%s' frame=(%d,%d,%d,%d) side=%d", t, f.left, f.top, f.right, f.bottom, g.side);
    SetState(State::Approach);
}

// 손이 닿아야 하는 점 → 기준점
void GripTarget(const RECT& f, double& tx, double& ty) {
    double gx = g.side > 0 ? f.right - 1 : f.left;
    double gy = f.top + g.cfg.gripOffsetDip * g.mon.scale;
    POINT ho = HandOffset(g.side, g.s);
    tx = gx - ho.x;
    ty = gy - ho.y;
}

void StartDrag(double now) {
    GetWindowRect(g.target, &g.startWin);
    RECT f = VisibleFrame(g.target);
    MonInfo wm = MonFrom(MonitorFromWindow(g.target, MONITOR_DEFAULTTONEAREST));
    // 캐릭터가 화면 안에 남도록 이동 거리 제한 (손 바깥쪽 몸 폭)
    const AnimMeta& m = g.sprites.meta[ANIM_DRAG];
    double beyond = (m.right - g.sprites.gripX) * g.s;
    double room = g.side > 0 ? wm.work.right - f.right - beyond : f.left - wm.work.left - beyond;
    double want = Rand(g.cfg.dragMinDip, g.cfg.dragMaxDip) * wm.scale;
    g.dragDist = std::max(0.0, std::min(want, room));
    if (g.dragDist < 12 * wm.scale) g.dragDist = 0;  // 공간이 없으면 제자리에서 당기기만
    g.dragMoved = 0;
    g.lastSetX = g.prevSetX = g.startWin.left;
    g.mismatch = 0;
    Log(L"drag: 잡음, 목표 %d px (원하는 %d, 여유 %d)", int(g.dragDist), int(want), int(room));
    Say(L"우라!");
    SetState(State::Drag);
    (void)now;
}

void TickDrag(double now, double dt) {
    HWND h = g.target;
    if (!IsWindow(h)) return Release(L"창 닫힘");
    if (IsIconic(h) || IsZoomed(h) || !IsWindowVisible(h)) return Release(L"창 상태 변경");
    if (IsInMoveSize(h)) return Release(L"사용자가 직접 이동", L"하아? 하아?");
    if (IsHungAppWindow(h)) return Release(L"응답 없음");
    RECT cur;
    GetWindowRect(h, &cur);
    bool sizeChanged = (cur.right - cur.left) != (g.startWin.right - g.startWin.left) ||
                       (cur.bottom - cur.top) != (g.startWin.bottom - g.startWin.top);
    bool posOk = (std::abs(cur.left - g.lastSetX) <= 1 || std::abs(cur.left - g.prevSetX) <= 1) &&
                 std::abs(cur.top - g.startWin.top) <= 1;
    if (sizeChanged) return Release(L"창 크기 변경");
    if (!posOk) {
        if (++g.mismatch >= 3) return Release(L"창이 다른 곳으로 이동/이동 실패");
    } else {
        g.mismatch = 0;
    }
    double el = now - g.phaseStart;
    if (g.dragDist <= 0) {
        if (el > 1.4) return Release(L"공간 부족", L"하아?");
    } else {
        // 프레임의 힘주는 포즈(0,2)에서 조금 더 빠르게 → 당기는 느낌
        double ramp = std::min(1.0, el / 0.4);
        double pulse = (g.frame % 2 == 0) ? 1.25 : 0.7;
        double v = g.cfg.dragSpeedDip * g.mon.scale * ramp * pulse;
        g.dragMoved = std::min(g.dragDist, g.dragMoved + v * dt);
        int nx = g.startWin.left + g.side * int(std::lround(g.dragMoved));
        if (nx != g.lastSetX) {
            // 비동기: 대상 앱이 느려도 펫이 멈추지 않게. 크기/Z순서/활성 상태 유지.
            if (!SetWindowPos(h, nullptr, nx, g.startWin.top, 0, 0,
                              SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS))
                return Release(L"SetWindowPos 실패");
            g.prevSetX = g.lastSetX;
            g.lastSetX = nx;
        }
        if (!g.saidUraura && el > 1.3) { g.saidUraura = true; Say(L"우라우라"); }
        if (g.dragMoved >= g.dragDist && std::abs(cur.left - nx) <= 1) return Release(L"완료", L"이~");
    }
    // 손을 실제 창 가장자리에 붙인다 (실제 위치 기준이라 지연이 있어도 벌어지지 않음)
    RECT f = VisibleFrame(h);
    GripTarget(f, g.px, g.py);
}

// ---------- 장난: 메모장 놀리기 ----------
// 사용자 문서를 건드리지 않도록, 열린 메모장이 하나도 없을 때만 새 메모장을 열어 타이핑한다.
std::vector<HWND> NotepadWindows() {
    std::vector<HWND> v;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        wchar_t c[64] = {};
        GetClassNameW(h, c, 64);
        if (!wcscmp(c, L"Notepad")) reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
        return TRUE;
    }, LPARAM(&v));
    return v;
}

HWND FindNotepadEdit(HWND top) {
    HWND found = nullptr;
    EnumChildWindows(top, [](HWND h, LPARAM lp) -> BOOL {
        wchar_t c[64] = {};
        GetClassNameW(h, c, 64);
        if (!wcscmp(c, L"RichEditD2DPT") || !wcscmp(c, L"Edit") || !wcsncmp(c, L"RICHEDIT", 8)) {
            *reinterpret_cast<HWND*>(lp) = h;
            return FALSE;
        }
        return TRUE;
    }, LPARAM(&found));
    return found;
}

const wchar_t* kTeases[] = {
    L"야하! 여기 메모장 발견!\r하아? 아직도 일하는 거야?\r우라우라~ 잠깐 쉬어도 돼! 푸루야~",
    L"이~ 이거 봐봐! 우사기가 쓴 거야\r구우구우~ 졸리면 자도 돼\r미야야야~ 얼른 돌아와!",
    L"우라! 오늘의 할 일: 우사기랑 놀기\r쥬르리... 간식 먹을 시간 아니야?\r푸루! 힘내라~",
    L"하아? 하아? 컴퓨터만 보고 있었지?\r우라우라 스트레칭 한 번!\r이~ 우사기 다녀감",
};

bool StartNotepadPrank(const std::wstring* text = nullptr) {
    if (!NotepadWindows().empty()) { Log(L"prank: 이미 열린 메모장이 있어 메모장 장난은 건너뜀"); return false; }
    g.npBefore.clear();
    g.npPrevFg = GetForegroundWindow();
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION pi{};
    wchar_t cmdline[] = L"notepad.exe";
    if (!CreateProcessW(nullptr, cmdline, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        Log(L"prank: 메모장 실행 실패 %lu", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g.npWnd = g.npEdit = nullptr;
    g.npFast = text != nullptr;
    g.npText = text ? *text : kTeases[std::uniform_int_distribution<int>(0, int(std::size(kTeases)) - 1)(g.rng)];
    g.npPos = 0;
    g.prankInputTick = LastInputTick();
    g.sleeping = false;
    Log(L"prank: 메모장 놀리기 시작");
    SetState(State::PrankLaunch);
    return true;
}

void EndPrank(const wchar_t* why, const wchar_t* say) {
    Log(L"prank: 끝 (%s)", why);
    Say(say);
    g.npWnd = g.npEdit = nullptr;
    g.prankIgnoreInput = false;
    SetState(State::Return, why);
}

void TickPrank(double now, double dt) {
    // 사용자가 돌아와 입력하면 즉시 멈춘다 (입력 방해 금지)
    if (g.state != State::PrankLaunch && !g.prankIgnoreInput && LastInputTick() != g.prankInputTick) return EndPrank(L"사용자 입력", L"하아?");
    if (g.state == State::PrankLaunch) {
        SetAnim(ANIM_WALK);
        g.frame = 1;
        if (now - g.phaseStart > 8) return EndPrank(L"메모장 창을 찾지 못함", L"하아?");
        if (!g.npWnd) {
            for (HWND h : NotepadWindows())
                if (IsWindowVisible(h)) g.npWnd = h;
            if (!g.npWnd) return;
            // 우사기 근처, 바닥 위에 적당한 크기로 놓기 (활성화하지 않음)
            double sc = g.mon.scale;
            int w = int(560 * sc), h = int(320 * sc);
            int x = std::clamp(int(g.px - 120 * sc), int(g.mon.work.left), int(g.mon.work.right) - w);
            int y = int(g.mon.work.bottom - h - 70 * sc);
            SetWindowPos(g.npWnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
            if (GetForegroundWindow() == g.npWnd && g.npPrevFg && IsWindow(g.npPrevFg)) SetForegroundWindow(g.npPrevFg);
            Log(L"prank: 새 메모장 %p", g.npWnd);
        }
        g.npEdit = FindNotepadEdit(g.npWnd);
        if (!g.npEdit) return;  // 편집 영역이 생길 때까지 대기
        g.prankInputTick = LastInputTick();
        Say(L"아하!");
        SetState(State::PrankWalk);
        return;
    }
    if (!IsWindow(g.npWnd) || IsIconic(g.npWnd)) return EndPrank(L"메모장 닫힘", L"하아?");
    RECT f = VisibleFrame(g.npWnd);
    double sc = g.mon.scale;
    if (g.state == State::PrankWalk) {
        SetAnim(ANIM_WALK);
        AdvanceDefault(dt * 1.4);
        if (now - g.phaseStart > 20) return EndPrank(L"접근 시간 초과", L"하아?");
        // 메모장 아래쪽 안에 서서 타이핑
        if (WalkToward(f.left + 110 * sc, f.bottom - 14 * sc, g.cfg.walkSpeedDip * 2.2 * sc, dt)) {
            Say(L"우라!");
            g.npNextChar = now + 0.4;
            SetState(State::PrankType);
        }
        return;
    }
    // PrankType: 팔을 앞으로 모은 춤 프레임(1,3)을 번갈아 → 타닥타닥
    SetAnim(ANIM_DANCE);
    g.frame = (int(now / 0.14) % 2) ? 1 : 3;
    g.px = f.left + 110 * sc;
    g.py = f.bottom - 14 * sc;
    if (now < g.npNextChar) return;
    if (g.npPos >= g.npText.size()) return EndPrank(L"타이핑 완료", L"이~");
    wchar_t ch = g.npText[g.npPos++];
    PostMessageW(g.npEdit, WM_CHAR, ch, 1);
    if (g.npFast) {
        g.npNextChar = now + (ch == L'\r' ? 0.2 : Rand(0.02, 0.05));
        return;
    }
    g.npNextChar = now + (ch == L'\r' ? 0.6 : Rand(0.08, 0.2));
    if (ch == L'\r' && Rand(0, 1) < 0.5) Say(Rand(0, 1) < 0.5 ? L"우라우라" : L"푸루야~", 1.2);
}

// 가끔 저절로 장난치기. force: 트레이 '지금 장난치기'
void MaybePrank(double now, bool force) {
    if (!force && (g.cfg.prank == PRANK_OFF || now < g.nextPrank)) return;
    ScheduleNextPrank(now);
    bool canNotepad = g.cfg.prankNotepad && (force || UserIdleSec() >= g.cfg.notepadIdleSec) && NotepadWindows().empty();
    if (canNotepad && Rand(0, 1) < 0.5 && StartNotepadPrank()) return;
    // 지금 쓰고 있는 앞 창은 건드리지 않는다
    HWND fg = GetForegroundWindow();
    HWND fgRoot = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    std::vector<TargetWindow> c;
    for (auto& w : ListMovableWindows(g.overlay, g.msg))
        if (w.hwnd != fg && w.hwnd != fgRoot) c.push_back(w);
    if (c.empty()) {
        Log(L"prank: 끌 수 있는 창이 없음");
        if (canNotepad) StartNotepadPrank();
        return;
    }
    auto& t = c[std::uniform_int_distribution<size_t>(0, c.size() - 1)(g.rng)];
    Log(L"prank: 자동 끌기 '%s'", t.title.c_str());
    BeginApproach(t.hwnd, true);
}

// ---------- 장난: 마우스 포인터 가져가기 ----------
bool MouseButtonDown() {
    return (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON) |
            GetAsyncKeyState(VK_XBUTTON1) | GetAsyncKeyState(VK_XBUTTON2)) & 0x8000;
}

// 지금 포인터를 가져가도 사용자 작업을 망치지 않는가
bool CursorFree(const wchar_t** why) {
    auto no = [&](const wchar_t* w) { if (why) *why = w; return false; };
    if (MouseButtonDown()) return no(L"마우스 버튼 누르는 중");
    GUITHREADINFO gi{sizeof(gi)};
    if (GetGUIThreadInfo(0, &gi)) {
        if (gi.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE | GUI_INMOVESIZE)) return no(L"메뉴/창 이동 중");
        if (gi.hwndCapture) return no(L"앱이 마우스를 잡고 있음");
    }
    CURSORINFO ci{sizeof(ci)};
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING)) return no(L"포인터 숨김 (게임·입력 중)");
    RECT clip;
    GetClipCursor(&clip);
    RECT vs{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    vs.right = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vs.bottom = vs.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (clip.left > vs.left || clip.top > vs.top || clip.right < vs.right || clip.bottom < vs.bottom) return no(L"포인터가 한 영역에 갇혀 있음");
    return true;
}

void EndCursor(const wchar_t* why, const wchar_t* say) {
    Log(L"cursor: 놓기 (%s), 이동 %d px", why, int(g.cursorMoved));
    Say(say);
    g.nextCursor = NowSec() + g.cfg.cursorIntervalSec;
    SetState(State::Release, why);
    g.stateUntil = NowSec() + 0.5;
}

// 포인터가 우사기 몸 근처(약 60 DIP 안)에 있는가
bool CursorNearPet(POINT c) {
    const AnimMeta& m = g.sprites.meta[g.anim];
    double le, re;
    Extents(g.anim, Flipped(), g.s, le, re);
    double margin = 60 * g.mon.scale;
    double top = g.py - (g.sprites.baseY - m.top) * g.s;
    return c.x >= g.px - le - margin && c.x <= g.px + re + margin && c.y >= top - margin && c.y <= g.py + margin;
}

// nearOnly: 춤추는 중처럼 정해진 주기는 쉬고, 가까이 온 포인터만 잡을 때
void MaybeGrabCursor(double now, bool force, bool nearOnly = false) {
    if (!force && !g.cfg.cursorGrab) return;
    if (!g.visible || g.autoHidden) return;
    POINT c;
    GetCursorPos(&c);
    bool isNear = CursorNearPet(c);
    if (!force) {
        if (nearOnly && !isNear) return;
        // 가까우면 쿨타임 없이. 단 어디든 돌아다니기 중엔 화면 한가운데 있을 때가 많아서 항상 쿨타임을 지킨다
        if ((!isNear || g.cfg.roam) && now < g.nextCursor) return;
        // 마우스를 쓰는 중이면 잠깐 멈출 때까지 기다림 (가까우면 더 짧게)
        if (now - g.cursorStillSince < (isNear ? 0.25 : 0.5)) return;
    }
    const wchar_t* why = L"";
    if (!CursorFree(&why)) {
        if (!isNear) g.nextCursor = now + 2;  // 조금 뒤에 다시 살펴봄
        if (force) Log(L"cursor: 지금은 안 됨 (%s)", why);
        return;
    }
    if (isNear) Log(L"cursor: 가까이 있어서 바로 잡으러 감");
    MonInfo cm = MonFrom(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST));
    g.side = (cm.work.right - c.x) >= (c.x - cm.work.left) ? 1 : -1;  // 여유 많은 쪽으로 끌고 감
    g.cursorMoved = 0;
    g.sleeping = false;
    Log(L"cursor: 쫓기 시작 (%d,%d)", c.x, c.y);
    Say(L"야하!");
    SetState(State::CursorChase);
}

void TickCursor(double now, double dt) {
    if (!g.cfg.cursorGrab && g.state == State::CursorChase) return EndCursor(L"기능 꺼짐", nullptr);
    if (!g.visible || g.autoHidden) return EndCursor(L"캐릭터 숨김", nullptr);
    const wchar_t* why = L"";
    bool free = CursorFree(&why);
    // 끄는 중에 버튼을 누르면 바로 놓는다 (드래그·클릭이 엉뚱한 곳에 떨어지지 않게)
    if (g.state == State::CursorDrag && !free) return EndCursor(why, L"하아?");
    POINT c;
    GetCursorPos(&c);
    POINT ho = HandOffset(g.side, g.s);
    if (g.state == State::CursorChase) {
        // 포기하지 않고 끝까지 쫓는다. 오래 쫓을수록 빨라짐 (걷기 속도의 4배 → 최대 12배)
        double el = now - g.phaseStart;
        double mult = std::min(12.0, 4.0 + el * 1.6);
        SetAnim(ANIM_WALK);
        AdvanceDefault(dt * std::min(3.0, 1.8 + el * 0.2));
        if (el > 3 && fmod(el, 4.0) < dt) Say(Rand(0, 1) < 0.5 ? L"우라우라" : L"하아?", 1.2);
        // 포인터가 있는 쪽 화면 여유에 따라 잡을 손(방향)을 다시 고름
        MonInfo cmn = MonFrom(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST));
        int side = (cmn.work.right - c.x) >= (c.x - cmn.work.left) ? 1 : -1;
        if (side != g.side) { g.side = side; ho = HandOffset(g.side, g.s); }
        bool arrived = WalkToward(c.x - ho.x, c.y - ho.y, g.cfg.walkSpeedDip * mult * g.mon.scale, dt);
        // 따라잡아도 포인터가 움직이는 동안은 손을 붙인 채 따라다니다가, 잠깐 멈추는 순간 잡는다
        if (arrived && free && now - g.cursorStillSince >= 0.12) {
            MonInfo cm = MonFrom(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST));
            const AnimMeta& m = g.sprites.meta[ANIM_DRAG];
            double beyond = (m.right - g.sprites.gripX) * g.s;  // 손 바깥쪽 몸 폭 (화면 안에 남게)
            double room = g.side > 0 ? cm.work.right - c.x - beyond : c.x - cm.work.left - beyond;
            g.cursorDist = std::clamp(std::min(Rand(150, 250) * cm.scale, room), 0.0, 1e9);
            g.cursorStart = g.cursorSet = c;
            SetAnim(ANIM_DRAG);
            Say(L"우라!");
            Log(L"cursor: 잡음, 목표 %d px", int(g.cursorDist));
            SetState(State::CursorDrag);
        }
        return;
    }
    // CursorDrag: 사용자가 마우스를 움직이면 바로 놓아준다
    if (std::abs(c.x - g.cursorSet.x) > 3 * g.mon.scale || std::abs(c.y - g.cursorSet.y) > 3 * g.mon.scale)
        return EndCursor(L"사용자가 움직임", L"하아? 하아?");
    SetAnim(ANIM_DRAG);
    AdvanceDefault(dt);
    double el = now - g.phaseStart;
    double v = g.cfg.dragSpeedDip * 1.6 * g.mon.scale * std::min(1.0, el / 0.3) * ((g.frame % 2 == 0) ? 1.25 : 0.7);
    g.cursorMoved = std::min(g.cursorDist, g.cursorMoved + v * dt);
    POINT n{g.cursorStart.x + g.side * LONG(std::lround(g.cursorMoved)), g.cursorStart.y};
    if (n.x != g.cursorSet.x) {
        if (!SetCursorPos(n.x, n.y)) return EndCursor(L"포인터 이동 실패", L"하아?");
        GetCursorPos(&g.cursorSet);  // 실제 위치 (경계·DPI 보정 반영)
    }
    g.px = g.cursorSet.x - ho.x;
    g.py = g.cursorSet.y - ho.y;
    if (g.cursorMoved >= g.cursorDist || el > 6) return EndCursor(L"완료", L"이~");
}

// ---------- 마우스로 우사기 잡기 ----------
// 화면 좌표의 점이 지금 그려진 우사기의 불투명한 픽셀 위인가
bool PetHit(POINT pt) {
    if (!g.visible || g.autoHidden || g.drawn.size <= 0 || !IsWindowVisible(g.overlay)) return false;
    int lx = pt.x - g.drawn.x, ly = pt.y - g.drawn.y;
    if (lx < 0 || ly < 0 || lx >= g.drawn.size || ly >= g.drawn.size) return false;
    const Bitmap& b = g.cache.Get(g.sprites, g.drawn.anim, g.drawn.frame, g.drawn.flip, g.drawn.size);
    return (b.px[size_t(ly) * b.w + lx] >> 24) > 60;
}

// 하던 일은 그 자리에서 그만둔다
void StopWork(double now) {
    if (g.state == State::Approach || g.state == State::Drag) { Log(L"drag: 놓기 (사용자가 우사기를 건드림)"); g.target = nullptr; }
    if (g.state == State::PrankLaunch || g.state == State::PrankWalk || g.state == State::PrankType) { g.npWnd = g.npEdit = nullptr; g.prankIgnoreInput = false; }
    if (g.state == State::CursorChase || g.state == State::CursorDrag) g.nextCursor = now + g.cfg.cursorIntervalSec;
}

void BeginHeld(double now) {
    if (!g.cfg.grabbable) { g.press = 0; return; }
    StopWork(now);
    g.chatPendingAt = 0;
    g.hitCount = 0;
    g.press = 2;
    g.trail.clear();
    g.grabOffX = g.px - g.pressPt.x;
    g.grabOffY = g.py - g.pressPt.y;
    g.sleeping = false;
    g.heldT = 0;
    g.heldSideUntil = 0;
    Log(L"held: 잡힘");
    Say(L"하아? 하아?");
    SetState(State::Held);
}

void TrailAdd(double t, double x, double y) {
    if (!g.trail.empty() && g.trail.back().x == x && g.trail.back().y == y) return;
    g.trail.push_back({t, x, y});
    if (g.trail.size() > 32) g.trail.erase(g.trail.begin());
}

void DropPet() {
    g.press = 0;
    // 놓기 직전 약 0.1초 동안의 포인터 속도로 던진다 (멈췄다 놓으면 그냥 떨어짐)
    double now = NowSec();
    POINT c;
    GetCursorPos(&c);
    TrailAdd(now, c.x, c.y);
    g.vx = g.vy = 0;
    const auto& last = g.trail.back();
    for (auto it = g.trail.rbegin(); it != g.trail.rend(); ++it) {
        if (last.t - it->t >= 0.04 && last.t - it->t <= 0.15) {
            double dt = last.t - it->t;
            g.vx = (last.x - it->x) / dt;
            g.vy = (last.y - it->y) / dt;
        }
        if (last.t - it->t > 0.15) break;
    }
    // 포인터가 이미 멈춰 있었으면 던진 게 아님
    if (g.trail.size() < 2 || now - g.trail[g.trail.size() - 2].t > 0.12) g.vx = g.vy = 0;
    double sc = g.mon.scale, maxV = 5000 * sc;
    double sp = std::hypot(g.vx, g.vy);
    if (sp > maxV) { g.vx *= maxV / sp; g.vy *= maxV / sp; sp = maxV; }
    g.thrown = sp > 600 * sc;
    g.bounces = 0;
    if (std::abs(g.vx) > 1) g.facing = g.vx > 0 ? 1 : -1;
    Log(L"held: 놓임 → %s (속도 %.0f, %.0f px/s)", g.thrown ? L"던짐" : L"떨어짐", g.vx, g.vy);
    if (g.thrown) Say(L"우라우라!", 1.2);
    g.nextCursor = std::max(g.nextCursor, now + g.cfg.cursorIntervalSec);  // 내려놓자마자 포인터를 잡지 않게
    SetState(State::Fall);
}

LRESULT CALLBACK MouseHookProc(int code, WPARAM w, LPARAM l) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, w, l);
    auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(l);
    if (m->dwExtraInfo == REPLAY_MAGIC) return CallNextHookEx(nullptr, code, w, l);  // 우리가 다시 보낸 클릭
    double now = NowSec();
    g.lastHookEvent = now;
    g.hookPt = m->pt;
    // 포인터를 던진 뒤 몇 초 동안은 마우스가 안 먹는다 (버튼 떼기는 통과: 눌린 채로 남지 않게)
    if (now < g.blockUntil) {
        if (w == WM_LBUTTONUP || w == WM_RBUTTONUP || w == WM_MBUTTONUP || w == WM_XBUTTONUP) return CallNextHookEx(nullptr, code, w, l);
        ++g.blocked;
        return 1;
    }
    switch (w) {
        case WM_LBUTTONDOWN:
            if (g.press == 0 && (g.cfg.grabbable || g.cfg.angry) && g.state != State::Angry && g.state != State::AngryThrow &&
                PetHit(m->pt)) {
                g.press = 1;
                g.pressPt = m->pt;
                g.pressT = now;
                return 1;  // 삼킴: 클릭인지 잡기인지 아직 모름
            }
            break;
        case WM_MOUSEMOVE:
            if (g.press == 2) TrailAdd(now, m->pt.x, m->pt.y);
            if (g.press == 1) {
                double sc = g.mon.scale;
                if (std::abs(m->pt.x - g.pressPt.x) > 6 * sc || std::abs(m->pt.y - g.pressPt.y) > 6 * sc) BeginHeld(now);
            }
            break;
        case WM_LBUTTONUP:
            if (g.press == 1) {
                g.press = 0;
                // 연달아 누른 횟수: 1번 = 뒤의 앱으로 클릭 전달, 2번 = 대화창, 3번 이상 = 때리기
                double win = std::max(0.5, GetDoubleClickTime() / 1000.0);
                g.hitCount = now - g.lastPetClick < win ? g.hitCount + 1 : 1;
                g.lastPetClick = now;
                g.lastPetClickPt = m->pt;
                if (g.hitCount == 1) {
                    PostMessageW(g.msg, WM_REPLAY_CLICK, 0, 0);  // 짧은 클릭: 그 자리에서 다시 클릭
                } else if (g.hitCount == 2) {
                    g.chatPendingAt = now + win;  // 세 번째가 안 오면 대화창
                } else {
                    g.chatPendingAt = 0;
                    if (g.cfg.angry && g.hitCount >= g.cfg.angryHits) { g.angryPending = true; g.hitCount = 0; }
                    else g.ouchPending = true;
                }
                return 1;
            }
            if (g.press == 2) {
                DropPet();
                return 1;
            }
            break;
    }
    return CallNextHookEx(nullptr, code, w, l);
}

// 던진 뒤 몇 초 동안 키보드도 안 먹게 (키 떼기는 통과). Ctrl+Alt+Del 은 Windows 가 항상 처리한다.
LRESULT CALLBACK KeyHookProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && NowSec() < g.blockUntil) {
        auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(l);
        if (!(k->flags & LLKHF_UP)) { ++g.blocked; return 1; }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

void SetKeyBlock(bool on) {
    if (on && !g.keyHook) g.keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHookProc, g.inst, 0);
    if (!on && g.keyHook) { UnhookWindowsHookEx(g.keyHook); g.keyHook = nullptr; }
}

void InstallMouseHook() {
    if (g.mouseHook) UnhookWindowsHookEx(g.mouseHook);
    g.mouseHook = (g.cfg.grabbable || g.cfg.angry) ? SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, g.inst, 0) : nullptr;
    Log(L"held: 마우스 후크 %s", g.mouseHook ? L"설치" : L"없음");
}

void ReplayClick() {
    INPUT in[2] = {};
    for (auto& i : in) { i.type = INPUT_MOUSE; i.mi.dwExtraInfo = REPLAY_MAGIC; }
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

void TickHeld(double now, double dt) {
    POINT c;
    GetCursorPos(&c);
    // 후크가 시스템에 의해 끊기면(응답 지연) 버튼이 놓인 걸 모를 수 있으니 안전하게 내려놓고 다시 설치
    if (g.state == State::Held && (c.x != g.hookPt.x || c.y != g.hookPt.y) && now - g.lastHookEvent > 2.0) {
        Log(L"held: 후크 응답 없음 → 내려놓고 재설치");
        DropPet();
        InstallMouseHook();
        return;
    }
    SetAnim(ANIM_WALK);
    if (g.state == State::Held && g.sprites.hasHeld) {
        TrailAdd(now, c.x, c.y);
        // 최근 0.12초 포인터 가로 속도로 옆으로 끌려가는 포즈인지 정한다
        double hv = 0;
        for (auto it = g.trail.rbegin(); it != g.trail.rend(); ++it)
            if (now - it->t >= 0.04) { hv = (c.x - it->x) / std::max(0.04, now - it->t); if (now - it->t > 0.12) break; }
        if (std::abs(hv) > 700 * g.mon.scale) { g.heldSideUntil = now + 0.3; g.facing = hv > 0 ? 1 : -1; }
        SetAnim(ANIM_HELD);
        g.heldT += dt;
        int sub = int(g.heldT * 1000 / 170) % 4;
        g.frame = (now < g.heldSideUntil ? 4 : 0) + sub;
        if (int(g.sprites.frames[ANIM_HELD].size()) <= g.frame) g.frame = sub;
        bool flip = Flipped();
        const AnimMeta& m = g.sprites.meta[ANIM_HELD];
        double ax = flip ? g.sprites.frameW - m.anchorX : m.anchorX;
        double hx = flip ? g.sprites.frameW - g.sprites.holdX : g.sprites.holdX;
        g.px = c.x - (hx - ax) * g.s;
        g.py = c.y - (g.sprites.holdY - g.sprites.baseY) * g.s;
        return;
    }
    if (g.state == State::Held) {
        AdvanceDefault(dt * 2.5);  // 다리를 버둥버둥
        double nx = c.x + g.grabOffX;
        if (std::abs(nx - g.px) > 1) g.facing = nx > g.px ? 1 : -1;
        g.px = nx;
        g.py = c.y + g.grabOffY;
        TrailAdd(now, c.x, c.y);
        return;
    }
    // Fall: 던져진 속도로 포물선 비행, 벽·천장에서 튕기고 바닥에서 통통 튄 뒤 멈춘다
    AdvanceDefault(dt * (g.thrown ? 3.0 : 2.0));
    MonInfo m = MonAt(g.px, std::min<double>(g.py, MonAt(g.px, g.py).work.bottom - 2));
    const AnimMeta& am = g.sprites.meta[g.anim];
    double le, re;
    Extents(g.anim, Flipped(), g.s, le, re);
    double bodyH = (g.sprites.baseY - am.top) * g.s;
    double floor = Floor(m);
    if (g.cfg.roam) {
        int n = std::max(1, int(std::ceil(std::hypot(g.vx, g.vy) * dt / (20 * m.scale))));
        double h = dt / n, top = m.work.top + bodyH;
        for (int i = 0; i < n; ++i) {
            double k = std::pow(0.08, h);  // 바닥 마찰처럼 빠르게 느려짐
            g.vx *= k;
            g.vy *= k;
            g.px += g.vx * h;
            g.py += g.vy * h;
            double midY = g.py - bodyH / 2;
            if (g.px - le < m.work.left && g.vx < 0 && !MonitorFromPoint(POINT{LONG(m.work.left - 2), LONG(midY)}, MONITOR_DEFAULTTONULL)) {
                g.px = m.work.left + le; g.vx = -g.vx * 0.6; g.facing = 1;
            }
            if (g.px + re > m.work.right && g.vx > 0 && !MonitorFromPoint(POINT{LONG(m.work.right + 1), LONG(midY)}, MONITOR_DEFAULTTONULL)) {
                g.px = m.work.right - re; g.vx = -g.vx * 0.6; g.facing = -1;
            }
            if (g.py < top && g.vy < 0 && !MonitorFromPoint(POINT{LONG(g.px), LONG(m.work.top - 2)}, MONITOR_DEFAULTTONULL)) {
                g.py = top; g.vy = -g.vy * 0.6;
            }
            if (g.py > floor && g.vy > 0 && !MonitorFromPoint(POINT{LONG(g.px), LONG(m.work.bottom + 1)}, MONITOR_DEFAULTTONULL)) {
                g.py = floor; g.vy = -g.vy * 0.6;
            }
        }
        if (std::hypot(g.vx, g.vy) < 80 * m.scale) {
            g.mon = MonAt(g.px, g.py - 2);
            ClampToFloor();
            Log(L"held: 안착 (%.0f, %.0f)", g.px, g.py);
            Say(g.thrown ? L"푸루야~" : L"푸루!");
            g.thrown = false;
            StartIdle(now);
            g.stateUntil = now + Rand(5.0, 10.0);  // 놓인 자리에서 잠시 머문다
        }
        return;
    }
    // 몇 단계로 나눠 움직여 빠르게 던져도 벽을 뚫지 않게
    int steps = std::max(1, int(std::ceil(std::hypot(g.vx, g.vy) * dt / (20 * m.scale))));
    double h = dt / steps;
    for (int i = 0; i < steps; ++i) {
        g.vy += 2400 * m.scale * h;
        g.vx *= std::pow(0.6, h);  // 공기 저항
        g.px += g.vx * h;
        g.py += g.vy * h;
        double midY = g.py - bodyH / 2;
        // 옆 벽: 그쪽에 다른 모니터가 없을 때만 튕김 (있으면 넘어감)
        if (g.px - le < m.work.left && g.vx < 0 &&
            !MonitorFromPoint(POINT{LONG(m.work.left - 2), LONG(midY)}, MONITOR_DEFAULTTONULL)) {
            g.px = m.work.left + le;
            g.vx = -g.vx * 0.55;
            g.facing = 1;
        }
        if (g.px + re > m.work.right && g.vx > 0 &&
            !MonitorFromPoint(POINT{LONG(m.work.right + 1), LONG(midY)}, MONITOR_DEFAULTTONULL)) {
            g.px = m.work.right - re;
            g.vx = -g.vx * 0.55;
            g.facing = -1;
        }
        // 천장
        if (g.py - bodyH < m.work.top && g.vy < 0 &&
            !MonitorFromPoint(POINT{LONG(g.px), LONG(m.work.top - 2)}, MONITOR_DEFAULTTONULL)) {
            g.py = m.work.top + bodyH;
            g.vy = -g.vy * 0.4;
        }
        // 바닥
        if (g.py >= floor && g.vy > 0) {
            g.py = floor;
            if (g.vy > 700 * m.scale && g.bounces < 4) {
                g.vy = -g.vy * 0.38;  // 통통
                g.vx *= 0.7;
                ++g.bounces;
            } else {
                g.mon = m;
                ClampToFloor();
                if (std::abs(g.vx) > 1) g.facing = g.vx > 0 ? 1 : -1;
                Log(L"held: 착지 (튕김 %d번)", g.bounces);
                Say(g.thrown ? L"푸루야~" : L"푸루!");
                g.thrown = false;
                StartWalk(now);
                return;
            }
        }
    }
}

void CancelBusy();
void ApplyMode(MusicMode m);

// ---------- 때리면 화내고 포인터 던지기 ----------
void BeginAngry(double now) {
    StopWork(now);
    g.press = 0;
    g.chatPendingAt = 0;
    g.sleeping = false;
    g.angryT0 = now;
    Log(L"angry: 화남");
    Say(L"하아?! 하아?!", 1.6);
    SetState(State::Angry);
}

void EndAngry(double now, const wchar_t* why) {
    g.blockUntil = 0;
    SetKeyBlock(false);
    Log(L"angry: 끝 (%s), 막은 입력 %ld개", why, g.blocked);
    g.tint = 0;
    SetState(State::Return, why);
    (void)now;
}

void TickAngry(double now, double dt) {
    double sc = g.mon.scale, t = now - g.angryT0;
    POINT c;
    GetCursorPos(&c);
    if (g.state == State::Angry) {
        // 포인터 쪽을 노려보며 발을 구르고 부들부들 떤다
        SetAnim(ANIM_WALK);
        g.frame = int(now / 0.07) % 4;
        g.facing = c.x >= g.px ? 1 : -1;
        g.shakeX = std::sin(now * 70) * 3 * sc;
        g.tint = 2 + int(now / 0.12) % 3;
        if (t < 1.0) return;
        const wchar_t* why = nullptr;
        if (!CursorFree(&why)) {
            Log(L"angry: 포인터를 못 잡음 (%s)", why);
            Say(L"흥!");
            g.tint = 0;
            return StartWalk(now);
        }
        // 포인터를 낚아챈다: 손 쪽으로 순간 이동
        g.side = c.x >= g.px ? 1 : -1;
        SetAnim(ANIM_DRAG);
        g.frame = 0;
        POINT h = HandOffset(g.side, g.s);
        g.throwFrom = POINT{LONG(g.px + h.x), LONG(g.py + h.y)};
        SetCursorPos(g.throwFrom.x, g.throwFrom.y);
        // 반대쪽 먼 곳 위로 던진다
        const RECT& w = g.mon.work;
        double W = w.right - w.left, H = w.bottom - w.top;
        bool toRight = g.px < (w.left + w.right) / 2.0;
        double tx = toRight ? Rand(w.right - W * 0.3, w.right - W * 0.08) : Rand(w.left + W * 0.08, w.left + W * 0.3);
        g.throwTo = POINT{LONG(tx), LONG(Rand(w.top + H * 0.2, w.top + H * 0.55))};
        g.blocked = 0;
        g.laughed = false;
        g.blockUntil = now + 0.45 + 0.75 + std::clamp(g.cfg.blockSec, 0.0, 10.0);
        SetKeyBlock(g.cfg.blockSec > 0);
        g.angryT0 = now;
        Log(L"angry: 포인터 낚아챔 → (%ld,%ld)로 던짐", g.throwTo.x, g.throwTo.y);
        SetState(State::AngryThrow);
        return;
    }
    // AngryThrow
    const double windup = 0.45, flight = 0.75;
    if (t < windup) {
        // 뒤로 젖혔다가
        SetAnim(ANIM_DRAG);
        g.frame = int(t / 0.12) % 4;
        g.tint = 4;
        g.shakeX = std::sin(now * 90) * 2 * sc;
        double k = t / windup;
        SetCursorPos(int(g.throwFrom.x - g.side * k * 26 * sc), int(g.throwFrom.y - k * 14 * sc));
        return;
    }
    if (t < windup + flight) {
        // 휙! 포물선으로 날아간다
        if (g.anim != ANIM_WALK) Say(L"우라!!", 1.2);
        SetAnim(ANIM_WALK);
        g.frame = 1;
        g.facing = g.throwTo.x >= g.px ? 1 : -1;
        double u = (t - windup) / flight;
        g.tint = int(std::lround(4 * (1 - u)));
        double e = 1 - (1 - u) * (1 - u);  // 처음에 빠르게
        double sx = g.throwFrom.x - g.side * 26 * sc, sy = g.throwFrom.y - 14 * sc;
        double arc = g.mon.work.bottom - g.mon.work.top;
        double x = sx + (g.throwTo.x - sx) * e;
        double y = sy + (g.throwTo.y - sy) * e - arc * 0.35 * 4 * e * (1 - e);
        SetCursorPos(int(x), int(std::max<double>(y, g.mon.work.top + 4)));
        return;
    }
    // 불타는 포인터가 몸부림치는 동안 우사기는 깔깔
    g.tint = 0;
    if (!g.laughed) { g.laughed = true; Say(L"야하! 야하!", 2.0); }
    SetAnim(ANIM_DANCE);
    AdvanceDefault(dt * 1.6);
    double b = t - windup - flight;
    SetCursorPos(int(g.throwTo.x + std::sin(b * 23) * 7 * sc + std::sin(b * 7) * 10 * sc),
                 int(g.throwTo.y + std::cos(b * 29) * 5 * sc - std::abs(std::sin(b * 5)) * 12 * sc));
    if (now >= g.blockUntil) {
        Say(L"이~");
        EndAngry(now, L"던지기 끝");
    }
}

// ---------- 스포티파이 따라 부르기 ----------
std::wstring FmtTime(double s) {
    wchar_t b[16];
    int t = int(std::max(0.0, s));
    swprintf_s(b, L"%d:%02d", t / 60, t % 60);
    return b;
}

// 매 틱: 곡이 바뀌면 가사를 받고, 재생 위치에 맞는 줄을 말풍선으로
void TickSing(double now) {
    g.song = g.media.Get();
    const MediaSnapshot& m = g.song;
    std::wstring key = m.ok && !m.title.empty() ? m.artist + L"\n" + m.title : L"";
    if (key != g.songKey) {
        g.songKey = key;
        g.lyricIdx = -1;
        g.songLyrics = nullptr;
        if (!key.empty() && g.cfg.sing) {
            g.songLyrics = g.lyrics.Get(m.title, m.artist, m.album, m.duration);
            Log(L"sing: 곡 바뀜 '%s' - %s (%s)", m.title.c_str(), m.artist.c_str(), FmtTime(m.duration).c_str());
            if (m.playing && g.cfg.bubbles) g.bubble.Say(L"♪ " + m.title, 2.5);
        }
    }
    bool ready = g.songLyrics && g.songLyrics->status == LyricsEntry::Ready;
    g.singing = g.cfg.sing && m.ok && m.playing && ready && !g.paused;
    if (!g.singing) return;
    const auto& L = g.songLyrics->lines;
    double pos = m.PositionAt(now) - g.cfg.lyricsOffsetSec;
    int idx = -1;
    for (int i = 0; i < int(L.size()) && L[i].t <= pos; ++i) idx = i;
    if (idx == g.lyricIdx) return;
    g.lyricIdx = idx;
    if (idx < 0 || L[idx].text.empty()) return;  // 전주·간주
    double next = idx + 1 < int(L.size()) ? L[idx + 1].t : L[idx].t + 5;
    double dur = std::clamp(next - pos - 0.05, 1.0, 7.0);
    if (g.cfg.bubbles && g.state == State::Music) g.bubble.Say(L"♪ " + L[idx].text, dur);
}

std::wstring NowPlayingText() {
    const MediaSnapshot& m = g.song;
    if (!m.ok) return L"스포티파이가 꺼져 있거나 재생 중인 곡이 없음";
    std::wstring s = (m.playing ? L"재생 중: " : L"멈춤: ") + m.title + L" — " + m.artist;
    if (!m.album.empty()) s += L" (앨범 " + m.album + L")";
    s += L", " + FmtTime(m.PositionAt(NowSec())) + L" / " + FmtTime(m.duration);
    if (!m.spotify) s += L", 앱: " + m.app;
    if (!g.cfg.sing) s += L", 따라 부르기 꺼짐";
    else if (!g.songLyrics || g.songLyrics->status == LyricsEntry::Pending) s += L", 가사 찾는 중";
    else s += g.songLyrics->status == LyricsEntry::Ready ? L", 가사 있음 → 우사기가 따라 부름" : L", 맞는 가사를 못 찾음 (춤만 춤)";
    return s;
}

// ---------- 에이전트(Gemini) 연결 ----------
std::wstring Lower(std::wstring s) {
    CharLowerBuffW(s.data(), DWORD(s.size()));
    return s;
}

std::vector<TargetWindow> AgentTargets() {
    std::vector<TargetWindow> v;
    for (auto& w : ListMovableWindows(g.overlay, g.msg))
        if (w.hwnd != Agent::ChatWindow()) v.push_back(w);
    return v;
}

bool Busy() {
    switch (g.state) {
        case State::Approach: case State::Drag: case State::PrankLaunch: case State::PrankWalk: case State::PrankType:
        case State::CursorChase: case State::CursorDrag: case State::Held: case State::Fall:
        case State::Angry: case State::AngryThrow: return true;
        default: return false;
    }
}

std::wstring SaveNoteFile(const std::wstring& text) {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    if (dir.empty()) return L"오류:문서 폴더를 찾지 못함";
    dir += L"\\우사기 메모";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"\\%04d%02d%02d-%02d%02d%02d.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring path = dir + name;
    std::wstring crlf;
    for (wchar_t c : text) {
        if (c == L'\n') crlf += L"\r\n";
        else if (c != L'\r') crlf += c;
    }
    std::string u = "\xEF\xBB\xBF" + ToUtf8(crlf);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"오류:파일을 만들지 못함";
    DWORD wr = 0;
    WriteFile(f, u.data(), DWORD(u.size()), &wr, nullptr);
    CloseHandle(f);
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return L"메모장이 이미 열려 있어서 파일로 저장하고 열었음: " + path;
}

void InitAgent() {
    AgentHost h;
    h.name = [] { return std::wstring(CurChar().name); };
    h.charId = [] { return std::wstring(CurChar().id); };
    h.persona = [] { return std::wstring(CurChar().persona); };
    h.say = [](const std::wstring& t, double sec) {
        std::wstring x = t.size() > 160 ? t.substr(0, 157) + L"…" : t;
        Say(x.c_str(), sec);
    };
    h.thinking = [](bool on) {
        if (on && g.cfg.bubbles) g.bubble.Say(L"음…", 60);
    };
    h.anchor = []() {
        const AnimMeta& m = g.sprites.meta[g.anim];
        return POINT{LONG(g.px), LONG(g.py - (g.sprites.baseY - m.top) * g.s)};
    };
    h.status = []() {
        std::wstring s = StateName(g.state);
        s += g.visible ? L", 보임" : L", 숨김";
        if (g.paused) s += L", 일시정지";
        s += g.cfg.mode == MODE_DANCE ? L", 음악 모드=춤" : g.cfg.mode == MODE_RAP ? L", 음악 모드=랩" : L", 음악 모드=끔";
        if (g.snd.playing) s += L", 지금 음악 재생 중";
        return s;
    };
    h.listWindows = []() {
        std::vector<std::wstring> v;
        for (auto& w : AgentTargets()) v.push_back(w.title);
        return v;
    };
    h.dragWindow = [](const std::wstring& title) -> std::wstring {
        if (title.empty()) return L"오류:제목이 비어 있음";
        if (!g.visible) return L"오류:우사기가 숨겨져 있음";
        if (Busy()) return L"오류:우사기가 다른 일을 하는 중";
        std::wstring want = Lower(title);
        for (auto& w : AgentTargets())
            if (Lower(w.title).find(want) != std::wstring::npos) {
                BeginApproach(w.hwnd);
                return L"'" + w.title + L"' 창으로 걸어가는 중 (곧 끌기 시작)";
            }
        return L"오류:그런 제목의 끌 수 있는 창이 없음 (최소화·최대화·전체 화면·관리자 창은 못 끎)";
    };
    h.setMusicMode = [](const std::wstring& m) -> std::wstring {
        if (m == L"dance") ApplyMode(MODE_DANCE);
        else if (m == L"rap") ApplyMode(MODE_RAP);
        else if (m == L"off") ApplyMode(MODE_OFF);
        else return L"모르는 모드";
        return L"바꿨음: " + m;
    };
    h.writeNote = [](const std::wstring& text) -> std::wstring {
        std::wstring t;
        for (wchar_t c : text) {
            if (c == L'\n') t += L'\r';
            else if (c != L'\r') t += c;
        }
        if (g.visible && !g.autoHidden && !Busy() && NotepadWindows().empty() && StartNotepadPrank(&t)) {
            g.prankIgnoreInput = true;  // 글자는 메모장에 직접 보내므로 사용자가 다른 데서 타이핑해도 괜찮음
            return L"메모장을 열고 우사기가 타이핑하는 중";
        }
        return SaveNoteFile(text);
    };
    h.nowPlaying = []() { return NowPlayingText(); };
    h.spotifyControl = [](const std::wstring& c) -> std::wstring {
        if (c != L"play" && c != L"pause" && c != L"play_pause" && c != L"next" && c != L"previous") return L"오류:모르는 동작";
        if (!g.song.ok) {
            if (c == L"play" || c == L"play_pause") {
                ShellExecuteW(nullptr, L"open", L"spotify:", nullptr, nullptr, SW_SHOWNORMAL);
                return L"스포티파이를 열었음. 아직 재생 세션이 없어서 사용자가 재생을 눌러야 할 수 있음";
            }
            return L"오류:스포티파이 재생 세션이 없음";
        }
        g.media.Command(c);
        return L"보냄: " + c;
    };
    h.petAction = [](const std::wstring& a) -> std::wstring {
        if (a == L"cancel") { if (Busy()) CancelBusy(); return L"멈췄음"; }
        if (Busy()) return L"다른 일을 하는 중이라 못 함";
        if (a == L"prank") { MaybePrank(NowSec(), true); return L"장난 시작"; }
        if (a == L"grab_cursor") { MaybeGrabCursor(NowSec(), true); return L"포인터 쫓아가는 중"; }
        return L"모르는 동작";
    };
    std::vector<std::string> models;
    std::string list = ToUtf8(g.cfg.agentModels) + ",";
    for (size_t a = 0, b; (b = list.find(',', a)) != std::string::npos; a = b + 1) {
        std::string m = list.substr(a, b - a);
        while (!m.empty() && m.front() == ' ') m.erase(m.begin());
        while (!m.empty() && m.back() == ' ') m.pop_back();
        if (!m.empty()) models.push_back(m);
    }
    Agent::Init(g.inst, h, models);
}

// ---------- 메인 틱 ----------
void Tick() {
    double now = NowSec();
    double dt = std::clamp(now - g.lastTick, 0.0, 0.1);
    {
        POINT cp;
        GetCursorPos(&cp);
        if (cp.x != g.cursorSeen.x || cp.y != g.cursorSeen.y) { g.cursorSeen = cp; g.cursorStillSince = now; }
    }
    g.lastTick = now;
    g.snd = g.audio.Running() ? g.audio.Get() : AudioSnapshot{};
    TickSing(now);
    // 소리가 감지되거나, 스포티파이 곡을 따라 부를 수 있으면 음악 상태
    bool music = (g.cfg.mode != MODE_OFF && g.snd.playing) || g.singing;

    UpdateScaleFor(g.px, g.py - 2);  // 기준점이 있는 모니터의 DPI·작업 영역
    g.shakeX = 0;
    if (g.state != State::Angry && g.state != State::AngryThrow) g.tint = 0;
    // 때리기 → 화내기, 더블클릭 → 대화창
    if (g.angryPending) {
        g.angryPending = false;
        if (g.state != State::Angry && g.state != State::AngryThrow && g.state != State::Held && g.state != State::Fall) BeginAngry(now);
    }
    if (g.ouchPending) { g.ouchPending = false; Say(L"아야!", 1.0); }
    if (g.chatPendingAt > 0 && now >= g.chatPendingAt) {
        g.chatPendingAt = 0;
        g.hitCount = 0;
        PostMessageW(g.msg, WM_OPEN_CHAT, 0, 0);
    }
    // 안전장치: 막는 시간이 지나면 무슨 상태든 키보드 후크를 푼다
    if (g.keyHook && now >= g.blockUntil) SetKeyBlock(false);
    // 우사기 위에서 0.3초 이상 누르고만 있어도 잡힌 것으로
    if (g.press == 1 && now - g.pressT > 0.3) BeginHeld(now);
    // 모니터가 사라졌거나 기준점이 어떤 모니터에도 없으면 가까운 작업 영역으로
    if (!MonitorFromPoint(POINT{LONG(g.px), LONG(g.py - 2)}, MONITOR_DEFAULTTONULL) &&
        g.state != State::Drag && g.state != State::Approach && g.state != State::Held && g.state != State::Fall) {
        ClampToFloor();
        Log(L"monitor: 기준점이 화면 밖 → 작업 영역으로 이동");
    }

    switch (g.state) {
        case State::Walk:
        case State::Idle: {
            if (music) {
                g.sleeping = false;
                SetState(State::Music, L"(재생 감지)");
                Say(g.cfg.mode == MODE_RAP ? L"우라우라!" : L"우라!");
                break;
            }
            SetAnim(ANIM_WALK);
            // 사용자가 오래 자리를 비우면 졸고, 돌아오면 "어서 와"
            double idle = UserIdleSec();
            if (!g.sleeping && idle >= g.cfg.sleepIdleSec) { g.sleeping = true; g.lastSnore = 0; Log(L"sleep: 졸기 시작"); }
            if (g.sleeping) {
                if (idle < 1.0) {
                    g.sleeping = false;
                    Say(L"미야아아~", 2.2);
                    StartWalk(now);
                } else {
                    g.frame = 1;
                    if (now - g.lastSnore > 6) { g.lastSnore = now; Say(L"구우구우~", 3.0); }
                    ClampToFloor();
                    break;
                }
            }
            if (!g.paused) MaybePrank(now, false);
            if (!g.paused && (g.state == State::Walk || g.state == State::Idle)) MaybeGrabCursor(now, false);
            if (g.state != State::Walk && g.state != State::Idle) break;
            bool moving = g.state == State::Walk && !g.paused;
            if (moving && g.cfg.roam) {  // 목표점까지 2D 로 걸어간다
                bool arrived = WalkToward(g.roamX, g.roamY, g.cfg.walkSpeedDip * g.mon.scale, dt);
                AdvanceDefault(dt);
                ClampToFloor();
                if (arrived || now > g.stateUntil) StartIdle(now);
                break;
            }
            if (moving) {
                g.px += g.facing * g.cfg.walkSpeedDip * g.mon.scale * dt;
                AdvanceDefault(dt);
                if (now > g.stateUntil) StartIdle(now);
            } else {
                g.frame = 1;  // 두 발을 모은 프레임
                if (g.state == State::Idle && now > g.stateUntil && !g.paused) StartWalk(now);
            }
            if (ClampToFloor() && moving) {
                double le, re;
                Extents(g.anim, Flipped(), g.s, le, re);
                g.facing = g.px < (g.mon.work.left + g.mon.work.right) / 2.0 ? 1 : -1;
            }
            break;
        }
        case State::Music: {
            if (!music) {
                StartWalk(now);
                break;
            }
            SetAnim(g.singing || g.cfg.mode == MODE_RAP ? ANIM_RAP : ANIM_DANCE);  // 노래할 땐 랩 동작
            AdvanceMusic(dt, now);
            ClampToFloor();
            if (!g.paused) MaybeGrabCursor(now, false, true);  // 춤추다가도 포인터가 가까이 오면 잡음
            break;
        }
        case State::Approach: {
            if (!IsWindow(g.target) || !IsMovableWindow(g.target)) { Release(L"대상이 더 이상 유효하지 않음"); break; }
            if (now - g.phaseStart > 20) { Release(L"접근 시간 초과"); break; }
            SetAnim(ANIM_WALK);
            AdvanceDefault(dt * 1.4);
            double tx, ty;
            GripTarget(VisibleFrame(g.target), tx, ty);
            if (WalkToward(tx, ty, g.cfg.walkSpeedDip * 2.2 * g.mon.scale, dt)) {
                SetAnim(ANIM_DRAG);
                StartDrag(now);
            }
            break;
        }
        case State::Drag:
            SetAnim(ANIM_DRAG);
            AdvanceDefault(dt);
            TickDrag(now, dt);
            break;
        case State::Release:
            SetAnim(ANIM_WALK);
            g.frame = 1;
            if (now > g.stateUntil) SetState(State::Return);
            break;
        case State::PrankLaunch:
        case State::PrankWalk:
        case State::PrankType:
            TickPrank(now, dt);
            break;
        case State::CursorChase:
        case State::CursorDrag:
            TickCursor(now, dt);
            break;
        case State::Held:
        case State::Fall:
            TickHeld(now, dt);
            break;
        case State::Angry:
        case State::AngryThrow:
            TickAngry(now, dt);
            break;
        case State::Return: {
            if (g.cfg.roam) {
                ClampToFloor();
                if (music) { SetState(State::Music, L"(복귀 후 음악)"); Say(g.cfg.mode == MODE_RAP ? L"우라우라!" : L"우라!"); }
                else StartIdle(now);
                break;
            }
            SetAnim(ANIM_WALK);
            AdvanceDefault(dt);
            double fl = Floor(g.mon);
            double tx = g.px + g.facing * 1.0;
            if (WalkToward(tx, fl, g.cfg.walkSpeedDip * 2.0 * g.mon.scale, dt) || std::abs(g.py - fl) < 1.5) {
                g.py = fl;
                // 바닥에 닿은 뒤 현재 음악 모드/재생 상태 적용
                if (music) { SetState(State::Music, L"(복귀 후 음악)"); Say(g.cfg.mode == MODE_RAP ? L"우라우라!" : L"우라!"); }
                else StartWalk(now);
            }
            break;
        }
    }

    // 전체 화면 앱(게임, 영상) 위에서는 잠시 숨김
    if (now - g.lastSlow > 0.5) {
        g.lastSlow = now;
        bool hide = false;
        HWND fg = GetForegroundWindow();
        if (g.cfg.hideOnFullscreen && fg && fg != g.overlay && g.state != State::Drag && g.state != State::Approach) {
            wchar_t cls[64] = {};
            GetClassNameW(fg, cls, 64);
            if (wcscmp(cls, L"Progman") && wcscmp(cls, L"WorkerW")) {
                RECT r;
                GetWindowRect(fg, &r);
                HMONITOR fm = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
                MonInfo mi = MonFrom(fm);
                if (fm == g.mon.h && r.left <= mi.mon.left && r.top <= mi.mon.top && r.right >= mi.mon.right && r.bottom >= mi.mon.bottom)
                    hide = true;
            }
        }
        if (hide != g.autoHidden) Log(L"fullscreen: %s", hide ? L"숨김" : L"다시 보임");
        // 음식 이야기가 나오는 창을 보면 쥬르리
        if (fg && fg != g.lastFoodWnd && now - g.lastFood > 120) {
            wchar_t t[256] = {};
            GetWindowTextW(fg, t, 256);
            std::wstring low = t;
            for (auto& ch : low) ch = wchar_t(towlower(ch));
            static const wchar_t* food[] = {L"맛집", L"레시피", L"요리", L"배달", L"치킨", L"피자", L"라면", L"간식", L"디저트",
                                            L"케이크", L"먹방", L"배민", L"쿠팡이츠", L"요기요", L"recipe", L"pizza", L"chicken", L"cake", L"food"};
            for (auto* k : food)
                if (low.find(k) != std::wstring::npos) { g.lastFood = now; g.lastFoodWnd = fg; Say(L"쥬르리…", 2.0); break; }
        }
        g.autoHidden = hide;
    }
    if (now - g.lastTopmost > 2.0) {
        g.lastTopmost = now;
        SetWindowPos(g.overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    if (now - g.lastLog > 1.0) {
        g.lastLog = now;
        Log(L"tick: state=%s anim=%d frame=%d pos=(%.0f,%.0f) scale=%.2f snd{ok=%d play=%d lock=%d bpm=%.0f lvl=%.1f restarts=%d} beatMode=%d",
            StateName(g.state), g.anim, g.frame, g.px, g.py, g.mon.scale, g.snd.deviceOk, g.snd.playing, g.snd.beatLocked,
            60.0 / std::max(0.1, g.snd.period), g.snd.levelDb, g.snd.restarts, g.beatMode);
    }
    Render();
    {
        const AnimMeta& m = g.sprites.meta[g.anim];
        double headY = g.py - (g.sprites.baseY - m.top) * g.s;
        bool flip = Flipped();
        double cx = g.px + (flip ? -1 : 1) * ((m.left + m.right) / 2.0 - m.anchorX) * g.s;
        g.bubble.Update(now, cx, headY - 2 * g.mon.scale, g.mon.scale, g.mon.work, g.visible && !g.autoHidden);
    }
    {
        // 던진 포인터에 불: 날아가는 순간부터 입력이 풀릴 때까지
        POINT c;
        GetCursorPos(&c);
        bool burning = g.state == State::AngryThrow && now - g.angryT0 >= 0.4 && now < g.blockUntil;
        if (burning || g.fire.Visible()) g.fire.Update(now, c, MonAt(c.x, c.y).scale, burning);
    }
}

// ---------- 트레이 ----------
void AddTray() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g.msg;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g.icon ? g.icon : LoadIcon(nullptr, IDI_APPLICATION);
    swprintf_s(nid.szTip, L"%s 데스크톱 펫", CurChar().name);
    g.trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}
void RemoveTray() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g.msg;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void UpdateTrayIcon() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g.msg;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = g.icon ? g.icon : LoadIcon(nullptr, IDI_APPLICATION);
    swprintf_s(nid.szTip, L"%s 데스크톱 펫", CurChar().name);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void CancelBusy() {
    if (g.state == State::CursorChase || g.state == State::CursorDrag) return EndCursor(L"사용자 취소", L"하아?");
    if (g.state == State::PrankLaunch || g.state == State::PrankWalk || g.state == State::PrankType) EndPrank(L"사용자 취소", L"하아?");
    else Release(L"사용자 취소", L"하아?");
}

// 어디든 돌아다니기 켜기/끄기 (끄면 바닥으로 돌아감)
void SetRoam(bool on) {
    g.cfg.roam = on;
    WritePrivateProfileStringW(L"pet", L"roam", on ? L"1" : L"0", SettingsPath().c_str());
    Log(L"roam: %s", on ? L"켬" : L"끔");
    if (g.state == State::Walk || g.state == State::Idle) {
        if (on) { Say(L"야하!"); StartWalk(NowSec()); }
        else { Say(L"푸루!"); SetState(State::Return, L"(바닥으로)"); }
    }
}

// 캐릭터 바꾸기: 스프라이트를 새로 읽고 설정 파일에 저장한다
bool SwitchCharacter(const std::wstring& id) {
    SpriteSet ns;
    std::wstring err;
    if (!ns.Load(AssetsDir(), id, err)) { Log(L"character: %s 실패 (%s)", id.c_str(), err.c_str()); return false; }
    if (g.state == State::Held || g.state == State::Angry || g.state == State::AngryThrow) return false;
    if (Busy()) CancelBusy();
    g.sprites = std::move(ns);
    g.cache.Reset();
    g.drawn.size = -1;
    g.anim = -1;
    SetAnim(ANIM_WALK);
    g.cfg.character = id;
    WritePrivateProfileStringW(L"pet", L"character", id.c_str(), SettingsPath().c_str());
    HICON old = g.icon;
    g.icon = MakeIconFromBitmap(g.sprites.frames[ANIM_WALK][0], GetSystemMetrics(SM_CXSMICON) * 2);
    UpdateTrayIcon();
    if (old) DestroyIcon(old);
    Log(L"character: %s", id.c_str());
    Say((std::wstring(CurChar().name) + L" 등장!").c_str(), 2.2);
    return true;
}

void ApplyMode(MusicMode m) {
    if (m != g.cfg.mode) Say(L"푸루!");
    g.cfg.mode = m;
    Log(L"mode: %s", m == MODE_DANCE ? L"춤" : m == MODE_RAP ? L"랩" : L"끄기");
    if (m == MODE_OFF) g.audio.Stop();
    else g.audio.Start(g.cfg.audio);
}

std::wstring EscapeMenu(std::wstring s) {
    if (s.size() > 60) s = s.substr(0, 57) + L"...";
    std::wstring o;
    for (wchar_t c : s) { if (c == L'&') o += L'&'; o += c; }
    return o;
}

void ShowTrayMenu() {
    HWND prevFg = GetForegroundWindow();
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_CHAT, g.cfg.chatHotkey ? L"우사기와 대화…\tCtrl+Alt+U" : L"우사기와 대화…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TOGGLE_VIS, g.visible ? L"캐릭터 숨기기" : L"캐릭터 보이기");
    AppendMenuW(menu, MF_STRING | (g.paused ? MF_CHECKED : 0), ID_PAUSE, L"이동 일시정지");
    HMENU cm = CreatePopupMenu();
    int curChar = 0;
    for (int i = 0; i < kCharCount; ++i) {
        AppendMenuW(cm, MF_STRING, ID_CHAR_BASE + i, kChars[i].name);
        if (g.cfg.character == kChars[i].id) curChar = i;
    }
    CheckMenuRadioItem(cm, ID_CHAR_BASE, ID_CHAR_BASE + kCharCount - 1, ID_CHAR_BASE + curChar, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, UINT_PTR(cm), L"캐릭터 고르기");
    AppendMenuW(menu, MF_STRING | (g.cfg.roam ? MF_CHECKED : 0), ID_ROAM_TOGGLE, L"화면 어디든 돌아다니기 (놓은 자리에 안착)");
    HMENU mm = CreatePopupMenu();
    AppendMenuW(mm, MF_STRING, ID_MODE_DANCE, L"춤");
    AppendMenuW(mm, MF_STRING, ID_MODE_RAP, L"랩");
    AppendMenuW(mm, MF_STRING, ID_MODE_OFF, L"끄기");
    CheckMenuRadioItem(mm, ID_MODE_DANCE, ID_MODE_OFF, ID_MODE_DANCE + g.cfg.mode, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, UINT_PTR(mm), L"음악 모드");
    HMENU dm = CreatePopupMenu();
    g.menuTargets = ListMovableWindows(g.overlay, g.msg);
    if (g.menuTargets.empty()) {
        AppendMenuW(dm, MF_STRING | MF_GRAYED, 0, L"끌 수 있는 창이 없습니다");
    } else {
        for (size_t i = 0; i < g.menuTargets.size(); ++i) {
            std::wstring t = EscapeMenu(g.menuTargets[i].title);
            if (!g.menuTargets[i].exe.empty()) t += L"\t" + g.menuTargets[i].exe;
            AppendMenuW(dm, MF_STRING, ID_TARGET_BASE + UINT(i), t.c_str());
        }
    }
    AppendMenuW(menu, MF_POPUP, UINT_PTR(dm), L"창 선택 후 끌기");
    HMENU pm = CreatePopupMenu();
    AppendMenuW(pm, MF_STRING, ID_PRANK_OFF, L"끄기");
    AppendMenuW(pm, MF_STRING, ID_PRANK_SOMETIMES, L"가끔 (5~10분마다)");
    AppendMenuW(pm, MF_STRING, ID_PRANK_OFTEN, L"자주 (1~3분마다)");
    CheckMenuRadioItem(pm, ID_PRANK_OFF, ID_PRANK_OFTEN, ID_PRANK_OFF + g.cfg.prank, MF_BYCOMMAND);
    AppendMenuW(pm, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(pm, MF_STRING | (g.cfg.prankNotepad ? MF_CHECKED : 0), ID_PRANK_NOTEPAD, L"메모장 놀리기 포함");
    bool busy = g.state == State::Approach || g.state == State::Drag || g.state == State::PrankLaunch ||
                g.state == State::PrankWalk || g.state == State::PrankType || g.state == State::CursorChase ||
                g.state == State::CursorDrag;
    AppendMenuW(pm, MF_STRING | (busy ? MF_GRAYED : 0), ID_PRANK_NOW, L"지금 장난치기");
    AppendMenuW(menu, MF_POPUP, UINT_PTR(pm), L"장난 모드");
    AppendMenuW(menu, MF_STRING | (g.cfg.bubbles ? MF_CHECKED : 0), ID_BUBBLES, L"말풍선");
    AppendMenuW(menu, MF_STRING | (g.cfg.voice ? MF_CHECKED : 0), ID_VOICE_TOGGLE, L"녹음한 목소리 재생");
    AppendMenuW(menu, MF_STRING, ID_VOICE_REC, L"목소리 녹음…");
    AppendMenuW(menu, MF_STRING, ID_GEMINI_KEY, L"Gemini API 키…");
    wchar_t ct[64];
    swprintf_s(ct, L"마우스 포인터 가져가기 (%g초마다)", g.cfg.cursorIntervalSec);
    AppendMenuW(menu, MF_STRING | (g.cfg.cursorGrab ? MF_CHECKED : 0), ID_CURSOR_TOGGLE, ct);
    wchar_t at[96];
    swprintf_s(at, L"때리면 화내기 (포인터 던짐, %g초 입력 막힘)", g.cfg.blockSec);
    AppendMenuW(menu, MF_STRING | (g.cfg.angry ? MF_CHECKED : 0), ID_ANGRY_TOGGLE, at);
    AppendMenuW(menu, MF_STRING | (g.cfg.sing ? MF_CHECKED : 0), ID_SING_TOGGLE, L"스포티파이 노래 따라 부르기");
    AppendMenuW(menu, MF_STRING | (busy ? 0 : MF_GRAYED), ID_CANCEL, L"끌기·장난 취소");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    std::wstring st = L"소리 감지: ";
    if (g.cfg.mode == MODE_OFF) st += L"꺼짐";
    else if (!g.snd.deviceOk) st += L"출력 장치 연결 중…";
    else st += g.snd.playing ? (g.snd.beatLocked ? L"재생 중 (박자 맞춤)" : L"재생 중") : L"대기";
    AppendMenuW(menu, MF_STRING | MF_GRAYED, ID_STATUS, st.c_str());
    if (g.song.ok) {
        std::wstring np = L"♪ " + g.song.title + L" — " + g.song.artist;
        if (np.size() > 60) np = np.substr(0, 57) + L"...";
        np += g.singing ? L" (따라 부르는 중)" : (g.songLyrics && g.songLyrics->status == LyricsEntry::None ? L" (가사 없음)" : L"");
        AppendMenuW(menu, MF_STRING | MF_GRAYED, ID_STATUS, EscapeMenu(np).c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"종료");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g.msg);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g.msg, nullptr);
    PostMessageW(g.msg, WM_NULL, 0, 0);
    DestroyMenu(menu);
    // 메뉴 때문에 바뀐 포커스를 원래 쓰던 앱으로 돌려준다
    if (prevFg && prevFg != g.msg && IsWindow(prevFg)) SetForegroundWindow(prevFg);

    switch (cmd) {
        case ID_TOGGLE_VIS:
            g.visible = !g.visible;
            if (g.visible) Say(L"미야야야~");
            break;
        case ID_PRANK_OFF: g.cfg.prank = PRANK_OFF; break;
        case ID_PRANK_SOMETIMES: g.cfg.prank = PRANK_SOMETIMES; ScheduleNextPrank(NowSec()); Say(L"우라!"); break;
        case ID_PRANK_OFTEN: g.cfg.prank = PRANK_OFTEN; ScheduleNextPrank(NowSec()); Say(L"우라우라!"); break;
        case ID_PRANK_NOTEPAD: g.cfg.prankNotepad = !g.cfg.prankNotepad; break;
        case ID_PRANK_NOW: MaybePrank(NowSec(), true); break;
        case ID_BUBBLES: g.cfg.bubbles = !g.cfg.bubbles; break;
        case ID_ROAM_TOGGLE: SetRoam(!g.cfg.roam); break;
        case ID_VOICE_TOGGLE: g.cfg.voice = !g.cfg.voice; break;
        case ID_SING_TOGGLE:
            g.cfg.sing = !g.cfg.sing;
            g.songKey.clear();  // 다시 켜면 가사를 다시 확인
            break;
        case ID_ANGRY_TOGGLE:
            g.cfg.angry = !g.cfg.angry;
            InstallMouseHook();
            break;
        case ID_CURSOR_TOGGLE:
            g.cfg.cursorGrab = !g.cfg.cursorGrab;
            g.nextCursor = NowSec() + g.cfg.cursorIntervalSec;
            break;
        case ID_VOICE_REC: ShowVoiceDialog(g.inst, &g.voices, PlayedVoice); break;
        case ID_CHAT: Agent::Open(); break;
        case ID_GEMINI_KEY: Agent::KeyDialog(); break;
        case ID_PAUSE: g.paused = !g.paused; break;
        case ID_MODE_DANCE: ApplyMode(MODE_DANCE); break;
        case ID_MODE_RAP: ApplyMode(MODE_RAP); break;
        case ID_MODE_OFF: ApplyMode(MODE_OFF); break;
        case ID_CANCEL: CancelBusy(); break;
        case ID_EXIT: DestroyWindow(g.msg); break;
        default:
            if (cmd >= ID_CHAR_BASE && cmd < UINT(ID_CHAR_BASE + kCharCount)) SwitchCharacter(kChars[cmd - ID_CHAR_BASE].id);
            else if (cmd >= ID_TARGET_BASE && cmd < ID_TARGET_BASE + g.menuTargets.size())
                BeginApproach(g.menuTargets[cmd - ID_TARGET_BASE].hwnd);
    }
}

// 다른 인스턴스(명령줄)에서 온 명령. 테스트·자동화용.
void HandleCommand(const std::wstring& c) {
    Log(L"cmd: %s", c.c_str());
    auto arg = [&](const wchar_t* p) -> const wchar_t* {
        size_t n = wcslen(p);
        return c.compare(0, n, p) == 0 ? c.c_str() + n : nullptr;
    };
    if (auto a = arg(L"drag-hwnd ")) BeginApproach(HWND(uintptr_t(wcstoull(a, nullptr, 16))));
    else if (auto t = arg(L"drag-title ")) {
        for (auto& w : ListMovableWindows(g.overlay, g.msg))
            if (w.title.find(t) != std::wstring::npos) { BeginApproach(w.hwnd); break; }
    } else if (c == L"cancel-drag") CancelBusy();
    else if (c == L"mode dance") ApplyMode(MODE_DANCE);
    else if (c == L"mode rap") ApplyMode(MODE_RAP);
    else if (c == L"mode off") ApplyMode(MODE_OFF);
    else if (c == L"pause") g.paused = !g.paused;
    else if (c == L"hide") g.visible = false;
    else if (c == L"show") g.visible = true;
    else if (c == L"device-restart") g.audio.RequestRestart();
    else if (c == L"prank now") MaybePrank(NowSec(), true);
    else if (c == L"prank notepad") StartNotepadPrank();
    else if (c == L"prank notepad-test") { if (StartNotepadPrank()) g.prankIgnoreInput = true; }
    else if (c == L"prank drag") { bool n = g.cfg.prankNotepad; g.cfg.prankNotepad = false; MaybePrank(NowSec(), true); g.cfg.prankNotepad = n; }
    else if (c == L"prank off") g.cfg.prank = PRANK_OFF;
    else if (c == L"prank sometimes") { g.cfg.prank = PRANK_SOMETIMES; ScheduleNextPrank(NowSec()); }
    else if (c == L"prank often") { g.cfg.prank = PRANK_OFTEN; ScheduleNextPrank(NowSec()); }
    else if (auto sy = arg(L"say ")) Say(sy, 2.5);
    else if (c == L"cursor now") MaybeGrabCursor(NowSec(), true);
    else if (c == L"cursor on") { g.cfg.cursorGrab = true; g.nextCursor = NowSec() + g.cfg.cursorIntervalSec; }
    else if (c == L"cursor off") g.cfg.cursorGrab = false;
    else if (c == L"voice-dialog") ShowVoiceDialog(g.inst, &g.voices, PlayedVoice);
    else if (c == L"voice-refresh") g.voices.Refresh();
    else if (c == L"chat-open") Agent::Open();
    else if (auto ch = arg(L"character ")) SwitchCharacter(ch);
    else if (auto fp = arg(L"fling ")) {  // fling x y vx vy (테스트용: 그 자리에서 이 속도로 던진 것처럼)
        double x = 0, y = 0, vx = 0, vy = 0;
        if (swscanf_s(fp, L"%lf %lf %lf %lf", &x, &y, &vx, &vy) == 4) {
            g.px = x; g.py = y; g.vx = vx; g.vy = vy; g.bounces = 0;
            g.thrown = std::hypot(vx, vy) > 600 * g.mon.scale;
            UpdateScaleFor(x, y - 2);
            SetState(State::Fall, L"(fling)");
        }
    }
    else if (c == L"roam on") SetRoam(true);
    else if (c == L"roam off") SetRoam(false);
    else if (c == L"angry") g.angryPending = true;
    else if (auto ct = arg(L"chat ")) { Agent::Open(); Agent::Submit(ct); }
    else if (auto p = arg(L"goto ")) {  // goto x y  (테스트용: 위치 지정)
        double x = 0, y = 0;
        if (swscanf_s(p, L"%lf %lf", &x, &y) == 2) { g.px = x; g.py = y; UpdateScaleFor(x, y - 2); SetState(State::Return); }
    } else if (c == L"quit") DestroyWindow(g.msg);
}

LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g.taskbarCreated && m) { AddTray(); return 0; }
    switch (m) {
        case WM_TIMER:
            if (w == TIMER_TICK) Tick();
            return 0;
        case WM_TRAY:
            if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_LBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) ShowTrayMenu();
            return 0;
        case WM_REPLAY_CLICK:
            ReplayClick();
            return 0;
        case WM_OPEN_CHAT:
            Log(L"chat: 우사기 더블클릭");
            Say(L"야하!");
            Agent::Open();
            return 0;
        case WM_HOTKEY:
            if (w == HOTKEY_CHAT) Agent::Open();
            return 0;
        case WM_COPYDATA: {
            auto* cd = reinterpret_cast<COPYDATASTRUCT*>(l);
            if (cd->dwData == COPYDATA_CMD && cd->cbData >= 2)
                HandleCommand(std::wstring(static_cast<const wchar_t*>(cd->lpData), cd->cbData / 2));
            return TRUE;
        }
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            Log(L"display: 화면 구성/작업 영역 변경");
            g.drawn.size = -1;
            return 0;
        case WM_DESTROY:
            KillTimer(h, TIMER_TICK);
            UnregisterHotKey(h, HOTKEY_CHAT);
            Agent::Shutdown();
            RemoveTray();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_NCHITTEST: return HTTRANSPARENT;
    }
    return DefWindowProcW(h, m, w, l);
}

void LoadSettings() {
    std::wstring f = SettingsPath();
    Settings& c = g.cfg;
    c.sizeDip = std::clamp(IniDouble(f, L"pet", L"sizeDip", c.sizeDip), 48.0, 512.0);
    c.walkSpeedDip = IniDouble(f, L"pet", L"walkSpeedDip", c.walkSpeedDip);
    c.dragSpeedDip = IniDouble(f, L"pet", L"dragSpeedDip", c.dragSpeedDip);
    c.dragMinDip = IniDouble(f, L"pet", L"dragMinDip", c.dragMinDip);
    c.dragMaxDip = std::max(c.dragMinDip, IniDouble(f, L"pet", L"dragMaxDip", c.dragMaxDip));
    c.gripOffsetDip = IniDouble(f, L"pet", L"gripOffsetDip", c.gripOffsetDip);
    c.hideOnFullscreen = IniInt(f, L"pet", L"hideOnFullscreen", 1) != 0;
    std::wstring mode = IniStr(f, L"music", L"mode", L"dance");
    c.mode = mode == L"rap" ? MODE_RAP : mode == L"off" ? MODE_OFF : MODE_DANCE;
    std::wstring pr = IniStr(f, L"prank", L"mode", L"sometimes");
    c.prank = pr == L"off" ? PRANK_OFF : pr == L"often" ? PRANK_OFTEN : PRANK_SOMETIMES;
    c.prankNotepad = IniInt(f, L"prank", L"notepad", 1) != 0;
    c.notepadIdleSec = IniDouble(f, L"prank", L"notepadIdleSec", c.notepadIdleSec);
    c.sleepIdleSec = IniDouble(f, L"prank", L"sleepIdleSec", c.sleepIdleSec);
    c.bubbles = IniInt(f, L"prank", L"bubbles", 1) != 0;
    c.voice = IniInt(f, L"voice", L"enabled", 1) != 0;
    c.cursorGrab = IniInt(f, L"cursor", L"enabled", 1) != 0;
    c.grabbable = IniInt(f, L"pet", L"grabbable", 1) != 0;
    c.character = IniStr(f, L"pet", L"character", L"usagi");
    c.roam = IniInt(f, L"pet", L"roam", 1) != 0;
    c.agentModels = IniStr(f, L"agent", L"models", c.agentModels.c_str());
    c.chatHotkey = IniInt(f, L"agent", L"hotkey", 1) != 0;
    c.angry = IniInt(f, L"angry", L"enabled", 1) != 0;
    c.sing = IniInt(f, L"sing", L"enabled", 1) != 0;
    c.singSpotifyOnly = IniInt(f, L"sing", L"spotifyOnly", 1) != 0;
    c.lyricsOffsetSec = std::clamp(IniDouble(f, L"sing", L"offsetSec", 0), -10.0, 10.0);
    c.angryHits = std::clamp(IniInt(f, L"angry", L"hits", 3), 3, 20);
    c.blockSec = std::clamp(IniDouble(f, L"angry", L"blockSec", 5), 0.0, 10.0);
    c.cursorIntervalSec = std::max(3.0, IniDouble(f, L"cursor", L"intervalSec", c.cursorIntervalSec));
    c.audio.thresholdDb = IniDouble(f, L"music", L"thresholdDb", c.audio.thresholdDb);
    c.audio.startSec = IniDouble(f, L"music", L"startSec", c.audio.startSec);
    c.audio.stopSec = IniDouble(f, L"music", L"stopSec", c.audio.stopSec);
    c.audio.gapSec = IniDouble(f, L"music", L"gapSec", c.audio.gapSec);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 명령줄: --log <파일>, 그 밖의 --xxx 는 실행 중인 인스턴스로 보내는 명령
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring logPath;
    std::vector<std::wstring> cmds;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--log") logPath = next();
        else if (a == L"--drag-hwnd") cmds.push_back(L"drag-hwnd " + next());
        else if (a == L"--drag-title") cmds.push_back(L"drag-title " + next());
        else if (a == L"--cancel-drag") cmds.push_back(L"cancel-drag");
        else if (a == L"--mode") cmds.push_back(L"mode " + next());
        else if (a == L"--pause") cmds.push_back(L"pause");
        else if (a == L"--hide") cmds.push_back(L"hide");
        else if (a == L"--show") cmds.push_back(L"show");
        else if (a == L"--device-restart") cmds.push_back(L"device-restart");
        else if (a == L"--prank") cmds.push_back(L"prank " + next());
        else if (a == L"--say") cmds.push_back(L"say " + next());
        else if (a == L"--voice-dialog") cmds.push_back(L"voice-dialog");
        else if (a == L"--cursor") cmds.push_back(L"cursor " + next());
        else if (a == L"--voice-refresh") cmds.push_back(L"voice-refresh");
        else if (a == L"--goto") { std::wstring x = next(); cmds.push_back(L"goto " + x + L" " + next()); }
        else if (a == L"--quit") cmds.push_back(L"quit");
        else if (a == L"--chat") cmds.push_back(L"chat " + next());
        else if (a == L"--chat-open") cmds.push_back(L"chat-open");
        else if (a == L"--character") cmds.push_back(L"character " + next());
        else if (a == L"--roam") cmds.push_back(L"roam " + next());
        else if (a == L"--fling") { std::wstring v = next(); for (int k = 0; k < 3; ++k) v += L" " + next(); cmds.push_back(L"fling " + v); }
        else if (a == L"--angry") cmds.push_back(L"angry");
        else if (a == L"--set-gemini-key" || a == L"--clear-gemini-key") {
            // 키는 이 프로세스에서 바로 암호화해 저장하고, 실행 중인 펫에는 보내지 않는다
            std::wstring k = a == L"--set-gemini-key" ? next() : L"";
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            bool ok = SaveGeminiKey(ToUtf8(k));
            SecureZeroMemory(k.data(), k.size() * sizeof(wchar_t));
            if (argc == 3 || a == L"--clear-gemini-key") { LocalFree(argv); return ok ? 0 : 2; }
        }
    }
    LocalFree(argv);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\UsagiDesktopPet.Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(kMsgClass, nullptr);
        for (auto& c : cmds) {
            COPYDATASTRUCT cd{COPYDATA_CMD, DWORD((c.size()) * sizeof(wchar_t)), (void*)c.c_str()};
            if (other) SendMessageW(other, WM_COPYDATA, 0, LPARAM(&cd));
        }
        return other ? 0 : 1;
    }
    if (!logPath.empty()) LogInit(logPath);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gsi, nullptr);
    g.inst = inst;
    LoadSettings();
    std::wstring err;
    if (!g.sprites.Load(AssetsDir(), g.cfg.character, err) && (g.cfg.character = L"usagi", !g.sprites.Load(AssetsDir(), L"usagi", err))) {
        MessageBoxW(nullptr, err.c_str(), L"우사기 데스크톱 펫", MB_ICONERROR);
        return 1;
    }
    g.icon = MakeIconFromBitmap(g.sprites.frames[ANIM_WALK][0], GetSystemMetrics(SM_CXSMICON) * 2);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MsgProc;
    wc.hInstance = inst;
    wc.lpszClassName = kMsgClass;
    wc.hIcon = g.icon;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = OverlayProc;
    wc.lpszClassName = kOverlayClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    // 트레이 메시지용 숨은 최상위 창 (TaskbarCreated 브로드캐스트를 받기 위해 message-only 아님)
    g.msg = CreateWindowExW(WS_EX_TOOLWINDOW, kMsgClass, L"UsagiDesktopPet", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    // 캐릭터 오버레이: 픽셀별 알파, 클릭 통과, 포커스 안 가져감, 작업 표시줄에 안 나옴
    g.overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                kOverlayClass, L"Usagi", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!g.msg || !g.overlay) return 1;
    g.bubble.Create(inst, kOverlayClass);
    g.fire.Create(inst, kOverlayClass);
    g.media.Start(g.cfg.singSpotifyOnly);
    g.voices.Init();
    InitAgent();
    if (g.cfg.chatHotkey && !RegisterHotKey(g.msg, HOTKEY_CHAT, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'U'))
        Log(L"chat: Ctrl+Alt+U 단축키를 등록하지 못함 (다른 앱이 사용 중)");
    g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g.msg, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    AddTray();

    // 시작 위치: 주 모니터 작업 영역 바닥 가운데
    MonInfo pm = MonFrom(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    g.px = (pm.work.left + pm.work.right) / 2.0;
    g.py = Floor(pm);
    UpdateScaleFor(g.px, g.py - 2);
    g.lastTick = NowSec();
    StartWalk(g.lastTick);
    ScheduleNextPrank(g.lastTick);
    g.nextCursor = g.lastTick + g.cfg.cursorIntervalSec;
    InstallMouseHook();
    Say(L"미야아아~", 2.5);
    if (g.cfg.mode != MODE_OFF) g.audio.Start(g.cfg.audio);
    Log(L"start: 모니터 scale=%.2f work=(%d,%d,%d,%d) size=%dpx", g.mon.scale, pm.work.left, pm.work.top, pm.work.right,
        pm.work.bottom, g.sizePx);
    for (auto& c : cmds) HandleCommand(c);

    SetTimer(g.msg, TIMER_TICK, 15, nullptr);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (Agent::PreTranslate(&m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g.mouseHook) UnhookWindowsHookEx(g.mouseHook);
    SetKeyBlock(false);
    g.media.Stop();
    g.audio.Stop();
    if (g.icon) DestroyIcon(g.icon);
    Gdiplus::GdiplusShutdown(gdipToken);
    CoUninitialize();
    ReleaseMutex(mutex);
    return 0;
}
