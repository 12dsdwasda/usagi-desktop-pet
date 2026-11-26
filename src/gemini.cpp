#include "gemini.h"
#include <shlobj.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <fstream>
#include <iterator>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

static std::wstring KeyPath() {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
        r = p;
        r += L"\\UsagiDesktopPet";
        CreateDirectoryW(r.c_str(), nullptr);
        r += L"\\gemini.key";
    }
    CoTaskMemFree(p);
    return r;
}

static std::string Trim(std::string s) {
    while (!s.empty() && (unsigned char)s.back() <= ' ') s.pop_back();
    size_t i = 0;
    while (i < s.size() && (unsigned char)s[i] <= ' ') ++i;
    return s.substr(i);
}

bool LoadGeminiKey(std::string& key) {
    key.clear();
    std::ifstream f(KeyPath(), std::ios::binary);
    if (f) {
        std::string blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        DATA_BLOB in{DWORD(blob.size()), reinterpret_cast<BYTE*>(blob.data())}, out{};
        if (!blob.empty() && CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            key = Trim(std::string(reinterpret_cast<char*>(out.pbData), out.cbData));
            SecureZeroMemory(out.pbData, out.cbData);
            LocalFree(out.pbData);
        }
    }
    if (key.empty()) {
        wchar_t buf[512];
        DWORD n = GetEnvironmentVariableW(L"GEMINI_API_KEY", buf, DWORD(std::size(buf)));
        if (n > 0 && n < std::size(buf)) key = Trim(ToUtf8(buf));
    }
    return !key.empty();
}

bool SaveGeminiKey(const std::string& raw) {
    std::wstring path = KeyPath();
    if (path.empty()) return false;
    std::string key = Trim(raw);
    if (key.empty()) return DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    DATA_BLOB in{DWORD(key.size()), reinterpret_cast<BYTE*>(key.data())}, out{};
    if (!CryptProtectData(&in, L"UsagiDesktopPet Gemini key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return bool(f);
}

static HttpResult HttpsRequest(const wchar_t* method, const wchar_t* host, const std::wstring& path, std::wstring hdr,
                               const std::string& body, int timeoutMs) {
    HttpResult r;
    const wchar_t* ua = L"UsagiDesktopPet/1.0 (https://github.com/12dsdwasda/usagi-desktop-pet)";
    HINTERNET s = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { r.err = L"WinHTTP 초기화 실패"; return r; }
    WinHttpSetTimeouts(s, 10000, 10000, 15000, timeoutMs);
    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET q = c ? WinHttpOpenRequest(c, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE)
                    : nullptr;
    bool ok = q && WinHttpSendRequest(q, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), hdr.empty() ? 0 : DWORD(-1),
                                      body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), DWORD(body.size()),
                                      DWORD(body.size()), 0) &&
              WinHttpReceiveResponse(q, nullptr);
    SecureZeroMemory(hdr.data(), hdr.size() * sizeof(wchar_t));
    if (!ok) {
        DWORD e = GetLastError();
        r.err = e == ERROR_WINHTTP_TIMEOUT ? L"응답 시간 초과" : L"네트워크 오류 " + std::to_wstring(e);
    } else {
        DWORD st = 0, sz = sizeof(st);
        WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz,
                            WINHTTP_NO_HEADER_INDEX);
        r.status = int(st);
        while (true) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(q, &avail) || avail == 0) break;
            size_t old = r.body.size();
            r.body.resize(old + avail);
            DWORD got = 0;
            if (!WinHttpReadData(q, r.body.data() + old, avail, &got)) { r.body.resize(old); break; }
            r.body.resize(old + got);
        }
    }
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return r;
}

HttpResult HttpsPostJson(const wchar_t* host, const std::wstring& path, const std::string& apiKey, const std::string& body,
                         int timeoutMs) {
    std::wstring hdr = L"Content-Type: application/json; charset=utf-8\r\nx-goog-api-key: " + FromUtf8(apiKey) + L"\r\n";
    HttpResult r = HttpsRequest(L"POST", host, path, hdr, body, timeoutMs);
    SecureZeroMemory(hdr.data(), hdr.size() * sizeof(wchar_t));
    return r;
}

HttpResult HttpsGet(const wchar_t* host, const std::wstring& pathAndQuery, int timeoutMs) {
    return HttpsRequest(L"GET", host, pathAndQuery, L"", "", timeoutMs);
}

std::wstring UrlEncodeQ(const std::wstring& w) {
    std::string s = ToUtf8(w), o;
    for (unsigned char ch : s) {
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') o += char(ch);
        else { char b[4]; snprintf(b, sizeof b, "%%%02X", ch); o += b; }
    }
    return FromUtf8(o);
}
