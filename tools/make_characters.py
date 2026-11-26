"""포즈 한 장씩 그려진 캐릭터 시트에서 우사기와 같은 형식의 실행용 스프라이트를 만든다.
- 입력 1: 5명 × (걷기, 춤, 랩, 창 끌기) 시트 (행 = 동작, 열 = 캐릭터)
- 입력 2: 5명 × (장갑에 위로 들림, 옆으로 들림) 시트
- 포즈마다 한 장뿐이라 회전·통통 튀기·찌그러뜨리기로 4프레임을 만든다
- 창 끌기 포즈의 그림 속 창은 지운다 (실제 창을 끌기 때문)
출력: assets/characters/<id>/{walk,dance,rap,drag,held}.png + char.ini (ASCII)
사용: python tools/make_characters.py <포즈 시트> <장갑 시트> <assets 폴더> [미리보기.png]
"""
import sys, os
import numpy as np
from PIL import Image
from scipy import ndimage as nd

args = sys.argv[1:]
WALK = None
if '--walk' in args:  # 캐릭터별 4프레임 걷기 시트 (행 = 캐릭터, 열 = 프레임)
    i = args.index('--walk'); WALK = args[i + 1]; del args[i:i + 2]
POSES, HELD, DST = args[0], args[1], args[2]
PREVIEW = args[3] if len(args) > 3 else None
IDS = ['hachiware', 'chiikawa', 'rakko', 'shisa', 'momonga']
FW = FH = 384
BASE_Y = 344
S_POSE = 1.05     # 포즈 시트 배율 (치이카와 걷기 키 ≈ 우사기 몸통)
S_HELD = 0.80     # 장갑 시트는 더 크게 그려져 있다
S_DRAG = 1.28     # 창 끌기 포즈는 창 때문에 캐릭터가 작게 그려져 있다
HOLD = (232, 52)  # held 프레임에서 장갑(=마우스 포인터) 위치
GRIP_X = 150      # drag 프레임에서 손 위치

def cells(sheet, rows, split=None, n=5):
    """행마다 x 순서로 5칸을 돌려준다. split[row] = 붙어 있는 두 칸을 나눌 x 목록"""
    a = sheet[..., 3] > 20
    lab, _ = nd.label(nd.binary_dilation(a, iterations=6))
    objs = [(j + 1, o) for j, o in enumerate(nd.find_objects(lab))
            if (o[0].stop - o[0].start) * (o[1].stop - o[1].start) > 4000]
    out = []
    for r, (y0, y1) in enumerate(rows):
        mine = [(j, o) for j, o in objs if y0 <= (o[0].start + o[0].stop) / 2 < y1]
        keep = np.isin(lab, [j for j, _ in mine])
        boxes = sorted([(o[1].start, o[1].stop) for _, o in mine])
        if split and r in split:
            nb = []
            for b in boxes:
                cut = [x for x in split[r] if b[0] < x < b[1]]
                xs = [b[0]] + cut + [b[1]]
                nb += list(zip(xs[:-1], xs[1:]))
            boxes = nb
        assert len(boxes) == n, (r, boxes)
        row = []
        for x0, x1 in boxes:
            c = sheet[:, x0:x1].copy()
            c[~keep[:, x0:x1]] = 0  # 다른 행에 속한 조각 제거
            ys, xs = np.nonzero(c[..., 3] > 20)
            row.append(c[ys.min():ys.max() + 1, xs.min():xs.max() + 1])
        out.append(row)
    return out

def remove_window(c):
    """왼쪽의 그림 창을 지운다. 창 안쪽의 밝은 면(제목 표시줄, 본문)은 그림 왼쪽 끝에 닿아 있다."""
    g = c.astype(int)
    op = g[..., 3] > 20
    dark = op & (g[..., :3].mean(-1) < 110)
    light = op & ~dark
    lab, k = nd.label(light)
    win = np.zeros_like(light)
    boxes = []
    for j, sl in enumerate(nd.find_objects(lab), 1):
        if sl is not None and sl[1].start <= 20 and (lab[sl] == j).sum() > 200:
            win |= lab == j
            boxes.append(sl)
    # 제목 표시줄 단추(□) 안쪽처럼 창 면에 둘러싸인 작은 밝은 조각
    for j, sl in enumerate(nd.find_objects(lab), 1):
        if sl is None or (lab[sl] == j).sum() > 150: continue
        for b in boxes:
            if b[0].start <= sl[0].start and sl[0].stop <= b[0].stop and b[1].start <= sl[1].start and sl[1].stop <= b[1].stop:
                win |= lab == j
    char = light & ~win
    dw = nd.distance_transform_edt(~win)
    dc = nd.distance_transform_edt(~char)
    kill = win | (dark & (dw < dc))
    out = c.copy()
    out[kill, 3] = 0
    keep = body(out)  # 캐릭터 본체에 붙은 것만 남김
    keep = nd.binary_dilation(keep, iterations=2) & (out[..., 3] > 0)
    out[~keep, 3] = 0
    ys, xs = np.nonzero(out[..., 3] > 20)
    return out[ys.min():ys.max() + 1, xs.min():xs.max() + 1]

def premul(a):
    f = a.astype(np.float32)
    f[..., :3] *= f[..., 3:4] / 255
    return f

def unpremul(f):
    out = f.copy()
    al = np.clip(out[..., 3:4], 1e-3, None)
    out[..., :3] = np.where(out[..., 3:4] > 0.5, out[..., :3] * 255 / al, 0)
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)

def affine(frame, pivot, rot=0.0, dx=0.0, dy=0.0, sx=1.0, sy=1.0):
    """pivot 기준으로 크기·회전 후 이동 (premultiplied 로 보간해서 가장자리가 깨끗하다)"""
    th = np.radians(rot)
    c, s = np.cos(th), np.sin(th)
    # 출력 → 입력 역변환
    px, py = pivot
    a, b = c / sx, s / sx
    d, e = -s / sy, c / sy
    tx, ty = px + dx, py + dy
    coeffs = (a, b, px - a * tx - b * ty, d, e, py - d * tx - e * ty)
    pm = premul(frame)
    chans = [Image.fromarray(pm[..., i]).transform((FW, FH), Image.AFFINE, coeffs, resample=Image.BICUBIC) for i in range(4)]
    return unpremul(np.stack([np.array(ch) for ch in chans], -1))

def scaled(c, k):
    im = Image.fromarray(c)
    return np.array(im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS))

def place(c, x, y):
    f = np.zeros((FH, FW, 4), np.uint8)
    h, w = c.shape[:2]
    sx0, sy0 = max(0, -x), max(0, -y)
    dx0, dy0 = max(0, x), max(0, y)
    w2, h2 = min(w - sx0, FW - dx0), min(h - sy0, FH - dy0)
    f[dy0:dy0 + h2, dx0:dx0 + w2] = c[sy0:sy0 + h2, sx0:sx0 + w2]
    return f

def body(c):
    m = c[..., 3] > 128
    lab, k = nd.label(m)
    sz = nd.sum(m, lab, range(1, k + 1))
    return lab == (np.argmax(sz) + 1)

def ground(c, cx=None):
    """발바닥 기준으로 프레임에 놓는다 (본체 최하단 = BASE_Y, 본체 중심 = 192)"""
    b = body(c)
    ys, xs = np.nonzero(b)
    x = 192 - int(round(xs.mean())) if cx is None else cx
    return place(c, x, BASE_Y - ys.max())

ANIMS = {  # (회전°, dx, dy, sx, sy)
    'walk': ([140, 110, 140, 110], [(0, 0, 0, 1, 1), (-3, 0, -7, 0.99, 1.02), (0, 0, 0, 1.03, 0.97), (3, 0, -7, 0.99, 1.02)]),
    'dance': ([190, 130, 190, 130], [(-8, -4, 0, 1, 1), (0, 0, -20, 0.96, 1.05), (8, 4, 0, 1, 1), (0, 0, 0, 1.07, 0.93)]),
    'rap': ([150, 100, 130, 110], [(-4, 0, 0, 1, 1), (0, 0, -5, 1, 1.01), (4, 0, 0, 1, 1), (0, 0, -5, 1.02, 0.98)]),
    'drag': ([240, 150, 230, 160], [(0, 0, 0, 1, 1), (-2.5, 0, 0, 1, 1), (0, 0, 0, 1.0, 0.97), (2, 0, 0, 1, 1)]),
}

def anim_frames(base, pivot, params):
    return [affine(base, pivot, *p) for p in params]

def stats(frames):
    xs0, xs1 = FW, 0
    for f in frames:
        xs = np.nonzero(f[..., 3] > 32)[1]
        xs0, xs1 = min(xs0, xs.min()), max(xs1, xs.max())
    cx = np.mean([np.nonzero(body(f))[1].mean() for f in frames])
    return int(round(cx)), int(xs0), int(xs1)

pose = np.array(Image.open(POSES).convert('RGBA'))
held = np.array(Image.open(HELD).convert('RGBA'))
H = pose.shape[0] / 4
P = cells(pose, [(i * H, (i + 1) * H) for i in range(4)], split={3: [1083]})
W = None
if WALK:
    wk = np.array(Image.open(WALK).convert('RGBA'))
    WH = wk.shape[0] / 5
    W = cells(wk, [(i * WH, (i + 1) * WH) for i in range(5)], n=4)
G = cells(held, [(0, held.shape[0] / 2), (held.shape[0] / 2, held.shape[0])])
previews = []
for ci, cid in enumerate(IDS):
    d = os.path.join(DST, 'characters', cid)
    os.makedirs(d, exist_ok=True)
    ini = ['; generated by tools/make_characters.py', '[sprites]', f'frameW={FW}', f'frameH={FH}', f'baseY={BASE_Y}', '']
    sheets = {}
    for ai, name in enumerate(['walk', 'dance', 'rap', 'drag']):
        c = P[ai][ci]
        if name == 'drag':
            c = remove_window(c)
        c = scaled(c, S_POSE * (S_DRAG if name == 'drag' else 1))
        durs, params = ANIMS[name]
        if name == 'drag':
            # 손 = 가장 왼쪽 불투명 픽셀들
            m = c[..., 3] > 128
            hx = int(np.nonzero(m.any(0))[0][0])
            hy = int(np.nonzero(m[:, hx:hx + 6].any(1))[0].mean())
            b = body(c)
            f0 = place(c, GRIP_X - hx, BASE_Y - np.nonzero(b)[0].max())
            gy = hy + (BASE_Y - np.nonzero(b)[0].max())
            frames = anim_frames(f0, (GRIP_X, gy), params)
        elif name == 'walk' and W:
            # 그려진 4프레임 걷기: 예전 걷기 포즈와 몸 키가 같도록 배율을 맞춘다
            bh = lambda im: np.ptp(np.nonzero(body(im))[0])
            k = bh(c) / np.median([bh(scaled(w, 1)) for w in W[ci]])
            frames = [ground(scaled(w, k)) for w in W[ci]]
            durs = [130, 130, 130, 130]
        else:
            f0 = ground(c)
            frames = anim_frames(f0, (192, BASE_Y), params)
        cx, l, r = stats(frames)
        sheets[name] = frames
        ini += [f'[{name}]', 'durations=' + ','.join(map(str, durs)), f'anchorX={cx}', f'left={l}', f'right={r}']
        if name == 'drag':
            ini += [f'gripX={GRIP_X}', f'gripY={gy}']
        ini.append('')
    # held: 0~3 위로 들려 흔들림, 4~7 옆으로 들림
    hf = []
    for gi in range(2):
        c = scaled(G[gi][ci], S_HELD)
        m = c[..., 3] > 128
        ys, xs = np.nonzero(m[:40])  # 장갑 맨 위 부분
        gx, gy = int(xs.mean()), int(ys.min()) + 40
        f0 = place(c, HOLD[0] - gx, HOLD[1] - gy)
        rots = [-6, 0, 6, 0] if gi == 0 else [-4, 0, 4, 0]
        hf += [affine(f0, HOLD, r) for r in rots]
    cx, l, r = stats(hf[:4])
    sheets['held'] = hf
    ini += ['[held]', 'durations=' + ','.join(['170'] * 8), f'anchorX={cx}', f'left={l}', f'right={r}',
            f'holdX={HOLD[0]}', f'holdY={HOLD[1]}', '']
    for name, frames in sheets.items():
        Image.fromarray(np.concatenate(frames, 1)).save(os.path.join(d, name + '.png'), optimize=True)
    open(os.path.join(d, 'char.ini'), 'w', encoding='ascii', newline='\r\n').write('\n'.join(ini))
    previews.append(np.concatenate([sheets[n][0] for n in ['walk', 'dance', 'rap', 'drag']] + [hf[0], hf[4]], 1))
    print(cid, 'ok')
if PREVIEW:
    pv = np.concatenate(previews, 0)
    bg = Image.new('RGBA', (pv.shape[1], pv.shape[0]), (110, 190, 120, 255))
    bg.alpha_composite(Image.fromarray(pv))
    a = np.array(bg)
    a[BASE_Y::FH, :, :3] = (255, 0, 0)  # 발바닥 기준선
    Image.fromarray(a).resize((pv.shape[1] // 2, pv.shape[0] // 2)).save(PREVIEW)
