#pragma once
#include "common.h"

// 우사기 머리 위 말풍선. 별도의 layered window (클릭 통과, 포커스 안 가져감)
class Bubble {
public:
    bool Create(HINSTANCE inst, const wchar_t* cls);
    void Say(const std::wstring& text, double seconds);
    // anchorX, anchorY: 말풍선 꼬리 끝(머리 위) 위치, 물리 픽셀
    void Update(double now, double anchorX, double anchorY, double scale, const RECT& work, bool show);
    bool Active(double now) const { return !text_.empty() && now < until_; }
    const std::wstring& Text() const { return text_; }

private:
    void RenderBitmap(double scale);
    HWND hwnd_{};
    std::wstring text_;
    double start_ = 0, until_ = 0, renderedScale_ = 0;
    bool dirty_ = false, shown_ = false;
    HDC memDC_{};
    HBITMAP dib_{}, old_{};
    int w_ = 0, h_ = 0;
};
