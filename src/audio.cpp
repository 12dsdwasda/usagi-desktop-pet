#include "audio.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "ole32.lib")

namespace {
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT / PCM (ksmedia.h 없이 직접 정의)
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// 기본 출력 장치 변경 알림 → 캡처 재시작 요청
class DeviceNotifier : public IMMNotificationClient {
public:
    explicit DeviceNotifier(std::atomic<bool>* flag) : flag_(flag) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** pp) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *pp = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) {
            Log(L"audio: 기본 출력 장치 변경 감지");
            *flag_ = true;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    LONG ref_ = 1;
    std::atomic<bool>* flag_;
};
}  // namespace

void AudioMonitor::Start(const AudioParams& p) {
    if (thread_.joinable()) return;
    params_ = p;
    stop_ = false;
    thread_ = std::thread([this] { ThreadMain(); });
}

void AudioMonitor::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    std::lock_guard<std::mutex> lk(mu_);
    snap_ = AudioSnapshot{};
}

AudioSnapshot AudioMonitor::Get() {
    std::lock_guard<std::mutex> lk(mu_);
    return snap_;
}

void AudioMonitor::ThreadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    DeviceNotifier* notifier = new DeviceNotifier(&restart_);
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))))
        en->RegisterEndpointNotificationCallback(notifier);
    int restarts = 0;
    while (!stop_) {
        restart_ = false;
        bool ok = CaptureLoop();
        {
            std::lock_guard<std::mutex> lk(mu_);
            snap_.deviceOk = false;
            snap_.restarts = ++restarts;
            if (!ok) {  // 장치 오류: 재생 상태를 버린다. 장치 변경(ok): 잠깐 끊겨도 춤이 끊기지 않게 유지
                snap_.playing = false;
                snap_.beatLocked = false;
            }
        }
        if (!ok) {
            playing_ = locked_ = false;
            activeRun_ = gapRun_ = silentRun_ = 0;
            onsets_.clear();
        }
        if (stop_) break;
        // 장치 오류면 잠시 후 재시도, 기본 장치 변경이면 곧바로 다시 연결
        double wait = ok ? 0.3 : 2.0;
        double until = NowSec() + wait;
        while (!stop_ && NowSec() < until) Sleep(50);
        Log(L"audio: 캡처 재시작 (%s)", ok ? L"장치 변경" : L"오류 후 재시도");
    }
    if (en) {
        en->UnregisterEndpointNotificationCallback(notifier);
        en->Release();
    }
    notifier->Release();
    CoUninitialize();
}

bool AudioMonitor::CaptureLoop() {
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* cap = nullptr;
    WAVEFORMATEX* wf = nullptr;
    bool result = false;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&wf);
    if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, wf, nullptr);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&cap));
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) {
        Log(L"audio: 캡처 시작 실패 hr=0x%08X", unsigned(hr));
    } else {
        // 샘플 형식 판별
        bool isFloat = false;
        int bits = wf->wBitsPerSample;
        if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) isFloat = true;
        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wf);
            isFloat = ext->SubFormat == kSubtypeFloat;
            if (!isFloat && ext->SubFormat != kSubtypePcm) bits = 0;
        }
        const int ch = wf->nChannels;
        const double sr = wf->nSamplesPerSec;
        const int hopLen = std::max(1, int(sr / 100));  // 10ms
        const double a = std::exp(-2.0 * 3.14159265358979 * 150.0 / sr);  // 저역 통과(킥) 150Hz
        double lp1 = 0, lp2 = 0, sumSq = 0, sumLow = 0;
        int hopN = 0;
        double lastData = NowSec();
        Log(L"audio: 캡처 시작 %d Hz, %d ch, %d bit%s", int(sr), ch, bits, isFloat ? L" float" : L"");
        {
            std::lock_guard<std::mutex> lk(mu_);
            snap_.deviceOk = true;
        }
        result = true;
        while (!stop_ && !restart_) {
            UINT32 packet = 0;
            hr = cap->GetNextPacketSize(&packet);
            if (FAILED(hr)) { result = false; Log(L"audio: 장치 오류 hr=0x%08X", unsigned(hr)); break; }
            if (packet == 0) {
                double now = NowSec();
                // loopback 은 아무것도 재생되지 않으면 패킷이 오지 않으므로 무음으로 처리
                if (now - lastData > 0.05) { ProcessSilence(now - lastData, now); lastData = now; }
                Sleep(10);
                continue;
            }
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) { result = false; Log(L"audio: GetBuffer 실패 hr=0x%08X", unsigned(hr)); break; }
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) || bits == 0;
            double now = NowSec();
            double tStart = now - frames / sr;
            for (UINT32 i = 0; i < frames; ++i) {
                double s = 0;
                if (!silent) {
                    for (int c = 0; c < ch; ++c) {
                        const BYTE* p = data + (size_t(i) * ch + c) * (bits / 8);
                        if (isFloat && bits == 32) s += *reinterpret_cast<const float*>(p);
                        else if (bits == 16) s += *reinterpret_cast<const int16_t*>(p) / 32768.0;
                        else if (bits == 32) s += *reinterpret_cast<const int32_t*>(p) / 2147483648.0;
                        else if (bits == 24) s += ((int32_t(p[0]) << 8 | int32_t(p[1]) << 16 | int32_t(p[2]) << 24) >> 8) / 8388608.0;
                    }
                    s /= ch;
                }
                lp1 = (1 - a) * s + a * lp1;
                lp2 = (1 - a) * lp1 + a * lp2;
                sumSq += s * s;
                sumLow += lp2 * lp2;
                if (++hopN == hopLen) {
                    ProcessHop(std::sqrt(sumSq / hopN), std::sqrt(sumLow / hopN), tStart + i / sr);
                    sumSq = sumLow = 0;
                    hopN = 0;
                }
            }
            cap->ReleaseBuffer(frames);
            lastData = now;
        }
        client->Stop();
    }
    if (wf) CoTaskMemFree(wf);
    SafeRelease(cap);
    SafeRelease(client);
    SafeRelease(dev);
    SafeRelease(en);
    return result;
}

static double ToDb(double rms) { return 20.0 * std::log10(std::max(rms, 1e-7)); }

void AudioMonitor::ProcessSilence(double dur, double t) {
    if (t < suppressUntil_) return;
    // 10ms 무음 hop 여러 개로 취급 (긴 공백은 한 번에)
    int n = int(dur / 0.01);
    if (n > 50) {
        silentRun_ += dur;
        activeRun_ = 0;
        gapRun_ = 0;
        ProcessHop(0, 0, t);
        return;
    }
    for (int i = 0; i < n; ++i) ProcessHop(0, 0, t - (n - 1 - i) * 0.01);
}

void AudioMonitor::ProcessHop(double rms, double lowRms, double t) {
    if (t < suppressUntil_) return;  // 우사기 목소리 재생 중
    const double hop = 0.01;
    double db = ToDb(rms);
    levelDb_ = levelDb_ * 0.9 + db * 0.1;
    // ---- 지속적인 재생 판단 (크기 + 지속 시간) ----
    bool active = db > params_.thresholdDb;
    if (active) {
        activeRun_ += hop + gapRun_;
        gapRun_ = 0;
        silentRun_ = 0;
    } else {
        silentRun_ += hop;
        gapRun_ += hop;
        if (gapRun_ > params_.gapSec) { activeRun_ = 0; gapRun_ = 0; }
    }
    if (!playing_ && activeRun_ >= params_.startSec) {
        playing_ = true;
        Log(L"audio: 재생 감지 (%.1f dB)", db);
    } else if (playing_ && silentRun_ >= params_.stopSec) {
        playing_ = false;
        locked_ = false;
        stableCount_ = 0;
        onsets_.clear();
        Log(L"audio: 재생 멈춤");
    }
    // ---- 킥(저역) 온셋 검출 ----
    double e = lowRms * lowRms;
    if (lowAvg_ <= 0) lowAvg_ = e;
    double lowDb = ToDb(lowRms);
    if (e > 2.0 * lowAvg_ && e > prevLow_ * 1.4 && t - lastOnset_ > 0.22 && lowDb > params_.thresholdDb - 6) {
        lastOnset_ = t;
        OnOnset(t);
    }
    lowAvg_ += (e - lowAvg_) * 0.02;  // 약 0.5초 평균
    prevLow_ = e;
    if (locked_ && t - lastGridHit_ > 4 * period_) {
        locked_ = false;
        stableCount_ = 0;
        Log(L"audio: 박자 놓침");
    }
    std::lock_guard<std::mutex> lk(mu_);
    snap_.deviceOk = true;
    snap_.playing = playing_;
    snap_.beatLocked = locked_ && playing_;
    snap_.period = period_;
    snap_.beatTime = beatTime_;
    snap_.levelDb = levelDb_;
}

void AudioMonitor::OnOnset(double t) {
    onsets_.push_back(t);
    while (!onsets_.empty() && t - onsets_.front() > 8.0) onsets_.erase(onsets_.begin());
    if (onsets_.size() < 5) { if (!locked_) beatTime_ = t; return; }
    // 박자 간격 후보(0.3~1.0초)마다 온셋 쌍 간격이 배수로 맞는 정도를 점수화
    double bestP = 0, bestScore = 0;
    for (double P = 0.30; P <= 1.0; P += 0.005) {
        double score = 0;
        for (size_t i = 0; i < onsets_.size(); ++i)
            for (size_t j = i + 1; j < onsets_.size(); ++j) {
                double d = onsets_[j] - onsets_[i];
                if (d > 4.2 * P) break;
                int k = int(std::lround(d / P));
                if (k < 1 || k > 4) continue;
                double err = (d - k * P) / 0.025;
                score += std::exp(-0.5 * err * err) * (k == 1 ? 1.0 : k == 2 ? 0.8 : 0.6);
            }
        double l = std::log2(P / 0.5);
        score *= std::exp(-0.5 * l * l);
        if (score > bestScore) { bestScore = score; bestP = P; }
    }
    if (bestP <= 0) return;
    int good = 0, total = 0;
    for (size_t i = 1; i < onsets_.size(); ++i) {
        double d = onsets_[i] - onsets_[i - 1];
        int k = int(std::lround(d / bestP));
        ++total;
        if (k >= 1 && k <= 4 && std::abs(d - k * bestP) < std::max(0.035, 0.06 * bestP)) ++good;
    }
    double conf = total ? double(good) / total : 0;
    if (std::abs(bestP - period_) / period_ < 0.04) {
        ++stableCount_;
        period_ = period_ * 0.7 + bestP * 0.3;
    } else {
        stableCount_ = 0;
        period_ = bestP;
    }
    // 위상: 예측한 박자 근처의 온셋으로 조금씩 보정
    double n = std::round((t - beatTime_) / period_);
    double pred = beatTime_ + n * period_;
    double err = t - pred;
    if (std::abs(err) < 0.2 * period_) {
        beatTime_ = pred + 0.5 * err;
        lastGridHit_ = t;
    } else if (!locked_) {
        beatTime_ = t;
        lastGridHit_ = t;
    }
    bool wasLocked = locked_;
    locked_ = stableCount_ >= 3 && conf >= 0.6;
    if (locked_ != wasLocked)
        Log(L"audio: 박자 %s (%.0f BPM, 신뢰도 %.2f)", locked_ ? L"고정" : L"해제", 60.0 / period_, conf);
}
