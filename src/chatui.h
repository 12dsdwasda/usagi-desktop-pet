#pragma once
#include "common.h"
#include <functional>

// 귀여운 대화창 꾸미기: 말풍선 대화 기록, 둥근 버튼, 창 위에 올라앉은 캐릭터들.
// GDI+ 는 main 에서 이미 시작돼 있어야 한다. 모두 UI 스레드에서 호출한다.
namespace ChatUi {

enum Kind { User = 0, Pet, System, Tool };

// 색 (COLORREF)
constexpr COLORREF kWindowBg = RGB(255, 248, 243);
constexpr COLORREF kSendColor = RGB(255, 150, 180);
constexpr COLORREF kNewColor = RGB(140, 212, 186);
constexpr COLORREF kKeyColor = RGB(190, 172, 240);

// 캐릭터가 바뀌면 대화창에 보내는 메시지 (제목·입력칸 안내 갱신)
constexpr UINT kMsgCharChanged = WM_APP + 41;

void Init(HINSTANCE inst);

// 말풍선 대화 기록 (스크롤은 휠, 오른쪽 클릭 = 대화 내용 복사)
HWND CreateLog(HWND parent, int id);
void Add(HWND log, Kind k, const std::wstring& who, const std::wstring& charId, const std::wstring& text);
void Clear(HWND log);
void SetTyping(HWND log, bool on, const std::wstring& who, const std::wstring& charId);  // "…" 입력 중 말풍선

// 대화창 위에 캐릭터들이 올라앉아 있는 장식 창 (클릭은 통과)
void AttachDeco(HWND chat, std::function<std::wstring()> currentId, std::function<bool()> busy);

// 대화창 그리기 도우미
void PaintChat(HWND chat, HDC dc, const std::wstring& title, int headerH, const RECT& inputPill, const RECT& closeBtn, bool closeHot);
void DrawButton(const DRAWITEMSTRUCT* d, COLORREF fill);
void RoundCorners(HWND w);  // Windows 11 둥근 모서리

}  // namespace ChatUi
