#include "wintarget.h"
#include <dwmapi.h>
#include <algorithm>

#pragma comment(lib, "dwmapi.lib")

RECT VisibleFrame(HWND h) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
        GetWindowRect(h, &r);
    return r;
}

bool IsInMoveSize(HWND h) {
    DWORD tid = GetWindowThreadProcessId(h, nullptr);
    GUITHREADINFO gi{sizeof(gi)};
    if (!tid || !GetGUIThreadInfo(tid, &gi)) return false;
    return (gi.flags & GUI_INMOVESIZE) != 0;
}

static DWORD IntegrityOf(HANDLE token) {
    DWORD len = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &len);
    std::vector<BYTE> buf(len ? len : 1);
    if (!GetTokenInformation(token, TokenIntegrityLevel, buf.data(), len, &len)) return 0;
    auto* til = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
    PSID sid = til->Label.Sid;
    return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
}

static DWORD OwnIntegrity() {
    static DWORD v = [] {
        HANDLE t = nullptr;
        DWORD r = 0;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) { r = IntegrityOf(t); CloseHandle(t); }
        return r;
    }();
    return v;
}

// 관리자 권한 등 우리보다 높은 무결성 수준의 창은 UIPI 때문에 옮길 수 없다
static bool HigherIntegrity(DWORD pid, std::wstring* exe) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return true;  // 정보조차 못 얻으면 보호된 프로세스로 간주
    if (exe) {
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &n)) {
            std::wstring s(path, n);
            size_t k = s.find_last_of(L'\\');
            *exe = k == std::wstring::npos ? s : s.substr(k + 1);
        }
    }
    HANDLE t = nullptr;
    bool higher = true;
    if (OpenProcessToken(p, TOKEN_QUERY, &t)) {
        higher = IntegrityOf(t) > OwnIntegrity();
        CloseHandle(t);
    }
    CloseHandle(p);
    return higher;
}

bool IsMovableWindow(HWND h, std::wstring* why) {
    auto no = [&](const wchar_t* w) { if (why) *why = w; return false; };
    if (!IsWindow(h) || !IsWindowVisible(h)) return no(L"보이지 않음");
    if (GetAncestor(h, GA_ROOT) != h) return no(L"최상위 창 아님");
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return no(L"다른 가상 데스크톱/숨김");
    if (IsIconic(h)) return no(L"최소화됨");
    if (IsZoomed(h)) return no(L"최대화됨");
    LONG style = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return no(L"도구 창");
    if ((style & WS_CAPTION) != WS_CAPTION && !(style & WS_THICKFRAME)) return no(L"제목 표시줄 없음");
    if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return no(L"소유된 대화상자");
    wchar_t cls[128] = {};
    GetClassNameW(h, cls, 128);
    static const wchar_t* shell[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
                                     L"Windows.UI.Core.CoreWindow", L"NotifyIconOverflowWindow",
                                     L"TopLevelWindowForOverflowXamlIsland", L"XamlExplorerHostIslandWindow",
                                     L"MultitaskingViewFrame", L"ForegroundStaging", L"Shell_InputSwitchTopLevelWindow"};
    for (auto* s : shell)
        if (!wcscmp(cls, s)) return no(L"시스템 UI");
    if (GetWindowTextLengthW(h) == 0) return no(L"제목 없음");
    RECT r = VisibleFrame(h);
    if (r.right - r.left < 120 || r.bottom - r.top < 60) return no(L"너무 작음");
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONULL);
    if (!mon) return no(L"화면 밖");
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    if (r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom)
        return no(L"전체 화면");
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return no(L"자기 자신");
    if (HigherIntegrity(pid, nullptr)) return no(L"관리자 권한 창");
    if (IsHungAppWindow(h)) return no(L"응답 없음");
    return true;
}

struct EnumCtx {
    std::vector<TargetWindow>* out;
    HWND ex1, ex2;
};

static BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<EnumCtx*>(lp);
    if (h == c->ex1 || h == c->ex2) return TRUE;
    if (!IsMovableWindow(h)) return TRUE;
    TargetWindow t{h};
    int n = GetWindowTextLengthW(h);
    t.title.resize(size_t(n) + 1);
    t.title.resize(GetWindowTextW(h, t.title.data(), n + 1));
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    HigherIntegrity(pid, &t.exe);
    c->out->push_back(std::move(t));
    return c->out->size() < 25;
}

std::vector<TargetWindow> ListMovableWindows(HWND exclude1, HWND exclude2) {
    std::vector<TargetWindow> v;
    EnumCtx c{&v, exclude1, exclude2};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&c));
    return v;
}
