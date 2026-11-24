#include "voice.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "winmm.lib")

// ---------------- 표현 목록 ----------------
const std::vector<Phrase>& Phrases() {
    static const std::vector<Phrase> v = {
        {"yaha", L"야하!", nullptr, 3},        {"aha", L"아하!", "yaha", 3},
        {"ura", L"우라!", nullptr, 3},         {"uraura", L"우라우라", "ura", 3},
        {"puru", L"푸루!", nullptr, 3},        {"puruya", L"푸루야~", "puru", 3},
        {"haa", L"하아?", nullptr, 2},         {"haahaa", L"하아? 하아?", "haa", 2},
        {"miyaaa", L"미야아아~", nullptr, 3},  {"miyayaya", L"미야야야~", "miyaaa", 3},
        {"jururi", L"쥬르리…", nullptr, 5},    {"ii", L"이~", nullptr, 3},
        {"guuguu", L"구우구우~", nullptr, 20},
    };
    return v;
}

// 한글 음절만 남겨 비교 ("하아? 하아?" → "하아하아", "푸루~" → "푸루")
static std::wstring Normalize(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s)
        if (c >= 0xAC00 && c <= 0xD7A3) o += c;
    return o;
}

static int IndexOf(const char* id) {
    const auto& p = Phrases();
    for (size_t i = 0; i < p.size(); ++i)
        if (!strcmp(p[i].id, id)) return int(i);
    return -1;
}

static std::wstring Widen(const char* s) { return std::wstring(s, s + strlen(s)); }

void VoiceBank::Init() {
    PWSTR base = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        dir_ = std::wstring(base) + L"\\UsagiDesktopPet\\voices\\";
        CoTaskMemFree(base);
    } else {
        dir_ = ExeDir() + L"voices\\";
    }
    SHCreateDirectoryExW(nullptr, dir_.c_str(), nullptr);
    lastPlayed_.assign(Phrases().size(), -1000);
    Refresh();
}

std::wstring VoiceBank::PathFor(const char* id) const { return dir_ + Widen(id) + L".wav"; }

static double WavDuration(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return 0;
    unsigned char h[44] = {};
    size_t n = fread(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    if (n < 44 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return 0;
    unsigned byteRate = h[28] | (h[29] << 8) | (h[30] << 16) | (unsigned(h[31]) << 24);
    return byteRate ? double(size - 44) / byteRate : 0;
}

void VoiceBank::Refresh() {
    const auto& p = Phrases();
    durations_.assign(p.size(), 0);
    for (size_t i = 0; i < p.size(); ++i) durations_[i] = WavDuration(PathFor(p[i].id));
}

double VoiceBank::Duration(const char* id) const {
    int i = IndexOf(id);
    return i < 0 ? 0 : durations_[i];
}

double VoiceBank::PlayId(const char* id) {
    int i = IndexOf(id);
    if (i < 0 || durations_[i] <= 0) return 0;
    if (!PlaySoundW(PathFor(id).c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) return 0;
    return durations_[i];
}

double VoiceBank::Play(const std::wstring& bubbleText, double now) {
    std::wstring key = Normalize(bubbleText);
    const auto& p = Phrases();
    int i = -1;
    for (size_t k = 0; k < p.size(); ++k)
        if (Normalize(p[k].text) == key) { i = int(k); break; }
    if (i < 0) return 0;
    if (now - lastPlayed_[i] < p[i].cooldownSec) return 0;
    // 녹음이 없으면 비슷한 표현으로 (하아? 하아? → 하아?)
    int use = i;
    while (use >= 0 && durations_[use] <= 0) use = p[use].fallback ? IndexOf(p[use].fallback) : -1;
    if (use < 0) return 0;
    double d = PlayId(p[use].id);
    if (d > 0) {
        lastPlayed_[i] = now;
        Log(L"voice: '%s' 재생 (%s.wav, %.1f초)", p[i].text, Widen(p[use].id).c_str(), d);
    }
    return d;
}

// ---------------- 녹음 ----------------
namespace {
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
}  // namespace

void Recorder::Start(double maxSec) {
    Cancel();
    stop_ = false;
    finished_ = false;
    busy_ = true;
    elapsed_ = 0;
    level_ = -90;
    {
        std::lock_guard<std::mutex> lk(mu_);
        err_.clear();
        pcm_.clear();
    }
    th_ = std::thread([this, maxSec] { Run(maxSec); });
}

void Recorder::Cancel() {
    stop_ = true;
    if (th_.joinable()) th_.join();
    finished_ = false;
    busy_ = false;
}

bool Recorder::Finished() {
    if (!finished_) return false;
    if (th_.joinable()) th_.join();
    finished_ = false;
    return true;
}

std::wstring Recorder::Error() {
    std::lock_guard<std::mutex> lk(mu_);
    return err_;
}

void Recorder::Run(double maxSec) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* cap = nullptr;
    WAVEFORMATEX* wf = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&wf);
    if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, wf, nullptr);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&cap));
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) {
        std::lock_guard<std::mutex> lk(mu_);
        if (hr == E_ACCESSDENIED)
            err_ = L"마이크 접근이 막혀 있습니다. Windows 설정 > 개인 정보 및 보안 > 마이크에서 "
                   L"'데스크톱 앱이 마이크에 액세스하도록 허용'을 켜 주세요.";
        else if (hr == HRESULT(0x80070490) /*E_NOTFOUND*/)
            err_ = L"녹음 장치(마이크)를 찾지 못했습니다.";
        else {
            wchar_t b[96];
            swprintf_s(b, L"마이크를 열지 못했습니다 (0x%08X).", unsigned(hr));
            err_ = b;
        }
        Log(L"record: 실패 hr=0x%08X", unsigned(hr));
    } else {
        bool isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                       (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wf)->SubFormat == kSubtypeFloat);
        int bits = wf->wBitsPerSample, ch = wf->nChannels;
        rate_ = int(wf->nSamplesPerSec);
        Log(L"record: 시작 %d Hz %d ch %d bit", rate_, ch, bits);
        double t0 = NowSec();
        std::vector<float> local;
        while (!stop_) {
            double el = NowSec() - t0;
            elapsed_ = el;
            if (el >= maxSec) break;
            UINT32 packet = 0;
            if (FAILED(cap->GetNextPacketSize(&packet))) break;
            if (!packet) { Sleep(10); continue; }
            BYTE* data;
            UINT32 frames;
            DWORD flags;
            if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            float peak = 0;
            for (UINT32 i = 0; i < frames; ++i) {
                float s = 0;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    for (int c = 0; c < ch; ++c) {
                        const BYTE* p = data + (size_t(i) * ch + c) * (bits / 8);
                        if (isFloat && bits == 32) s += *reinterpret_cast<const float*>(p);
                        else if (bits == 16) s += *reinterpret_cast<const int16_t*>(p) / 32768.0f;
                        else if (bits == 32) s += *reinterpret_cast<const int32_t*>(p) / 2147483648.0f;
                        else if (bits == 24) s += ((int32_t(p[0]) << 8 | int32_t(p[1]) << 16 | int32_t(p[2]) << 24) >> 8) / 8388608.0f;
                    }
                    s /= ch;
                }
                local.push_back(s);
                peak = std::max(peak, std::abs(s));
            }
            cap->ReleaseBuffer(frames);
            level_ = float(20 * std::log10(std::max(peak, 1e-5f)));
        }
        client->Stop();
        std::lock_guard<std::mutex> lk(mu_);
        pcm_ = std::move(local);
        Log(L"record: 끝 %.2f초", double(pcm_.size()) / rate_);
    }
    if (wf) CoTaskMemFree(wf);
    SafeRelease(cap);
    SafeRelease(client);
    SafeRelease(dev);
    SafeRelease(en);
    CoUninitialize();
    busy_ = false;
    finished_ = true;
}

bool Recorder::SaveResult(const std::wstring& path, double& durationOut, std::wstring& why) {
    std::vector<float> x;
    {
        std::lock_guard<std::mutex> lk(mu_);
        x = pcm_;
    }
    if (x.empty()) { why = L"녹음된 소리가 없습니다."; return false; }
    // 20ms 창의 RMS 로 말소리 구간 찾기
    const int win = std::max(1, rate_ / 50);
    int first = -1, last = -1;
    float maxRms = 0;
    for (size_t i = 0; i + win <= x.size(); i += win) {
        double e = 0;
        for (int k = 0; k < win; ++k) e += double(x[i + k]) * x[i + k];
        float rms = float(std::sqrt(e / win));
        maxRms = std::max(maxRms, rms);
        if (rms > 0.01f) {  // 약 -40 dBFS
            if (first < 0) first = int(i);
            last = int(i + win);
        }
    }
    if (first < 0) { why = L"소리가 너무 작아요. 마이크에 조금 더 가까이 말해 주세요."; return false; }
    int pad = rate_ * 8 / 100;  // 앞뒤 80ms 여유
    first = std::max(0, first - pad);
    last = std::min(int(x.size()), last + pad);
    float peak = 0;
    for (int i = first; i < last; ++i) peak = std::max(peak, std::abs(x[i]));
    float gain = std::min(8.0f, 0.89f / std::max(peak, 1e-4f));  // 최대 -1 dBFS 로 맞춤
    int n = last - first;
    std::vector<int16_t> pcm16(static_cast<size_t>(n));  // (size_t(n) 은 함수 선언으로 해석됨)
    int fade = std::min(n / 4, rate_ / 100);  // 10ms 페이드로 '틱' 소리 방지
    for (int i = 0; i < n; ++i) {
        float g = gain;
        if (i < fade) g *= float(i) / fade;
        if (n - 1 - i < fade) g *= float(n - 1 - i) / fade;
        pcm16[size_t(i)] = int16_t(std::clamp(x[size_t(first + i)] * g, -1.0f, 1.0f) * 32767);
    }
    FILE* f = nullptr;
    std::wstring tmp = path + L".tmp";
    if (_wfopen_s(&f, tmp.c_str(), L"wb") || !f) { why = L"파일을 저장하지 못했습니다."; return false; }
    auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    uint32_t dataBytes = uint32_t(n * 2);
    fwrite("RIFF", 1, 4, f); w32(36 + dataBytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(1); w32(uint32_t(rate_)); w32(uint32_t(rate_ * 2)); w16(2); w16(16);
    fwrite("data", 1, 4, f); w32(dataBytes);
    fwrite(pcm16.data(), 2, pcm16.size(), f);
    fclose(f);
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { why = L"파일을 저장하지 못했습니다."; return false; }
    durationOut = double(n) / rate_;
    return true;
}

// ---------------- 녹음 창 ----------------
namespace {
enum { IDC_LIST = 201, IDC_REC, IDC_PLAY, IDC_DEL, IDC_FOLDER, IDC_CLOSE, IDC_STATUS, IDC_TITLE, IDC_HINT };
constexpr UINT_PTR TIMER_UI = 7;
const wchar_t* kDlgClass = L"UsagiDesktopPet.Voice";

struct Dlg {
    HWND hwnd{}, list{}, rec{}, play{}, del{}, folder{}, close{}, status{}, title{}, hint{};
    HFONT font{}, big{};
    VoiceBank* bank = nullptr;
    std::function<void(double)> onPlay;
    Recorder recorder;
    int recIndex = -1;
    std::wstring message;
    RECT meter{};
} d;

int Sel() {
    int i = int(SendMessageW(d.list, LB_GETCURSEL, 0, 0));
    return i < 0 ? 0 : i;
}

void FillList() {
    int keep = Sel();
    SendMessageW(d.list, LB_RESETCONTENT, 0, 0);
    for (const Phrase& p : Phrases()) {
        double dur = d.bank->Duration(p.id);
        wchar_t b[128];
        if (dur > 0) swprintf_s(b, L"●  %s   (%.1f초)", p.text, dur);
        else swprintf_s(b, L"○  %s", p.text);
        SendMessageW(d.list, LB_ADDSTRING, 0, LPARAM(b));
    }
    SendMessageW(d.list, LB_SETCURSEL, keep, 0);
}

void UpdateUi() {
    const Phrase& p = Phrases()[size_t(Sel())];
    bool busy = d.recorder.Busy();
    wchar_t t[128];
    swprintf_s(t, L"‘%s’", p.text);
    SetWindowTextW(d.title, t);
    SetWindowTextW(d.rec, busy ? L"■  녹음 끝내기" : L"●  녹음하기");
    bool has = d.bank->Duration(p.id) > 0;
    EnableWindow(d.play, !busy && has);
    EnableWindow(d.del, !busy && has);
    EnableWindow(d.list, !busy);
    std::wstring st;
    if (busy) {
        wchar_t b[96];
        swprintf_s(b, L"녹음 중… %.1f초 / 최대 5초", d.recorder.Elapsed());
        st = b;
    } else if (!d.message.empty()) {
        st = d.message;
    } else if (has) {
        wchar_t b[96];
        swprintf_s(b, L"녹음됨 (%.1f초). 말풍선이 뜰 때 이 목소리가 나와요.", d.bank->Duration(p.id));
        st = b;
    } else {
        const Phrase* fb = nullptr;
        for (const char* f = p.fallback; f;) {
            for (const Phrase& q : Phrases())
                if (!strcmp(q.id, f)) { fb = &q; break; }
            if (fb && d.bank->Duration(fb->id) > 0) break;
            f = fb ? fb->fallback : nullptr;
            fb = nullptr;
        }
        if (fb) st = std::wstring(L"아직 녹음 안 됨. 지금은 ‘") + fb->text + L"’ 목소리로 대신 나와요.";
        else st = L"아직 녹음 안 됨. 말풍선만 떠요.";
    }
    SetWindowTextW(d.status, st.c_str());
    InvalidateRect(d.hwnd, &d.meter, FALSE);
}

void Layout() {
    UINT dpi = GetDpiForWindow(d.hwnd);
    auto S = [dpi](int v) { return MulDiv(v, int(dpi), 96); };
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    if (d.font) DeleteObject(d.font);
    if (d.big) DeleteObject(d.big);
    ncm.lfMessageFont.lfHeight = -S(14);
    d.font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -S(22);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    d.big = CreateFontIndirectW(&ncm.lfMessageFont);
    for (HWND h : {d.list, d.rec, d.play, d.del, d.folder, d.close, d.status, d.hint}) SendMessageW(h, WM_SETFONT, WPARAM(d.font), TRUE);
    SendMessageW(d.title, WM_SETFONT, WPARAM(d.big), TRUE);
    MoveWindow(d.list, S(14), S(14), S(200), S(320), TRUE);
    int x = S(230), w = S(300);
    MoveWindow(d.title, x, S(14), w, S(34), TRUE);
    MoveWindow(d.status, x, S(52), w, S(44), TRUE);
    d.meter = RECT{x, S(102), x + w, S(114)};
    MoveWindow(d.rec, x, S(128), S(146), S(40), TRUE);
    MoveWindow(d.play, x + S(154), S(128), S(146), S(40), TRUE);
    MoveWindow(d.del, x, S(176), S(146), S(32), TRUE);
    MoveWindow(d.folder, x + S(154), S(176), S(146), S(32), TRUE);
    MoveWindow(d.hint, x, S(220), w, S(66), TRUE);
    MoveWindow(d.close, x + S(154), S(302), S(146), S(32), TRUE);
}

void FinishRecording() {
    if (d.recIndex < 0) return;
    std::wstring err = d.recorder.Error();
    const Phrase& p = Phrases()[size_t(d.recIndex)];
    if (!err.empty()) {
        d.message = err;
    } else {
        double dur = 0;
        std::wstring why;
        if (d.recorder.SaveResult(d.bank->PathFor(p.id), dur, why)) {
            d.bank->Refresh();
            d.message.clear();
            Log(L"record: '%s' 저장 %.2f초", p.text, dur);
            double pd = d.bank->PlayId(p.id);  // 바로 들려주기
            if (pd > 0 && d.onPlay) d.onPlay(pd);
        } else {
            d.message = why;
        }
    }
    d.recIndex = -1;
    FillList();
    UpdateUi();
}

LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == IDC_LIST && HIWORD(w) == LBN_SELCHANGE) { d.message.clear(); UpdateUi(); }
            else if (id == IDC_LIST && HIWORD(w) == LBN_DBLCLK) SendMessageW(h, WM_COMMAND, IDC_PLAY, 0);
            else if (id == IDC_REC) {
                if (d.recorder.Busy()) d.recorder.Stop();
                else {
                    d.message.clear();
                    PlaySoundW(nullptr, nullptr, 0);
                    d.recIndex = Sel();
                    d.recorder.Start(5.0);
                }
                UpdateUi();
            } else if (id == IDC_PLAY) {
                double pd = d.bank->PlayId(Phrases()[size_t(Sel())].id);
                if (pd > 0 && d.onPlay) d.onPlay(pd);
            } else if (id == IDC_DEL) {
                const Phrase& p = Phrases()[size_t(Sel())];
                std::wstring q = std::wstring(L"‘") + p.text + L"’ 녹음을 지울까요?";
                if (MessageBoxW(h, q.c_str(), L"목소리 녹음", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    DeleteFileW(d.bank->PathFor(p.id).c_str());
                    d.bank->Refresh();
                    FillList();
                    UpdateUi();
                }
            } else if (id == IDC_FOLDER) {
                ShellExecuteW(h, L"open", d.bank->Dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else if (id == IDC_CLOSE) {
                DestroyWindow(h);
            }
            return 0;
        }
        case WM_TIMER:
            if (d.recorder.Finished()) FinishRecording();
            else if (d.recorder.Busy()) UpdateUi();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            HBRUSH bg = CreateSolidBrush(RGB(226, 222, 214));
            FillRect(dc, &d.meter, bg);
            DeleteObject(bg);
            if (d.recorder.Busy()) {
                float lv = std::clamp((d.recorder.LevelDb() + 60.0f) / 60.0f, 0.0f, 1.0f);
                RECT r = d.meter;
                r.right = r.left + LONG((r.right - r.left) * lv);
                HBRUSH fg = CreateSolidBrush(lv > 0.9f ? RGB(220, 90, 70) : RGB(240, 150, 170));
                FillRect(dc, &r, fg);
                DeleteObject(fg);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
            SetBkMode(HDC(w), TRANSPARENT);
            return LRESULT(GetSysColorBrush(COLOR_WINDOW));
        case WM_DPICHANGED: {
            auto* r = reinterpret_cast<RECT*>(l);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }
        case WM_DESTROY:
            KillTimer(h, TIMER_UI);
            d.recorder.Cancel();
            d.recIndex = -1;
            if (d.font) { DeleteObject(d.font); d.font = nullptr; }
            if (d.big) { DeleteObject(d.big); d.big = nullptr; }
            d.hwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
}  // namespace

bool VoiceDialogOpen() { return d.hwnd != nullptr; }

void ShowVoiceDialog(HINSTANCE inst, VoiceBank* bank, std::function<void(double)> onPlay) {
    if (d.hwnd) {
        ShowWindow(d.hwnd, SW_RESTORE);
        SetForegroundWindow(d.hwnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = inst;
        wc.lpszClassName = kDlgClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.hIcon = HICON(GetClassLongPtrW(FindWindowW(L"UsagiDesktopPet.Msg", nullptr), GCLP_HICON));
        RegisterClassExW(&wc);
        registered = true;
    }
    d.bank = bank;
    d.onPlay = std::move(onPlay);
    d.message.clear();
    bank->Refresh();
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    UINT dpi = 96, dy;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dy);
    RECT rc{0, 0, MulDiv(544, int(dpi), 96), MulDiv(348, int(dpi), 96)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    int x = (mi.rcWork.left + mi.rcWork.right - W) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - H) / 2;
    d.hwnd = CreateWindowExW(0, kDlgClass, L"우사기 목소리 녹음", style, x, y, W, H, nullptr, nullptr, inst, nullptr);
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int id) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | st, 0, 0, 10, 10, d.hwnd, HMENU(INT_PTR(id)), inst, nullptr);
    };
    d.list = mk(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP, IDC_LIST);
    d.title = mk(L"STATIC", L"", 0, IDC_TITLE);
    d.status = mk(L"STATIC", L"", 0, IDC_STATUS);
    d.rec = mk(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, IDC_REC);
    d.play = mk(L"BUTTON", L"▶  들어보기", BS_PUSHBUTTON | WS_TABSTOP, IDC_PLAY);
    d.del = mk(L"BUTTON", L"지우기", BS_PUSHBUTTON | WS_TABSTOP, IDC_DEL);
    d.folder = mk(L"BUTTON", L"녹음 폴더 열기", BS_PUSHBUTTON | WS_TABSTOP, IDC_FOLDER);
    d.hint = mk(L"STATIC",
                L"표현을 고르고 ‘녹음하기’를 누른 뒤 말하세요. 다시 누르거나 5초가 지나면 끝나요. "
                L"마이크는 녹음하는 동안에만 써요.",
                0, IDC_HINT);
    d.close = mk(L"BUTTON", L"닫기", BS_PUSHBUTTON | WS_TABSTOP, IDC_CLOSE);
    Layout();
    FillList();
    SendMessageW(d.list, LB_SETCURSEL, 0, 0);
    UpdateUi();
    SetTimer(d.hwnd, TIMER_UI, 60, nullptr);
    ShowWindow(d.hwnd, SW_SHOW);
    SetForegroundWindow(d.hwnd);
}
