#pragma once
#include "common.h"
#include <random>

// 마우스 포인터에 붙는 불꽃 효과. 클릭 통과·포커스 없는 layered window 에 파티클을 직접 그린다.
// 하드웨어 포인터는 모든 창 위에 그려지므로 불꽃은 포인터 뒤에서 타오르는 것처럼 보인다.
class Fire {
public:
    bool Create(HINSTANCE inst, const wchar_t* cls);
    // burning=false 면 새 불꽃을 만들지 않고 남은 불꽃이 다 꺼지면 숨는다
    void Update(double now, POINT cursor, double scale, bool burning);
    bool Visible() const { return shown_; }

private:
    struct P { double x, y, vx, vy, born, life, r; };
    std::vector<P> ps_;
    std::vector<float> acc_;
    HWND hwnd_{};
    HDC dc_{};
    HBITMAP dib_{}, old_{};
    uint32_t* bits_ = nullptr;
    int w_ = 0, h_ = 0;
    double last_ = 0, spawnAcc_ = 0;
    bool shown_ = false;
    std::mt19937 rng_{12345};
};
