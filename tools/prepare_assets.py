"""원본 스프라이트(../usagi-animation)에서 실행용 에셋을 만든다.
- 각 프레임을 발바닥 기준선(BASE_Y)에 맞춰 세로 정렬 (dance의 점프 프레임은 점프 높이를 유지)
- drag: 그림에 포함된 가짜 창을 지운 파생 스프라이트를 만들고, 손 위치가 프레임마다 같도록 가로 정렬
- assets/usagi.ini 에 프레임 시간(animations.json)과 손 위치를 기록
원본 파일은 수정하지 않는다.
사용: python tools/prepare_assets.py <원본폴더> <assets폴더>
"""
import sys, json, os
import numpy as np
from PIL import Image
from scipy import ndimage as nd

SRC, DST = sys.argv[1], sys.argv[2]
FW = FH = 384
BASE_Y = 344          # 정렬 후 발바닥(본체 최하단) 위치
meta = json.load(open(os.path.join(SRC, 'animations.json'), encoding='utf-8'))['animations']

def body_bottom(f):
    m = f[..., 3] > 128
    lab, k = nd.label(m)
    sz = nd.sum(m, lab, range(1, k + 1))
    big = lab == (np.argmax(sz) + 1)
    ys, xs = np.nonzero(big)
    return ys.max(), big

def shift(f, dx, dy):
    out = np.zeros_like(f)
    h, w = f.shape[:2]
    ys0, ys1 = max(0, dy), min(h, h + dy)
    xs0, xs1 = max(0, dx), min(w, w + dx)
    out[ys0:ys1, xs0:xs1] = f[ys0 - dy:ys1 - dy, xs0 - dx:xs1 - dx]
    return out

def remove_fake_window(f):
    g = f.astype(int)
    r, gg, b, al = g[..., 0], g[..., 1], g[..., 2], g[..., 3]
    op = al > 8
    dark = op & ((r + gg + b) / 3 < 110)
    rab = nd.binary_opening(op & ~dark & ((r - b) > 18), iterations=1)   # 크림/분홍 = 캐릭터
    win = nd.binary_opening(op & ~dark & ~rab, iterations=1)             # 흰색/라벤더 = 창
    lab, k = nd.label(win)
    sz = nd.sum(win, lab, range(1, k + 1))
    win = np.isin(lab, [j + 1 for j in range(k) if sz[j] > 300])         # 큰 덩어리만 (눈 하이라이트 제외)
    dr = nd.distance_transform_edt(~rab)
    dw = nd.distance_transform_edt(~win)
    kill = win | (op & ~rab & (dw < dr) & (dw < 14))
    kill |= op & ~rab & nd.binary_dilation(kill, iterations=1) & (dr > 3)
    out = f.copy()
    out[kill, 3] = 0
    ys, xs = np.nonzero(win)
    return out, (xs.min(), xs.max(), ys.min(), ys.max())

ini = ['; 자동 생성: tools/prepare_assets.py', '[sprites]', f'frameW={FW}', f'frameH={FH}', f'baseY={BASE_Y}', '']
for name in ['walk', 'dance', 'rap', 'drag']:
    sheet = np.array(Image.open(os.path.join(SRC, 'sprites', name + '.png')).convert('RGBA'))
    frames = [sheet[:, i * FW:(i + 1) * FW] for i in range(4)]
    info = []
    if name == 'drag':
        sep = [remove_fake_window(f) for f in frames]
        frames = [s[0] for s in sep]
        rects = [s[1] for s in sep]
        # 손 = 분리 후 창이 있던 오른쪽 가장자리 근처의 캐릭터 최좌측 픽셀
        grips = []
        for f, (x0, x1, y0, y1) in zip(frames, rects):
            m = f[..., 3] > 128
            rows = [y for y in range(y0, y1) if m[y, x1 - 12:x1 + 30].any()]
            hx = min(np.nonzero(m[y, x1 - 12:x1 + 30])[0][0] + x1 - 12 for y in rows)
            grips.append((hx, int(np.mean(rows)), y0))
        tx = int(round(np.mean([g[0] for g in grips])))
    grounded = []
    for i, f in enumerate(frames):
        bot, _ = body_bottom(f)
        grounded.append(bot)
    # dance: 점프 프레임(본체가 지면 프레임보다 8px 이상 높음)은 점프 높이를 유지
    ground = max(grounded)
    out = []
    for i, f in enumerate(frames):
        bot = grounded[i]
        if name == 'dance' and ground - bot >= 8:
            gf = [b for b in grounded if ground - b < 8]
            dy = BASE_Y - int(round(np.mean(gf)))    # 지면 프레임 평균만큼만 이동 → 점프 유지
        else:
            dy = BASE_Y - bot
        dx = 0
        if name == 'drag':
            dx = tx - grips[i][0]
        out.append(shift(f, dx, dy))
        info.append((dx, dy))
    # 몸통 중심(크림색 픽셀 평균 x)과 불투명 영역 가로 범위
    cx, xmin, xmax = [], 384, 0
    for f in out:
        g = f.astype(int)
        cream = (g[..., 3] > 128) & (g[..., 0] > 220) & (g[..., 1] > 200) & (g[..., 2] > 150) & (g[..., 2] < 225) & (g[..., 0] - g[..., 2] > 25)
        cx.append(np.nonzero(cream)[1].mean())
        xs = np.nonzero(f[..., 3] > 32)[1]
        xmin, xmax = min(xmin, xs.min()), max(xmax, xs.max())
    Image.fromarray(np.concatenate(out, axis=1)).save(os.path.join(DST, 'sprites', name + '.png'))
    durs = [fr['durationMs'] for fr in meta[name]['frames']]
    ini.append(f'[{name}]')
    ini.append('durations=' + ','.join(map(str, durs)))
    ini.append(f'anchorX={int(round(np.mean(cx)))}')
    ini.append(f'left={int(xmin)}')
    ini.append(f'right={int(xmax)}')
    if name == 'drag':
        gy = int(round(np.mean([g[1] + info[i][1] for i, g in enumerate(grips)])))
        top = int(round(np.mean([g[2] + info[i][1] for i, g in enumerate(grips)])))
        ini.append(f'gripX={tx}')
        ini.append(f'gripY={gy}')
        ini.append(f'fakeWindowTop={top}')
    print(name, 'offsets', [(int(a), int(b)) for a, b in info], 'bottoms', [int(b) for b in grounded])
    ini.append('')
open(os.path.join(DST, 'usagi.ini'), 'w', encoding='utf-16').write('\n'.join(ini))
