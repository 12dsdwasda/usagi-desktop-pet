#pragma once
#include "common.h"
#include <array>

// 미리 곱한 알파(premultiplied) BGRA 비트맵
struct Bitmap {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
};

enum AnimId { ANIM_WALK = 0, ANIM_DANCE, ANIM_RAP, ANIM_DRAG, ANIM_COUNT };

struct AnimMeta {
    std::vector<int> durationsMs;  // 프레임별 기본 재생 시간
    int anchorX = 192;             // 몸통 중심 x (프레임 좌표)
    int left = 0, right = 384;     // 불투명 영역 가로 범위
    int top = 0;                   // 불투명 영역 맨 위 (말풍선 위치)
};

struct SpriteSet {
    int frameW = 384, frameH = 384, baseY = 344;
    int gripX = 178, gripY = 274;  // drag 스프라이트에서 손이 창 가장자리에 닿는 점
    std::array<AnimMeta, ANIM_COUNT> meta;
    std::array<std::vector<Bitmap>, ANIM_COUNT> frames;  // 원본 해상도

    bool Load(const std::wstring& assetsDir, std::wstring& err);
};

// 크기를 바꾼 프레임 캐시 (DPI 바뀔 때만 다시 만듦)
class ScaledCache {
public:
    const Bitmap& Get(const SpriteSet& s, int anim, int frame, bool flip, int sizePx);
private:
    int sizePx_ = 0;
    std::array<std::vector<Bitmap>, ANIM_COUNT * 2> cache_;
};

Bitmap Resample(const Bitmap& src, int w, int h);
Bitmap FlipH(const Bitmap& src);
HICON MakeIconFromBitmap(const Bitmap& src, int size);
