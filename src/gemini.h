#pragma once
#include "common.h"

// UTF-8 <-> UTF-16
std::string ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);

// Gemini API 키: %LOCALAPPDATA%\UsagiDesktopPet\gemini.key 에 이 Windows 사용자 계정으로만 풀 수 있게
// DPAPI 로 암호화해 저장한다. 파일이 없으면 환경 변수 GEMINI_API_KEY 를 쓴다.
bool LoadGeminiKey(std::string& key);
bool SaveGeminiKey(const std::string& key);  // 빈 문자열이면 삭제

struct HttpResult {
    int status = 0;       // HTTP 상태 코드, 0 = 연결 실패
    std::string body;     // UTF-8 응답 본문
    std::wstring err;     // 연결 실패 사유
};
// HTTPS POST (JSON). 작업 스레드에서 호출한다 (블로킹).
HttpResult HttpsPostJson(const wchar_t* host, const std::wstring& path, const std::string& apiKey, const std::string& body,
                         int timeoutMs);
