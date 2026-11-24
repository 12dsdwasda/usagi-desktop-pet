#include "sprites.h"
#include <wincodec.h>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "windowscodecs.lib")

static bool LoadPng(const std::wstring& path, Bitmap& out) {
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))))
        return false;
    bool ok = false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frm = nullptr;
    IWICBitmapSource* conv = nullptr;
    if (SUCCEEDED(fac->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frm)) &&
        SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, frm, &conv))) {
        UINT w, h;
        conv->GetSize(&w, &h);
        out.w = int(w);
        out.h = int(h);
        out.px.resize(size_t(w) * h);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(out.px.size() * 4), reinterpret_cast<BYTE*>(out.px.data())));
    }
    if (conv) conv->Release();
    if (frm) frm->Release();
    if (dec) dec->Release();
    fac->Release();
    return ok;
}

static std::vector<int> ParseInts(const std::wstring& s) {
    std::vector<int> v;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(L',', i);
        if (j == std::wstring::npos) j = s.size();
        if (j > i) v.push_back(_wtoi(s.substr(i, j - i).c_str()));
        i = j + 1;
    }
    return v;
}

bool SpriteSet::Load(const std::wstring& assetsDir, std::wstring& err) {
    const std::wstring ini = assetsDir + L"usagi.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = L"에셋 파일이 없습니다: " + ini;
        return false;
    }
    frameW = IniInt(ini, L"sprites", L"frameW", 384);
    frameH = IniInt(ini, L"sprites", L"frameH", 384);
    baseY = IniInt(ini, L"sprites", L"baseY", 344);
    gripX = IniInt(ini, L"drag", L"gripX", 178);
    gripY = IniInt(ini, L"drag", L"gripY", 274);
    const wchar_t* names[ANIM_COUNT] = {L"walk", L"dance", L"rap", L"drag"};
    for (int a = 0; a < ANIM_COUNT; ++a) {
        AnimMeta& m = meta[a];
        m.durationsMs = ParseInts(IniStr(ini, names[a], L"durations", L"150,150,150,150"));
        m.anchorX = IniInt(ini, names[a], L"anchorX", frameW / 2);
        m.left = IniInt(ini, names[a], L"left", 0);
        m.right = IniInt(ini, names[a], L"right", frameW);
        Bitmap sheet;
        std::wstring path = assetsDir + L"sprites\\" + names[a] + L".png";
        if (!LoadPng(path, sheet)) {
            err = L"스프라이트를 읽지 못했습니다: " + path;
            return false;
        }
        int count = sheet.w / frameW;
        if (count <= 0 || sheet.h < frameH) {
            err = L"스프라이트 크기가 잘못되었습니다: " + path;
            return false;
        }
        frames[a].clear();
        for (int f = 0; f < count; ++f) {
            Bitmap b;
            b.w = frameW;
            b.h = frameH;
            b.px.resize(size_t(frameW) * frameH);
            for (int y = 0; y < frameH; ++y)
                std::copy_n(&sheet.px[size_t(y) * sheet.w + size_t(f) * frameW], frameW, &b.px[size_t(y) * frameW]);
            frames[a].push_back(std::move(b));
        }
        m.durationsMs.resize(frames[a].size(), m.durationsMs.empty() ? 150 : m.durationsMs.back());
        m.top = frameH;
        for (const Bitmap& b : frames[a])
            for (int y = 0; y < m.top; ++y)
                for (int x = 0; x < b.w; ++x)
                    if ((b.px[size_t(y) * b.w + x] >> 24) > 32) { m.top = y; break; }
    }
    return true;
}

// 분리형 리샘플 (축소: 면적 가중 삼각 필터, 확대: 선형). premultiplied 이므로 채널별 평균이 올바르다.
static void ResampleAxis(const float* src, int sw, int sh, float* dst, int dw, bool horizontal) {
    // horizontal: 각 행에서 sw->dw, vertical: 각 열에서 sh->dh(=dw)
    const int srcLen = horizontal ? sw : sh;
    const int lines = horizontal ? sh : sw;
    const double scale = double(dw) / srcLen;
    const double support = scale < 1.0 ? 1.0 / scale : 1.0;
    struct Tap { int i; float w; };
    std::vector<std::vector<Tap>> taps(dw);
    for (int d = 0; d < dw; ++d) {
        double c = (d + 0.5) / scale - 0.5;
        int i0 = int(std::floor(c - support)), i1 = int(std::ceil(c + support));
        double sum = 0;
        for (int i = i0; i <= i1; ++i) {
            double w = 1.0 - std::abs(i - c) / support;
            if (w <= 0) continue;
            int ii = std::clamp(i, 0, srcLen - 1);
            taps[d].push_back({ii, float(w)});
            sum += w;
        }
        for (auto& t : taps[d]) t.w = float(t.w / sum);
    }
    for (int l = 0; l < lines; ++l) {
        for (int d = 0; d < dw; ++d) {
            float acc[4] = {0, 0, 0, 0};
            for (const Tap& t : taps[d]) {
                const float* p = horizontal ? &src[(size_t(l) * sw + t.i) * 4] : &src[(size_t(t.i) * sw + l) * 4];
                for (int k = 0; k < 4; ++k) acc[k] += p[k] * t.w;
            }
            float* q = horizontal ? &dst[(size_t(l) * dw + d) * 4] : &dst[(size_t(d) * sw + l) * 4];
            for (int k = 0; k < 4; ++k) q[k] = acc[k];
        }
    }
}

Bitmap Resample(const Bitmap& src, int w, int h) {
    std::vector<float> a(size_t(src.w) * src.h * 4);
    for (size_t i = 0; i < src.px.size(); ++i) {
        uint32_t p = src.px[i];
        a[i * 4 + 0] = float(p & 0xFF);
        a[i * 4 + 1] = float((p >> 8) & 0xFF);
        a[i * 4 + 2] = float((p >> 16) & 0xFF);
        a[i * 4 + 3] = float(p >> 24);
    }
    std::vector<float> b(size_t(w) * src.h * 4);
    ResampleAxis(a.data(), src.w, src.h, b.data(), w, true);
    std::vector<float> c(size_t(w) * h * 4);
    ResampleAxis(b.data(), w, src.h, c.data(), h, false);
    Bitmap out;
    out.w = w;
    out.h = h;
    out.px.resize(size_t(w) * h);
    for (size_t i = 0; i < out.px.size(); ++i) {
        auto cl = [](float v) { return uint32_t(std::clamp(int(v + 0.5f), 0, 255)); };
        uint32_t A = cl(c[i * 4 + 3]);
        uint32_t B = std::min(cl(c[i * 4 + 0]), A), G = std::min(cl(c[i * 4 + 1]), A), R = std::min(cl(c[i * 4 + 2]), A);
        out.px[i] = B | (G << 8) | (R << 16) | (A << 24);
    }
    return out;
}

Bitmap FlipH(const Bitmap& src) {
    Bitmap out = src;
    for (int y = 0; y < src.h; ++y)
        std::reverse(out.px.begin() + size_t(y) * src.w, out.px.begin() + size_t(y + 1) * src.w);
    return out;
}

const Bitmap& ScaledCache::Get(const SpriteSet& s, int anim, int frame, bool flip, int sizePx) {
    if (sizePx != sizePx_) {
        for (auto& v : cache_) v.clear();
        sizePx_ = sizePx;
    }
    auto& v = cache_[anim * 2 + (flip ? 1 : 0)];
    if (v.empty()) {
        for (const Bitmap& b : s.frames[anim]) {
            Bitmap r = Resample(b, sizePx, sizePx);
            v.push_back(flip ? FlipH(r) : std::move(r));
        }
    }
    return v[size_t(frame) % v.size()];
}

HICON MakeIconFromBitmap(const Bitmap& src, int size) {
    // 캐릭터 영역만 잘라 정사각형 아이콘으로
    int x0 = src.w, y0 = src.h, x1 = 0, y1 = 0;
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x)
            if ((src.px[size_t(y) * src.w + x] >> 24) > 32) {
                x0 = std::min(x0, x); x1 = std::max(x1, x);
                y0 = std::min(y0, y); y1 = std::max(y1, y);
            }
    if (x1 <= x0) return nullptr;
    int side = std::max(x1 - x0, y1 - y0) + 8;
    int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    Bitmap sq;
    sq.w = sq.h = side;
    sq.px.assign(size_t(side) * side, 0);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) {
            int sx = cx - side / 2 + x, sy = cy - side / 2 + y;
            if (sx >= 0 && sy >= 0 && sx < src.w && sy < src.h) sq.px[size_t(y) * side + x] = src.px[size_t(sy) * src.w + sx];
        }
    Bitmap ic = Resample(sq, size, size);
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    // 아이콘은 straight alpha 를 기대하므로 un-premultiply
    auto* d = static_cast<uint32_t*>(bits);
    for (size_t i = 0; i < ic.px.size(); ++i) {
        uint32_t p = ic.px[i], A = p >> 24;
        if (A == 0) { d[i] = 0; continue; }
        uint32_t B = std::min(255u, (p & 0xFF) * 255 / A), G = std::min(255u, ((p >> 8) & 0xFF) * 255 / A),
                 R = std::min(255u, ((p >> 16) & 0xFF) * 255 / A);
        d[i] = B | (G << 8) | (R << 16) | (A << 24);
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return h;
}
