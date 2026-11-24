#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// QueryPerformanceCounter 기반 초 단위 시계 (모든 스레드 공통)
double NowSec();

// 실행 파일이 있는 폴더 (끝에 '\' 포함)
std::wstring ExeDir();

// %LOCALAPPDATA%\UsagiDesktopPet\ (끝에 '\' 포함, 없으면 만듦)
std::wstring DataDir();
// 에셋 폴더: EXE 옆 assets\ 가 있으면 그것, 없으면 EXE 안에 든 에셋을 DataDir()\assets\ 로 풀어서 쓴다
std::wstring AssetsDir();
// 설정 파일: EXE 옆 settings.ini 가 있으면 그것, 없으면 DataDir()\settings.ini (처음엔 기본값을 풀어 둠)
std::wstring SettingsPath();

// 간단한 로그 (--log <경로> 로 켬). 스레드 안전.
void LogInit(const std::wstring& path);
void Log(const wchar_t* fmt, ...);

// INI 읽기 도우미
int IniInt(const std::wstring& file, const wchar_t* sec, const wchar_t* key, int def);
double IniDouble(const std::wstring& file, const wchar_t* sec, const wchar_t* key, double def);
std::wstring IniStr(const std::wstring& file, const wchar_t* sec, const wchar_t* key, const wchar_t* def);
