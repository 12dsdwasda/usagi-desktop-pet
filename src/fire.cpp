#include "fire.h"
#include <algorithm>
#include <cmath>

bool Fire::Create(HINSTANCE inst, const wchar_t* cls) {
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, cls, L"UsagiFire",
                            WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    return hwnd_ != nullptr;
}

void Fire::Update(double now, POINT c, double sc, bool burning) {
    if (!hwnd_) return;
    double dt = last_ > 0 ? std::clamp(now - last_, 0.0, 0.05) : 0.0;
    last_ = now;
    auto U = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng_); };

    if (burning) {
        // 화살표 포인터 모양(끝점에서 오른쪽 아래로 퍼지는 삼각형) 안에서 불꽃이 피어오른다
        spawnAcc_ += dt * 480;
        while (spawnAcc_ >= 1) {
            spawnAcc_ -= 1;
            double py = U(-4, 26), px = U(-6, std::max(-5.0, py * 0.75 + 4));
            bool spark = U(0, 1) < 0.08;  // 가끔 튀는 불티
            if (spark)
                ps_.push_back({c.x + px * sc, c.y + py * sc, U(-120, 120) * sc, U(-380, -200) * sc, now, U(0.4, 0.8), U(2.5, 4) * sc});
            else
                ps_.push_back({c.x + px * sc, c.y + py * sc, U(-40, 40) * sc, U(-260, -90) * sc, now, U(0.35, 0.8), U(9, 17) * sc});
        }
    } else {
        spawnAcc_ = 0;
    }
    for (auto& p : ps_) {
        p.vy -= 90 * sc * dt;  // 뜨거운 공기는 위로
        p.vx *= std::pow(0.3, dt);
        p.x += p.vx * dt;
        p.y += p.vy * dt;
    }
    ps_.erase(std::remove_if(ps_.begin(), ps_.end(), [&](const P& p) { return now - p.born >= p.life; }), ps_.end());

    if (ps_.empty()) {
        if (shown_) { ShowWindow(hwnd_, SW_HIDE); shown_ = false; }
        return;
    }

    int W = int(280 * sc), H = int(340 * sc);
    if (W != w_ || H != h_) {
        if (!dc_) dc_ = CreateCompatibleDC(nullptr);
        if (dib_) { SelectObject(dc_, old_); DeleteObject(dib_); }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        dib_ = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        old_ = HBITMAP(SelectObject(dc_, dib_));
        bits_ = static_cast<uint32_t*>(bits);
        w_ = W;
        h_ = H;
        acc_.assign(size_t(W) * H * 4, 0.f);
    }
    // 창은 포인터를 따라다니고(포인터가 아래쪽 1/4 지점), 불꽃은 화면 좌표로 남아 꼬리가 생긴다
    int ox = c.x - W / 2, oy = c.y - H * 3 / 4;
    std::fill(acc_.begin(), acc_.end(), 0.f);
    for (auto& p : ps_) {
        double a = (now - p.born) / p.life;  // 0..1
        double rad = p.r * (1.0 - 0.55 * a);
        float cr, cg, cb;
        if (a < 0.18) { cr = 1.f; cg = 0.97f; cb = 0.7f; }        // 하얀 노랑 심지
        else if (a < 0.45) { cr = 1.f; cg = 0.62f; cb = 0.12f; }  // 주황
        else { cr = 0.95f; cg = 0.25f; cb = 0.05f; }              // 빨강
        float alpha = float(std::pow(1.0 - a, 1.2) * 0.75);
        double x = p.x - ox, y = p.y - oy;
        int x0 = std::max(0, int(x - rad)), x1 = std::min(W - 1, int(x + rad) + 1);
        int y0 = std::max(0, int(y - rad)), y1 = std::min(H - 1, int(y + rad) + 1);
        double inv = 1.0 / (rad * rad);
        for (int yy = y0; yy <= y1; ++yy) {
            float* row = &acc_[size_t(yy) * W * 4];
            double dy2 = (yy - y) * (yy - y);
            for (int xx = x0; xx <= x1; ++xx) {
                double d2 = ((xx - x) * (xx - x) + dy2) * inv;
                if (d2 >= 1) continue;
                float w = float((1 - d2) * (1 - d2)) * alpha;
                float* q = row + xx * 4;
                q[0] += cr * w;
                q[1] += cg * w;
                q[2] += cb * w;
                q[3] += w;
            }
        }
    }
    for (size_t i = 0, n = size_t(W) * H; i < n; ++i) {
        const float* q = &acc_[i * 4];
        float A = std::min(1.f, q[3]);
        auto ch = [&](float v) { return uint32_t(std::lround(std::min(A, v) * 255)); };
        bits_[i] = (uint32_t(std::lround(A * 255)) << 24) | (ch(q[0]) << 16) | (ch(q[1]) << 8) | ch(q[2]);
    }
    POINT dst{ox, oy}, src{0, 0};
    SIZE sz{W, H};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, nullptr, &dst, &sz, dc_, &src, 0, &bf, ULW_ALPHA);
    if (!shown_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        shown_ = true;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
