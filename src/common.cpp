#include "common.h"
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <mutex>
#include <shlobj.h>
#include "resource_ids.h"

double NowSec() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(freq.QuadPart);
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L".\\" : p.substr(0, s + 1);
}

std::wstring DataDir() {
    PWSTR base = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) d = std::wstring(base) + L"\\UsagiDesktopPet\\";
    CoTaskMemFree(base);
    if (d.empty()) d = ExeDir();
    SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
    return d;
}

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// EXE 리소스를 파일로. overwrite=false 면 이미 있을 때 그대로 둔다. 내용이 같으면 다시 쓰지 않는다.
static bool ExtractResource(int id, const std::wstring& path, bool overwrite) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL h = r ? LoadResource(nullptr, r) : nullptr;
    const void* data = h ? LockResource(h) : nullptr;
    DWORD size = r ? SizeofResource(nullptr, r) : 0;
    if (!data || !size) return false;
    if (FileExists(path)) {
        if (!overwrite) return true;
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            std::vector<char> cur(size_t(size) + 1);
            DWORD got = 0;
            bool same = ReadFile(f, cur.data(), size + 1, &got, nullptr) && got == size && memcmp(cur.data(), data, size) == 0;
            CloseHandle(f);
            if (same) return true;
        }
    }
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    bool ok = WriteFile(f, data, size, &wr, nullptr) && wr == size;
    CloseHandle(f);
    ok = ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

std::wstring AssetsDir() {
    std::wstring local = ExeDir() + L"assets\\";
    if (FileExists(local + L"usagi.ini") && FileExists(local + L"sprites\\walk.png")) return local;
    std::wstring d = DataDir() + L"assets\\";
    SHCreateDirectoryExW(nullptr, (d + L"sprites").c_str(), nullptr);
    ExtractResource(IDR_USAGI_INI, d + L"usagi.ini", true);
    ExtractResource(IDR_SPRITE_WALK, d + L"sprites\\walk.png", true);
    ExtractResource(IDR_SPRITE_DANCE, d + L"sprites\\dance.png", true);
    ExtractResource(IDR_SPRITE_RAP, d + L"sprites\\rap.png", true);
    ExtractResource(IDR_SPRITE_DRAG, d + L"sprites\\drag.png", true);
    static const wchar_t* chars[] = {L"hachiware", L"chiikawa", L"rakko", L"shisa", L"momonga"};
    static const wchar_t* files[] = {L"char.ini", L"walk.png", L"dance.png", L"rap.png", L"drag.png", L"held.png"};
    for (int ci = 0; ci < 5; ++ci) {
        std::wstring cd = d + L"characters\\" + chars[ci] + L"\\";
        SHCreateDirectoryExW(nullptr, cd.c_str(), nullptr);
        for (int k = 0; k < 6; ++k) ExtractResource(IDR_CHAR_BASE + ci * 10 + k, cd + files[k], true);
    }
    return d;
}

std::wstring SettingsPath() {
    std::wstring local = ExeDir() + L"settings.ini";
    if (FileExists(local)) return local;
    std::wstring p = DataDir() + L"settings.ini";
    ExtractResource(IDR_SETTINGS_INI, p, false);  // 사용자가 고친 설정은 덮어쓰지 않음
    return p;
}

static std::mutex g_logMu;
static FILE* g_log = nullptr;

void LogInit(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(g_logMu);
    if (g_log) fclose(g_log);
    g_log = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);  // 실행 중에도 읽을 수 있게
}

void Log(const wchar_t* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMu);
    if (!g_log) return;
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    fwprintf(g_log, L"[%9.3f] %s\n", NowSec(), buf);
    fflush(g_log);
}

std::wstring IniStr(const std::wstring& file, const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[512];
    GetPrivateProfileStringW(sec, key, def, buf, DWORD(std::size(buf)), file.c_str());
    return buf;
}

int IniInt(const std::wstring& file, const wchar_t* sec, const wchar_t* key, int def) {
    std::wstring s = IniStr(file, sec, key, L"");
    if (s.empty()) return def;
    return _wtoi(s.c_str());
}

double IniDouble(const std::wstring& file, const wchar_t* sec, const wchar_t* key, double def) {
    std::wstring s = IniStr(file, sec, key, L"");
    if (s.empty()) return def;
    return _wtof(s.c_str());
}
