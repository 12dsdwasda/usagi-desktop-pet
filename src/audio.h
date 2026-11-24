#pragma once
#include "common.h"
#include <atomic>
#include <mutex>
#include <thread>

struct AudioParams {
    double thresholdDb = -50.0;  // 이 크기(dBFS)보다 크면 '소리 있음'
    double startSec = 0.7;       // 이만큼 이어지면 재생 중으로 판단
    double stopSec = 1.5;        // 이만큼 조용하면 재생 끝으로 판단
    double gapSec = 0.3;         // 재생 시작 판단 중 허용하는 짧은 끊김
};

struct AudioSnapshot {
    bool deviceOk = false;   // 캡처 장치가 동작 중인가
    bool playing = false;    // 지속적인 재생음이 있는가
    bool beatLocked = false; // 박자가 안정적으로 잡혔는가
    double period = 0.5;     // 박자 간격(초)
    double beatTime = 0;     // 기준 박자 시각(NowSec 기준)
    double levelDb = -120;   // 최근 소리 크기
    int restarts = 0;        // 장치 재시작 횟수
};

// WASAPI loopback 으로 기본 출력 장치의 재생음을 분석한다.
// 오디오는 메모리에서만 처리하고 저장하거나 전송하지 않는다.
class AudioMonitor {
public:
    void Start(const AudioParams& p);
    void Stop();
    bool Running() const { return thread_.joinable(); }
    AudioSnapshot Get();
    void RequestRestart() { restart_ = true; }
    // 이 시각(NowSec)까지 들어오는 소리는 분석하지 않고 상태를 그대로 둔다
    void SuppressUntil(double t) { suppressUntil_ = t; }

private:
    void ThreadMain();
    bool CaptureLoop();  // 장치 하나로 캡처. false = 장치 문제로 재시작 필요
    void ProcessHop(double rms, double lowRms, double t);
    void OnOnset(double t);
    void ProcessSilence(double dur, double t);

    AudioParams params_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> restart_{false};
    std::atomic<double> suppressUntil_{0};
    std::mutex mu_;
    AudioSnapshot snap_;

    // 분석 상태 (오디오 스레드 전용)
    double activeRun_ = 0, gapRun_ = 0, silentRun_ = 0;
    bool playing_ = false;
    double lowAvg_ = 0, prevLow_ = 0, lastOnset_ = -10;
    std::vector<double> onsets_;
    double period_ = 0.5, beatTime_ = 0;
    int stableCount_ = 0;
    bool locked_ = false;
    double lastGridHit_ = -10;
    double levelDb_ = -120;
};
