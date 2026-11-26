#pragma once
#include "common.h"
#include <functional>

// 우사기 에이전트: Gemini 와 대화하고, 정해진 도구(웹 열기, 앱 실행, 타이머, 창 끌기 등)로 일을 한다.
// 모든 함수는 메인(UI) 스레드에서 호출한다. HTTP 요청만 작업 스레드에서 돈다.
struct AgentHost {
    std::function<void(const std::wstring& text, double seconds)> say;  // 말풍선(+녹음한 목소리)
    std::function<void(bool on)> thinking;                              // 생각 중 표시
    std::function<POINT()> anchor;                                      // 대화창을 띄울 기준점(우사기 머리 위)
    std::function<std::wstring()> status;                               // 우사기 상태 설명
    std::function<std::vector<std::wstring>()> listWindows;             // 끌 수 있는 창 제목
    std::function<std::wstring(const std::wstring& title)> dragWindow;  // 결과 설명
    std::function<std::wstring(const std::wstring& mode)> setMusicMode;
    std::function<std::wstring(const std::wstring& text)> writeNote;
    std::function<std::wstring(const std::wstring& action)> petAction;
    std::function<std::wstring()> nowPlaying;                           // 스포티파이 지금 재생 중
    std::function<std::wstring(const std::wstring& cmd)> spotifyControl;
    std::function<std::wstring()> name;     // 지금 캐릭터 이름 (예: 우사기)
    std::function<std::wstring()> charId;   // 지금 캐릭터 id (예: usagi) — 대화창 그림
    std::function<std::wstring()> persona;  // "작은 토끼 캐릭터 '우사기'. 말버릇 …" 같은 소개와 말투
};

namespace Agent {
void Init(HINSTANCE inst, const AgentHost& host, const std::vector<std::string>& models);
void Open();                             // 대화창 열기 (앞으로 가져옴)
void Submit(const std::wstring& text);   // 대화창에 쓴 것처럼 보내기
void KeyDialog();                        // API 키 입력 창
bool PreTranslate(MSG* m);               // 메시지 루프에서 호출 (Enter/Esc/Tab)
HWND ChatWindow();
void Shutdown();
}  // namespace Agent
