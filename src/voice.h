#pragma once
#include "common.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

// 말풍선 표현 하나 = 녹음 파일 하나 (%LOCALAPPDATA%\UsagiDesktopPet\voices\<id>.wav)
struct Phrase {
    const char* id;
    const wchar_t* text;      // 녹음 창에 보이는 표현
    const char* fallback;     // 이 표현 녹음이 없으면 대신 틀 표현
    double cooldownSec;       // 같은 목소리를 다시 틀기까지 최소 간격
};
const std::vector<Phrase>& Phrases();

class VoiceBank {
public:
    void Init();                         // 폴더 만들고 목록 읽기
    void Refresh();
    std::wstring Dir() const { return dir_; }
    std::wstring PathFor(const char* id) const;
    double Duration(const char* id) const;  // 녹음 없으면 0
    // 말풍선 문장에 맞는 목소리를 비동기로 재생. 재생 길이(초), 없으면 0
    double Play(const std::wstring& bubbleText, double now);
    double PlayId(const char* id);

private:
    std::wstring dir_;
    std::vector<double> durations_;
    std::vector<double> lastPlayed_;
};

// 기본 녹음 장치(마이크)에서 최대 maxSec 초 녹음. 녹음 버튼을 누른 동안에만 마이크를 연다.
class Recorder {
public:
    ~Recorder() { Cancel(); }
    void Start(double maxSec = 5.0);
    void Stop() { stop_ = true; }        // 정상 종료 (결과 보관)
    void Cancel();                       // 결과 버림
    bool Busy() const { return busy_; }
    bool Finished();                     // 끝났고 결과/오류를 아직 안 가져감
    double Elapsed() const { return elapsed_; }
    float LevelDb() const { return level_; }
    std::wstring Error();
    // 앞뒤 무음을 자르고 크기를 맞춰 WAV 로 저장. 소리가 없으면 false
    bool SaveResult(const std::wstring& path, double& durationOut, std::wstring& why);

private:
    void Run(double maxSec);
    std::thread th_;
    std::atomic<bool> stop_{false}, busy_{false}, finished_{false};
    std::atomic<double> elapsed_{0};
    std::atomic<float> level_{-90};
    std::mutex mu_;
    std::wstring err_;
    std::vector<float> pcm_;
    int rate_ = 48000;
};

// 녹음 창 (트레이 ▸ 목소리 녹음…)
void ShowVoiceDialog(HINSTANCE inst, VoiceBank* bank, std::function<void(double)> onPlay);
bool VoiceDialogOpen();
