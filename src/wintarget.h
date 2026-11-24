#pragma once
#include "common.h"

struct TargetWindow {
    HWND hwnd;
    std::wstring title;
    std::wstring exe;
};

// 현재 화면에 보이고 정상적으로 옮길 수 있는 최상위 앱 창 목록 (Z 순서대로)
std::vector<TargetWindow> ListMovableWindows(HWND exclude1, HWND exclude2);

// 끌기 대상으로 쓸 수 있는지 (최소화·최대화·전체 화면·시스템 UI·권한 높은 창 제외)
bool IsMovableWindow(HWND h, std::wstring* why = nullptr);

// DWM 이 그리는 실제 보이는 창 테두리 (보이지 않는 리사이즈 테두리 제외)
RECT VisibleFrame(HWND h);

// 사용자가 창을 직접 옮기거나 크기를 바꾸는 중인지
bool IsInMoveSize(HWND h);
