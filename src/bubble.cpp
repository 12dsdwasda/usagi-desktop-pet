#include "bubble.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

bool Bubble::Create(HINSTANCE inst, const wchar_t* cls) {
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, cls,
                            L"UsagiBubble", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    return hwnd_ != nullptr;
}

void Bubble::Say(const std::wstring& text, double seconds) {
    text_ = text;
    start_ = 0;  // 다음 Update 에서 시작 시각 기록
    until_ = seconds;
    dirty_ = true;
}

void Bubble::RenderBitmap(double scale) {
    const float s = float(scale);
    const float pad = 9 * s, tail = 9 * s, stroke = 2.2f * s, radius = 11 * s;
    FontFamily fam(L"Malgun Gothic");
    Font font(&fam, 14.0f * s, FontStyleBold, UnitPixel);
    RectF box;
    {
        Bitmap probe(1, 1, PixelFormat32bppPARGB);
        Graphics g(&probe);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        // 긴 말은 여러 줄로 감싼다
        g.MeasureString(text_.c_str(), -1, &font, RectF(0, 0, 300 * s, 4000 * s), nullptr, &box);
        box.Width = std::ceil(box.Width) + 1;
        box.Height = std::ceil(box.Height);
    }
    int w = int(std::ceil(box.Width + 2 * pad + 2 * stroke));
    int h = int(std::ceil(box.Height + 2 * pad + tail + 2 * stroke));
    Bitmap bmp(w, h, PixelFormat32bppPARGB);
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(0, 0, 0, 0));
        float x0 = stroke, y0 = stroke, x1 = w - stroke, y1 = h - stroke - tail;
        float d = radius * 2, cx = w / 2.0f;
        GraphicsPath path;
        path.AddArc(x0, y0, d, d, 180, 90);
        path.AddArc(x1 - d, y0, d, d, 270, 90);
        path.AddArc(x1 - d, y1 - d, d, d, 0, 90);
        path.AddLine(cx + tail * 0.8f, y1, cx, y1 + tail);  // 아래쪽 꼬리
        path.AddLine(cx, y1 + tail, cx - tail * 0.8f, y1);
        path.AddArc(x0, y1 - d, d, d, 90, 90);
        path.CloseFigure();
        SolidBrush fill(Color(248, 255, 253, 246));
        Pen pen(Color(255, 40, 30, 26), stroke);
        pen.SetLineJoin(LineJoinRound);
        g.FillPath(&fill, &path);
        g.DrawPath(&pen, &path);
        SolidBrush ink(Color(255, 40, 30, 26));
        g.DrawString(text_.c_str(), -1, &font, RectF(stroke + pad, stroke + pad, box.Width, box.Height), nullptr, &ink);
    }
    if (!memDC_) memDC_ = CreateCompatibleDC(nullptr);
    if (dib_) { SelectObject(memDC_, old_); DeleteObject(dib_); }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    dib_ = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    old_ = HBITMAP(SelectObject(memDC_, dib_));
    BitmapData bd;
    Rect r(0, 0, w, h);
    if (bmp.LockBits(&r, ImageLockModeRead, PixelFormat32bppPARGB, &bd) == Ok) {
        for (int y = 0; y < h; ++y)
            memcpy(static_cast<BYTE*>(bits) + size_t(y) * w * 4, static_cast<BYTE*>(bd.Scan0) + size_t(y) * bd.Stride, size_t(w) * 4);
        bmp.UnlockBits(&bd);
    }
    w_ = w;
    h_ = h;
    renderedScale_ = scale;
    dirty_ = false;
}

void Bubble::Update(double now, double ax, double ay, double scale, const RECT& work, bool show) {
    if (!hwnd_) return;
    if (dirty_ && start_ == 0) {
        start_ = now;
        until_ = now + until_;
    }
    if (text_.empty() || now >= until_ || !show) {
        if (shown_) { ShowWindow(hwnd_, SW_HIDE); shown_ = false; }
        if (!text_.empty() && now >= until_) text_.clear();
        return;
    }
    if (dirty_ || std::abs(renderedScale_ - scale) > 0.01) RenderBitmap(scale);
    // 나타날 때 살짝 튀어 오르고, 사라질 때 흐려진다
    double age = now - start_, left = until_ - now;
    double alpha = std::clamp(std::min(age / 0.12, left / 0.25), 0.0, 1.0);
    double rise = (1.0 - std::min(1.0, age / 0.15)) * 6 * scale;
    LONG x = LONG(std::lround(ax - w_ / 2.0));
    LONG y = LONG(std::lround(ay - h_ + rise));
    x = std::clamp<LONG>(x, work.left + 2, std::max<LONG>(work.left + 2, work.right - w_ - 2));
    y = std::max<LONG>(y, work.top + 2);
    POINT dst{x, y}, src{0, 0};
    SIZE sz{w_, h_};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, BYTE(std::lround(alpha * 255)), AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, nullptr, &dst, &sz, memDC_, &src, 0, &bf, ULW_ALPHA);
    if (!shown_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        shown_ = true;
    }
}
