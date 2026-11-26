#include "chatui.h"
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

namespace ChatUi {
namespace {

using Gdiplus::Color;
using Gdiplus::RectF;
using Gdiplus::PointF;

HINSTANCE g_inst{};
const wchar_t* kLogClass = L"UsagiDesktopPet.ChatLog";
const wchar_t* kDecoClass = L"UsagiDesktopPet.ChatDeco";
const wchar_t* kFace = L"Malgun Gothic";

Color Rgb(COLORREF c, BYTE a = 255) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }
const Color kBg(255, 255, 248, 243);
const Color kDots(255, 252, 228, 234);
const Color kPetFill(255, 255, 255, 255), kPetLine(255, 244, 196, 210), kPetText(255, 74, 58, 64);
const Color kUserFill(255, 255, 199, 214), kUserText(255, 92, 46, 60);
const Color kSysFill(255, 238, 232, 255), kSysText(255, 120, 106, 152);
const Color kToolFill(255, 221, 245, 236), kToolText(255, 63, 122, 99);
const Color kName(255, 214, 112, 146);
const Color kAvatarBg(255, 255, 228, 236);

float Scale(HWND h) { return GetDpiForWindow(h) / 96.0f; }

void RoundRectPath(Gdiplus::GraphicsPath& p, RectF r, float rad) {
    rad = std::min(rad, std::min(r.Width, r.Height) / 2);
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

void FillRound(Gdiplus::Graphics& g, RectF r, float rad, const Color& fill, const Color* line = nullptr, float lw = 1) {
    Gdiplus::GraphicsPath p;
    RoundRectPath(p, r, rad);
    Gdiplus::SolidBrush b(fill);
    g.FillPath(&b, &p);
    if (line) {
        Gdiplus::Pen pen(*line, lw);
        g.DrawPath(&pen, &p);
    }
}

// ---------- 캐릭터 그림 (걷기 시트의 프레임들, 모든 프레임에 같은 테두리로 잘라 둠) ----------
struct Art { std::vector<std::unique_ptr<Gdiplus::Bitmap>> frames; };
std::map<std::wstring, Art> g_art;

const Art& GetArt(const std::wstring& id) {
    auto it = g_art.find(id);
    if (it != g_art.end()) return it->second;
    Art& a = g_art[id];
    std::wstring dir = AssetsDir();
    std::wstring path = (id.empty() || id == L"usagi") ? dir + L"sprites\\walk.png" : dir + L"characters\\" + id + L"\\walk.png";
    std::unique_ptr<Gdiplus::Bitmap> sheet(Gdiplus::Bitmap::FromFile(path.c_str()));
    if (!sheet || sheet->GetLastStatus() != Gdiplus::Ok) return a;
    int W = int(sheet->GetWidth()), H = int(sheet->GetHeight()), fw = H, n = std::max(1, W / std::max(1, fw));
    int x0 = fw, y0 = H, x1 = -1, y1 = -1;
    Gdiplus::BitmapData bd;
    Gdiplus::Rect r(0, 0, W, H);
    if (sheet->LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
        for (int y = 0; y < H; ++y) {
            const BYTE* row = static_cast<const BYTE*>(bd.Scan0) + size_t(y) * bd.Stride;
            for (int x = 0; x < n * fw; ++x)
                if (row[x * 4 + 3] > 40) {
                    int lx = x % fw;
                    x0 = std::min(x0, lx); x1 = std::max(x1, lx);
                    y0 = std::min(y0, y); y1 = std::max(y1, y);
                }
        }
        sheet->UnlockBits(&bd);
    }
    if (x1 < 0) return a;
    for (int i = 0; i < n; ++i)
        a.frames.emplace_back(sheet->Clone(Gdiplus::Rect(i * fw + x0, y0, x1 - x0 + 1, y1 - y0 + 1), PixelFormat32bppPARGB));
    return a;
}

// 둥근 얼굴 아이콘 (그림 위쪽 정사각형 = 머리)
void DrawAvatar(Gdiplus::Graphics& g, const std::wstring& id, RectF r, float s) {
    Gdiplus::SolidBrush bg(kAvatarBg);
    g.FillEllipse(&bg, r);
    const Art& a = GetArt(id);
    if (!a.frames.empty()) {
        Gdiplus::Bitmap* im = a.frames[0].get();
        float iw = float(im->GetWidth()), ih = float(im->GetHeight());
        float side = std::min(iw, ih * 0.8f);
        Gdiplus::GraphicsPath clip;
        clip.AddEllipse(r);
        g.SetClip(&clip);
        RectF dst(r.X - r.Width * 0.06f, r.Y + r.Height * 0.08f, r.Width * 1.12f, r.Height * 1.12f);
        g.DrawImage(im, dst, (iw - side) / 2, 0, side, side, Gdiplus::UnitPixel);
        g.ResetClip();
    }
    Gdiplus::Pen pen(kPetLine, 2 * s);
    g.DrawEllipse(&pen, r);
}

// ---------- 대화 기록 ----------
struct Msg {
    Kind k;
    std::wstring who, id, text;
    float h = 0, bw = 0, bh = 0;
};
struct LogData {
    std::vector<Msg> msgs;
    float scroll = 0, content = 0;
    int lastW = -1;
    bool stick = true, typing = false;
    std::wstring typWho, typId;
};

LogData* Data(HWND h) { return reinterpret_cast<LogData*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

struct Fonts {
    Gdiplus::FontFamily fam{kFace};
    Gdiplus::Font body, smallFont, name;
    explicit Fonts(float s)
        : body(&fam, 13.5f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel),
          smallFont(&fam, 11.5f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel),
          name(&fam, 11.5f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel) {}
};

constexpr float M = 10, AV = 34, PX = 11, PY = 7, GAP = 10, NAMEH = 16;

void Measure(Gdiplus::Graphics& g, const Fonts& f, Msg& m, float W, float s) {
    bool isSmall = m.k == System || m.k == Tool;
    float maxText = m.k == Pet ? W - (2 * M + AV + 8 + 30) * s : m.k == User ? W * 0.74f : W * 0.86f;
    maxText = std::max(40.0f * s, maxText - 2 * PX * s);
    RectF out;
    g.MeasureString(m.text.c_str(), -1, isSmall ? &f.smallFont : &f.body, RectF(0, 0, maxText, 100000), &out);
    float px = (isSmall ? 9 : PX) * s, py = (isSmall ? 4 : PY) * s;
    m.bw = std::ceil(out.Width) + 2 * px + 1;
    m.bh = std::ceil(out.Height) + 2 * py;
    m.h = m.bh + GAP * s + (m.k == Pet ? NAMEH * s : 0);
    if (m.k == Pet) m.h = std::max(m.h, (AV + GAP) * s);
}

void DrawMsg(Gdiplus::Graphics& g, const Fonts& f, const Msg& m, float y, float W, float s, int typingPhase = -1) {
    Gdiplus::StringFormat fmt;
    bool isSmall = m.k == System || m.k == Tool;
    float px = (isSmall ? 9 : PX) * s, py = (isSmall ? 4 : PY) * s;
    if (m.k == Pet) {
        DrawAvatar(g, m.id, RectF(M * s, y, AV * s, AV * s), s);
        Gdiplus::SolidBrush nb(kName);
        g.DrawString(m.who.c_str(), -1, &f.name, PointF((M + AV + 9) * s, y - 1 * s), &nb);
        RectF b((M + AV + 8) * s, y + NAMEH * s, m.bw, m.bh);
        FillRound(g, b, 12 * s, kPetFill, &kPetLine, 1.6f * s);
        Gdiplus::SolidBrush tb(kPetText);
        if (typingPhase >= 0) {  // 통통 튀는 점 세 개
            for (int i = 0; i < 3; ++i) {
                float bounce = (typingPhase % 3 == i) ? -3 * s : 0;
                Gdiplus::SolidBrush db(kName);
                g.FillEllipse(&db, b.X + px + i * 11 * s, b.Y + b.Height / 2 - 3 * s + bounce, 6 * s, 6 * s);
            }
        } else {
            g.DrawString(m.text.c_str(), -1, &f.body, RectF(b.X + px, b.Y + py, b.Width - 2 * px + 2, b.Height), &fmt, &tb);
        }
    } else if (m.k == User) {
        RectF b(W - M * s - m.bw, y, m.bw, m.bh);
        FillRound(g, b, 12 * s, kUserFill);
        Gdiplus::SolidBrush tb(kUserText);
        g.DrawString(m.text.c_str(), -1, &f.body, RectF(b.X + px, b.Y + py, b.Width - 2 * px + 2, b.Height), &fmt, &tb);
    } else {
        RectF b((W - m.bw) / 2, y, m.bw, m.bh);
        FillRound(g, b, m.bh / 2, m.k == Tool ? kToolFill : kSysFill);
        Gdiplus::SolidBrush tb(m.k == Tool ? kToolText : kSysText);
        g.DrawString(m.text.c_str(), -1, &f.smallFont, RectF(b.X + px, b.Y + py, b.Width - 2 * px + 2, b.Height), &fmt, &tb);
    }
}

void PaintLog(HWND h, HDC dc) {
    LogData* d = Data(h);
    RECT rc;
    GetClientRect(h, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0 || !d) return;
    float s = Scale(h);
    Gdiplus::Bitmap back(W, H, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(&back);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.Clear(kBg);
    {  // 물방울 무늬
        Gdiplus::SolidBrush db(kDots);
        float step = 26 * s;
        for (float y = step / 2; y < H; y += step)
            for (float x = (int(y / step) % 2 ? step : step / 2); x < W; x += step) g.FillEllipse(&db, x, y, 3.5f * s, 3.5f * s);
    }
    Fonts f(s);
    bool all = d->lastW != W;
    d->lastW = W;
    float content = M * s;
    for (Msg& m : d->msgs) {
        if (all || m.h == 0) Measure(g, f, m, float(W), s);
        content += m.h;
    }
    Msg typ{Pet, d->typWho, d->typId, L"……"};
    if (d->typing) { Measure(g, f, typ, float(W), s); typ.bw = 48 * s; content += typ.h; }
    d->content = content;
    float maxScroll = std::max(0.0f, content - H);
    if (d->stick) d->scroll = maxScroll;
    d->scroll = std::clamp(d->scroll, 0.0f, maxScroll);
    float y = M * s - d->scroll;
    for (const Msg& m : d->msgs) {
        if (y + m.h >= 0 && y <= H) DrawMsg(g, f, m, y, float(W), s);
        y += m.h;
    }
    if (d->typing) DrawMsg(g, f, typ, y, float(W), s, int(GetTickCount() / 260));
    if (maxScroll > 0) {  // 얇은 스크롤 막대
        float th = std::max(24 * s, H * H / content), ty = (H - th) * d->scroll / maxScroll;
        FillRound(g, RectF(W - 5 * s, ty + 2 * s, 3 * s, th - 4 * s), 1.5f * s, Color(120, 244, 170, 192));
    }
    Gdiplus::Graphics out(dc);
    out.DrawImage(&back, 0, 0, W, H);
}

void CopyAll(HWND h) {
    LogData* d = Data(h);
    std::wstring t;
    for (const Msg& m : d->msgs) t += (m.k == User ? L"나: " : m.k == Pet ? m.who + L": " : L"") + m.text + L"\r\n";
    if (!OpenClipboard(h)) return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
    if (mem) {
        memcpy(GlobalLock(mem), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
}

LRESULT CALLBACK LogProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE: SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(new LogData)); return 0;
        case WM_DESTROY: delete Data(h); SetWindowLongPtrW(h, GWLP_USERDATA, 0); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            PaintLog(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_MOUSEWHEEL: {
            LogData* d = Data(h);
            RECT rc;
            GetClientRect(h, &rc);
            float maxScroll = std::max(0.0f, d->content - rc.bottom);
            d->scroll = std::clamp(d->scroll - GET_WHEEL_DELTA_WPARAM(w) / 120.0f * 60 * Scale(h), 0.0f, maxScroll);
            d->stick = d->scroll >= maxScroll - 2;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_TIMER:
            if (Data(h)->typing) InvalidateRect(h, nullptr, FALSE);
            else KillTimer(h, 1);
            return 0;
        case WM_RBUTTONUP: {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"대화 내용 복사");
            POINT p;
            GetCursorPos(&p);
            if (TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, h, nullptr) == 1) CopyAll(h);
            DestroyMenu(menu);
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------- 캐릭터 장식 창 ----------
struct Deco {
    HWND wnd{}, chat{};
    std::function<std::wstring()> cur;
    std::function<bool()> busy;
    HDC mem{};
    HBITMAP dib{}, old{};
    void* bits = nullptr;
    int w = 0, h = 0;
    DWORD t0 = 0;
} D;

void DecoTick() {
    if (!D.chat || !IsWindow(D.chat)) { ShowWindow(D.wnd, SW_HIDE); return; }
    if (!IsWindowVisible(D.chat) || IsIconic(D.chat)) {
        if (IsWindowVisible(D.wnd)) ShowWindow(D.wnd, SW_HIDE);
        return;
    }
    RECT cr;
    GetWindowRect(D.chat, &cr);
    float s = Scale(D.chat);
    int W = cr.right - cr.left, H = int(96 * s), sink = int(9 * s);
    if (W <= 0) return;
    if (W != D.w || H != D.h) {
        if (!D.mem) D.mem = CreateCompatibleDC(nullptr);
        if (D.dib) { SelectObject(D.mem, D.old); DeleteObject(D.dib); }
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB};
        D.dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &D.bits, nullptr, 0);
        D.old = HBITMAP(SelectObject(D.mem, D.dib));
        D.w = W;
        D.h = H;
    }
    Gdiplus::Bitmap bmp(W, H, PixelFormat32bppPARGB);
    {
        Gdiplus::Graphics g(&bmp);
        g.Clear(Color(0, 0, 0, 0));
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        std::wstring cur = D.cur ? D.cur() : L"usagi";
        static std::wstring lastCur;
        if (cur != lastCur) {  // 캐릭터가 바뀌면 제목도 다시 그림
            lastCur = cur;
            PostMessageW(D.chat, kMsgCharChanged, 0, 0);
        }
        static const wchar_t* all[] = {L"usagi", L"hachiware", L"chiikawa", L"rakko", L"shisa", L"momonga"};
        std::vector<std::wstring> ids{cur};
        for (auto* a : all)
            if (cur != a) ids.push_back(a);
        bool busy = D.busy && D.busy();
        double t = (GetTickCount() - D.t0) / 1000.0;
        float x = 12 * s, limit = W - 46 * s;
        for (size_t i = 0; i < ids.size(); ++i) {
            const Art& a = GetArt(ids[i]);
            if (a.frames.empty()) continue;
            bool main = i == 0;
            size_t fi = main ? size_t(t * 1000 / 130) % a.frames.size() : 0;
            Gdiplus::Bitmap* im = a.frames[fi].get();
            float ih = (main ? 86 : 58) * s, iw = ih * im->GetWidth() / im->GetHeight();
            if (x + iw > limit) break;
            double sp = busy ? 9.0 : 2.6, amp = (busy ? 7.0 : 2.5) * s;
            float bob = float(-std::abs(std::sin(t * sp + i * 1.3)) * amp);
            float y = H - ih + bob;
            if (main) {
                g.DrawImage(im, RectF(x, y, iw, ih));
            } else {  // 옆 친구들은 주인공 쪽(왼쪽)을 본다
                PointF pts[3] = {PointF(x + iw, y), PointF(x, y), PointF(x + iw, y + ih)};
                g.DrawImage(im, pts, 3);
            }
            x += iw - (main ? 2 : 8) * s;
        }
    }
    Gdiplus::BitmapData bd;
    Gdiplus::Rect r(0, 0, W, H);
    if (bmp.LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bd) == Gdiplus::Ok) {
        for (int y = 0; y < H; ++y)
            memcpy(static_cast<BYTE*>(D.bits) + size_t(y) * W * 4, static_cast<BYTE*>(bd.Scan0) + size_t(y) * bd.Stride, size_t(W) * 4);
        bmp.UnlockBits(&bd);
    }
    POINT dst{cr.left, cr.top - H + sink}, src{0, 0};
    SIZE sz{W, H};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(D.wnd, nullptr, &dst, &sz, D.mem, &src, 0, &bf, ULW_ALPHA);
    if (!IsWindowVisible(D.wnd)) ShowWindow(D.wnd, SW_SHOWNOACTIVATE);
}

LRESULT CALLBACK DecoProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER) { DecoTick(); return 0; }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

void Init(HINSTANCE inst) {
    g_inst = inst;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = LogProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kLogClass;
    RegisterClassExW(&wc);
    WNDCLASSEXW dc{sizeof(dc)};
    dc.lpfnWndProc = DecoProc;
    dc.hInstance = inst;
    dc.lpszClassName = kDecoClass;
    RegisterClassExW(&dc);
}

HWND CreateLog(HWND parent, int id) {
    return CreateWindowExW(0, kLogClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent, HMENU(INT_PTR(id)), g_inst, nullptr);
}

void Add(HWND log, Kind k, const std::wstring& who, const std::wstring& charId, const std::wstring& text) {
    LogData* d = log ? Data(log) : nullptr;
    if (!d) return;
    RECT rc;
    GetClientRect(log, &rc);
    d->stick = d->scroll >= d->content - rc.bottom - 4 * Scale(log) || k == User;
    d->msgs.push_back(Msg{k, who, charId, text});
    if (d->msgs.size() > 300) d->msgs.erase(d->msgs.begin(), d->msgs.begin() + 50);
    InvalidateRect(log, nullptr, FALSE);
}

void Clear(HWND log) {
    LogData* d = log ? Data(log) : nullptr;
    if (!d) return;
    d->msgs.clear();
    d->scroll = 0;
    d->stick = true;
    InvalidateRect(log, nullptr, FALSE);
}

void SetTyping(HWND log, bool on, const std::wstring& who, const std::wstring& charId) {
    LogData* d = log ? Data(log) : nullptr;
    if (!d) return;
    d->typing = on;
    d->typWho = who;
    d->typId = charId;
    if (on) { d->stick = true; SetTimer(log, 1, 130, nullptr); }
    InvalidateRect(log, nullptr, FALSE);
}

void AttachDeco(HWND chat, std::function<std::wstring()> currentId, std::function<bool()> busy) {
    D.chat = chat;
    D.cur = std::move(currentId);
    D.busy = std::move(busy);
    D.t0 = GetTickCount();
    if (!D.wnd)
        D.wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kDecoClass, L"", WS_POPUP, 0,
                                0, 1, 1, chat, nullptr, g_inst, nullptr);
    SetTimer(D.wnd, 1, 40, nullptr);
}

void PaintChat(HWND chat, HDC dc, const std::wstring& title, int headerH, const RECT& pill, const RECT& closeBtn, bool closeHot) {
    RECT rc;
    GetClientRect(chat, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;
    float s = Scale(chat);
    Gdiplus::Bitmap back(W, H, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(&back);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    g.Clear(kBg);
    // 머리 띠 (분홍 그라데이션 + 아래 물결)
    Gdiplus::LinearGradientBrush hb(PointF(0, 0), PointF(float(W), 0), Color(255, 255, 196, 214), Color(255, 255, 222, 206));
    g.FillRectangle(&hb, 0.0f, 0.0f, float(W), float(headerH));
    Gdiplus::SolidBrush wave(kBg);
    float r = 7 * s;
    for (float x = -r; x < W + r; x += 2 * r) g.FillEllipse(&wave, x, headerH - r, 2 * r, 2 * r);
    Gdiplus::FontFamily fam(kFace);
    Gdiplus::Font tf(&fam, 13.5f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush tb(Color(255, 122, 70, 92));
    std::wstring t = L"♡ " + title;
    g.DrawString(t.c_str(), -1, &tf, PointF(14 * s, headerH / 2.0f - 9 * s), &tb);
    // 닫기 단추
    RectF cb(float(closeBtn.left), float(closeBtn.top), float(closeBtn.right - closeBtn.left), float(closeBtn.bottom - closeBtn.top));
    Gdiplus::SolidBrush cbb(closeHot ? Color(255, 255, 150, 180) : Color(170, 255, 255, 255));
    g.FillEllipse(&cbb, cb);
    Gdiplus::Pen xp(closeHot ? Color(255, 255, 255, 255) : Color(255, 214, 112, 146), 2 * s);
    xp.SetStartCap(Gdiplus::LineCapRound);
    xp.SetEndCap(Gdiplus::LineCapRound);
    float c = cb.Width * 0.3f;
    g.DrawLine(&xp, cb.X + c, cb.Y + c, cb.GetRight() - c, cb.GetBottom() - c);
    g.DrawLine(&xp, cb.GetRight() - c, cb.Y + c, cb.X + c, cb.GetBottom() - c);
    // 입력칸 알약
    RectF pr(float(pill.left), float(pill.top), float(pill.right - pill.left), float(pill.bottom - pill.top));
    FillRound(g, pr, pr.Height / 2, Color(255, 255, 255, 255), &kPetLine, 1.6f * s);
    Gdiplus::Graphics out(dc);
    out.DrawImage(&back, 0, 0, W, H);
}

void DrawButton(const DRAWITEMSTRUCT* d, COLORREF fill) {
    RECT rc = d->rcItem;
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    float s = Scale(d->hwndItem);
    Gdiplus::Bitmap back(W, H, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(&back);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    g.Clear(kBg);
    Color c = Rgb(fill);
    if (d->itemState & ODS_DISABLED) c = Color(255, 226, 220, 223);
    else if (d->itemState & ODS_SELECTED) c = Color(255, BYTE(c.GetR() * 0.86), BYTE(c.GetG() * 0.86), BYTE(c.GetB() * 0.86));
    float press = (d->itemState & ODS_SELECTED) ? 1.5f * s : 0;
    // 아래 그림자 → 말랑한 단추
    FillRound(g, RectF(1, 3 * s, W - 2.0f, H - 4 * s), (H - 4 * s) / 2,
              Color(255, BYTE(c.GetR() * 0.82), BYTE(c.GetG() * 0.82), BYTE(c.GetB() * 0.82)));
    FillRound(g, RectF(1, 1 + press, W - 2.0f, H - 4 * s), (H - 4 * s) / 2, c);
    wchar_t text[64] = {};
    GetWindowTextW(d->hwndItem, text, 64);
    Gdiplus::FontFamily fam(kFace);
    Gdiplus::Font f(&fam, 12.5f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::SolidBrush tb(Color(255, 255, 255, 255));
    g.DrawString(text, -1, &f, RectF(0, 1 + press, float(W), H - 4 * s), &fmt, &tb);
    Gdiplus::Graphics out(d->hDC);
    out.DrawImage(&back, rc.left, rc.top, W, H);
}

void RoundCorners(HWND w) {
    DWORD pref = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(w, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &pref, sizeof(pref));
}

}  // namespace ChatUi
