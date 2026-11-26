// 우사기 데스크톱 펫 — macOS (AppKit)
import AppKit
import Carbon
import QuartzCore

func now() -> Double { CACurrentMediaTime() }
func rnd(_ a: Double, _ b: Double) -> Double { Double.random(in: a...b) }

func warpCursor(_ p: NSPoint) { CGWarpMouseCursorPosition(CGPoint(x: p.x, y: primaryHeight - p.y)) }

func screenFor(_ p: NSPoint) -> NSScreen {
    if let s = NSScreen.screens.first(where: { $0.frame.contains(p) }) { return s }
    // 화면 밖이면 가장 가까운 화면
    return NSScreen.screens.min(by: { dist($0.frame, p) < dist($1.frame, p) }) ?? NSScreen.main!
}
private func dist(_ r: NSRect, _ p: NSPoint) -> CGFloat {
    let dx = max(r.minX - p.x, 0, p.x - r.maxX), dy = max(r.minY - p.y, 0, p.y - r.maxY)
    return dx * dx + dy * dy
}

// ---------- 설정 (UserDefaults) ----------
final class Settings {
    private let d = UserDefaults.standard
    private func b(_ k: String, _ def: Bool) -> Bool { d.object(forKey: k) == nil ? def : d.bool(forKey: k) }
    var mode: String { get { d.string(forKey: "musicMode") ?? "dance" } set { d.set(newValue, forKey: "musicMode") } }
    var prank: String { get { d.string(forKey: "prank") ?? "sometimes" } set { d.set(newValue, forKey: "prank") } }
    var bubbles: Bool { get { b("bubbles", true) } set { d.set(newValue, forKey: "bubbles") } }
    var cursorGrab: Bool { get { b("cursorGrab", true) } set { d.set(newValue, forKey: "cursorGrab") } }
    var angry: Bool { get { b("angry", true) } set { d.set(newValue, forKey: "angry") } }
    var sing: Bool { get { b("sing", true) } set { d.set(newValue, forKey: "sing") } }
    var spotifyOnly: Bool { get { b("spotifyOnly", false) } set { d.set(newValue, forKey: "spotifyOnly") } }
    var cursorInterval: Double { max(3, d.object(forKey: "cursorInterval") == nil ? 10 : d.double(forKey: "cursorInterval")) }
    var blockSec: Double { min(10, max(0, d.object(forKey: "blockSec") == nil ? 5 : d.double(forKey: "blockSec"))) }
    var sizePt: CGFloat { CGFloat(min(512, max(48, d.object(forKey: "sizePt") == nil ? 140 : d.double(forKey: "sizePt")))) }
    var lyricsOffset: Double { d.double(forKey: "lyricsOffset") }
}

enum State: String {
    case walk = "걷기", idle = "쉬기", music = "음악", approach = "창으로 이동", drag = "창 끌기", release = "놓기", fall = "떨어지는 중"
    case chase = "포인터 쫓기", cursorDrag = "포인터 끌기", held = "마우스에 잡힘", angry = "화남", angryThrow = "포인터 던지기"
}

final class Pet: NSObject, NSMenuDelegate, AgentHost {
    let cfg = Settings()
    let sprites = Sprites()
    var panel: NSPanel!
    let view = PetView()
    var bubble: Bubble!
    var fire: Fire!
    let media = MediaWatcher()
    let lyrics = LyricsFetcher()
    let agent = Agent()
    var status: NSStatusItem!

    var state = State.walk
    var t0 = 0.0, lastTick = 0.0, lastMedia = 0.0, stateUntil = 0.0
    var px: CGFloat = 0, py: CGFloat = 0  // 발바닥 기준점 (AppKit 좌표)
    var facing: CGFloat = 1, side: CGFloat = 1
    var anim = Anim.walk, frame = 0, frameT = 0.0
    var visible = true, paused = false
    var s: CGFloat { cfg.sizePt / kFrame }

    // 창 끌기
    var target: AXWin?
    var dragDist: CGFloat = 0, dragMoved: CGFloat = 0, lastSetX: CGFloat = 0
    var nextPrank = 0.0
    // 포인터
    var nextCursor = 0.0, cursorStillSince = 0.0, cursorSeen = NSPoint.zero
    var cursorSet = NSPoint.zero, cursorDist: CGFloat = 0, cursorMoved: CGFloat = 0
    var lastChaseSay = 0.0
    // 잡기·던지기
    var pressPt = NSPoint.zero, pressT = 0.0, pressing = false
    var grabOff = NSPoint.zero
    var trail: [(t: Double, p: NSPoint)] = []
    var vx: CGFloat = 0, vy: CGFloat = 0, bounces = 0, thrown = false
    var chatPendingAt = 0.0
    // 화내기
    var tint: CGFloat = 0, shakeX: CGFloat = 0
    var throwFrom = NSPoint.zero, throwTo = NSPoint.zero, laughed = false
    // 노래
    var song = NowPlaying()
    var songKey = "", lyricIdx = -1, singing = false
    var songLyrics: LyricsEntry?

    // ---------- 시작 ----------
    func start() {
        guard sprites.load() else {
            let a = NSAlert()
            a.messageText = "스프라이트를 불러오지 못했습니다."
            a.runModal()
            NSApp.terminate(nil)
            return
        }
        panel = makeOverlayPanel(NSRect(x: 0, y: 0, width: kFrame * s, height: kFrame * s), mouse: true)
        view.owner = self
        view.frame = NSRect(x: 0, y: 0, width: kFrame * s, height: kFrame * s)
        panel.contentView = view
        bubble = Bubble()
        fire = Fire()
        agent.host = self
        if let src = CGEventSource(stateID: .combinedSessionState) { src.localEventsSuppressionInterval = 0 }

        let vf = (NSScreen.main ?? NSScreen.screens[0]).visibleFrame
        px = vf.midX
        py = vf.minY
        lastTick = now()
        nextCursor = lastTick + cfg.cursorInterval
        schedulePrank(lastTick)
        setupStatusItem()
        registerHotKey()
        say("미야아아~", 2.5)
        panel.orderFrontRegardless()
        let timer = Timer(timeInterval: 1.0 / 60, repeats: true) { [weak self] _ in self?.tick() }
        RunLoop.main.add(timer, forMode: .common)
        NotificationCenter.default.addObserver(forName: NSApplication.didChangeScreenParametersNotification, object: nil,
                                               queue: .main) { [weak self] _ in self?.clampToScreen() }
    }

    func say(_ text: String, _ sec: Double = 1.8) {
        NSLog("say: %@", text)
        if cfg.bubbles { bubble.say(text, sec) }
    }

    func setState(_ st: State) {
        if st == state { return }
        NSLog("state: %@ -> %@", state.rawValue, st.rawValue)
        state = st
        t0 = now()
        frameT = 0
    }

    func schedulePrank(_ t: Double) { nextPrank = t + (cfg.prank == "often" ? rnd(60, 180) : rnd(300, 600)) }

    // ---------- 기하 ----------
    var flipped: Bool { anim == .drag ? side < 0 : facing < 0 }
    var area: NSRect { screenFor(NSPoint(x: px, y: py + 2)).visibleFrame }
    func extents() -> (CGFloat, CGFloat) {
        let m = kMeta[anim.rawValue]
        let l = (m.anchorX - m.left) * s, r = (m.right - m.anchorX) * s
        return flipped ? (r, l) : (l, r)
    }
    func handOffset(_ sd: CGFloat) -> NSPoint {
        let hx = (kGripX - kMeta[Anim.drag.rawValue].anchorX) * s
        return NSPoint(x: sd > 0 ? hx : -hx, y: (kBaseY - kGripY) * s)
    }
    var headTop: NSPoint {
        let m = kMeta[anim.rawValue]
        let cx = px + (flipped ? -1 : 1) * ((m.left + m.right) / 2 - m.anchorX) * s
        return NSPoint(x: cx, y: py + (kBaseY - sprites.top[anim.rawValue]) * s + 2)
    }
    func clampToScreen() {
        let a = area
        let (l, r) = extents()
        px = max(a.minX + l, min(px, a.maxX - r))
        if state == .walk || state == .idle || state == .music { py = a.minY }
    }

    func setAnim(_ a: Anim) {
        if anim != a { anim = a; frame = 0; frameT = 0 }
    }
    func advance(_ dt: Double) {
        frameT += dt
        let d = kMeta[anim.rawValue].durations
        while frameT >= d[frame] { frameT -= d[frame]; frame = (frame + 1) % 4 }
    }

    // 한 걸음씩 목표로. 도착하면 true
    func walkToward(_ tx: CGFloat, _ ty: CGFloat, _ speed: CGFloat, _ dt: Double) -> Bool {
        let dx = tx - px, dy = ty - py, d = hypot(dx, dy), step = speed * CGFloat(dt)
        if abs(dx) > 1 { facing = dx > 0 ? 1 : -1 }
        if d <= step { px = tx; py = ty; return true }
        px += dx / d * step
        py += dy / d * step
        return false
    }

    // ---------- 그리기 ----------
    func render() {
        if !visible {
            if panel.isVisible { panel.orderOut(nil) }
            return
        }
        let size = kFrame * s
        let m = kMeta[anim.rawValue]
        let ax = flipped ? kFrame - m.anchorX : m.anchorX
        let origin = NSPoint(x: (px + shakeX - ax * s).rounded(), y: (py - (kFrame - kBaseY) * s).rounded())
        if panel.frame.size.width != size {
            panel.setContentSize(NSSize(width: size, height: size))
            view.frame = NSRect(x: 0, y: 0, width: size, height: size)
        }
        if panel.frame.origin != origin { panel.setFrameOrigin(origin) }
        let img = sprites.frames[anim.rawValue][frame]
        if view.image !== img || view.flip != flipped || view.tint != tint {
            view.image = img
            view.flip = flipped
            view.tint = tint
            view.needsDisplay = true
        }
        if !panel.isVisible { panel.orderFrontRegardless() }
    }

    // ---------- 마우스로 잡기·때리기 ----------
    func opaqueAt(_ p: NSPoint, in v: NSView) -> Bool {
        var x = Int(p.x / s), y = Int(kFrame) - 1 - Int(p.y / s)
        if flipped { x = Int(kFrame) - 1 - x }
        guard x >= 0, y >= 0, x < Int(kFrame), y < Int(kFrame) else { return false }
        return sprites.alpha[anim.rawValue][frame][y * Int(kFrame) + x] > 60
    }

    func petMouseDown(_ e: NSEvent, in v: NSView) {
        guard state != .angry && state != .angryThrow, opaqueAt(v.convert(e.locationInWindow, from: nil), in: v) else { return }
        pressing = true
        pressPt = NSEvent.mouseLocation
        pressT = now()
    }

    func petMouseDragged(_ e: NSEvent) {
        let m = NSEvent.mouseLocation
        if pressing && state != .held && hypot(m.x - pressPt.x, m.y - pressPt.y) > 6 { beginHeld() }
        if state == .held { trailAdd(m) }
    }

    func petMouseUp(_ e: NSEvent) {
        if state == .held { pressing = false; return drop() }
        guard pressing else { return }
        pressing = false
        // 연달아 누른 횟수: 2번 = 대화창, 3번 이상 = 때리기
        if e.clickCount >= 3 {
            chatPendingAt = 0
            if cfg.angry { beginAngry() } else { say("아야!", 1) }
        } else if e.clickCount == 2 {
            chatPendingAt = now() + NSEvent.doubleClickInterval
        }
    }

    func stopWork() {
        if state == .approach || state == .drag { target = nil }
        if state == .chase || state == .cursorDrag { nextCursor = now() + cfg.cursorInterval }
    }

    func beginHeld() {
        stopWork()
        chatPendingAt = 0
        trail = []
        grabOff = NSPoint(x: px - pressPt.x, y: py - pressPt.y)
        say("하아? 하아?")
        setState(.held)
    }

    func trailAdd(_ p: NSPoint) {
        trail.append((now(), p))
        if trail.count > 32 { trail.removeFirst() }
    }

    func drop() {
        let t = now()
        trailAdd(NSEvent.mouseLocation)
        vx = 0; vy = 0
        if let last = trail.last {
            for smp in trail.reversed() {
                let d = last.t - smp.t
                if d >= 0.04 && d <= 0.15 { vx = (last.p.x - smp.p.x) / CGFloat(d); vy = (last.p.y - smp.p.y) / CGFloat(d) }
                if d > 0.15 { break }
            }
        }
        if trail.count < 2 || t - trail[trail.count - 2].t > 0.12 { vx = 0; vy = 0 }
        let sp = hypot(vx, vy)
        if sp > 5000 { vx *= 5000 / sp; vy *= 5000 / sp }
        thrown = hypot(vx, vy) > 600
        bounces = 0
        if abs(vx) > 1 { facing = vx > 0 ? 1 : -1 }
        if thrown { say("우라우라!", 1.2) }
        setState(.fall)
    }

    func tickFall(_ dt: Double) {
        setAnim(.walk)
        advance(dt * (thrown ? 3 : 2))
        let a = area
        let (l, r) = extents()
        let bodyH = (kBaseY - sprites.top[anim.rawValue]) * s
        let steps = max(1, Int(ceil(hypot(vx, vy) * CGFloat(dt) / 20)))
        let h = CGFloat(dt) / CGFloat(steps)
        for _ in 0..<steps {
            vy -= 2400 * h
            vx *= pow(0.6, h)
            px += vx * h
            py += vy * h
            let mid = NSPoint(x: px, y: py + bodyH / 2)
            if px - l < a.minX && vx < 0 && !NSScreen.screens.contains(where: { $0.frame.contains(NSPoint(x: a.minX - 2, y: mid.y)) }) {
                px = a.minX + l; vx = -vx * 0.55; facing = 1
            }
            if px + r > a.maxX && vx > 0 && !NSScreen.screens.contains(where: { $0.frame.contains(NSPoint(x: a.maxX + 2, y: mid.y)) }) {
                px = a.maxX - r; vx = -vx * 0.55; facing = -1
            }
            if py + bodyH > a.maxY && vy > 0 { py = a.maxY - bodyH; vy = -vy * 0.4 }
            if py <= a.minY && vy < 0 {
                py = a.minY
                if -vy > 700 && bounces < 4 {
                    vy = -vy * 0.38; vx *= 0.7; bounces += 1
                } else {
                    clampToScreen()
                    say(thrown ? "푸루야~" : "푸루!")
                    thrown = false
                    startWalk()
                    return
                }
            }
        }
    }

    // ---------- 때리면 화내고 포인터 던지기 ----------
    func beginAngry() {
        stopWork()
        pressing = false
        say("하아?! 하아?!", 1.6)
        setState(.angry)
    }

    func cursorFree() -> Bool {
        NSEvent.pressedMouseButtons == 0
    }

    func tickAngry(_ t: Double, _ dt: Double) {
        let el = t - t0
        let c = NSEvent.mouseLocation
        if state == .angry {
            setAnim(.walk)
            frame = Int(t / 0.07) % 4
            facing = c.x >= px ? 1 : -1
            shakeX = CGFloat(sin(t * 70) * 3)
            tint = CGFloat(2 + Int(t / 0.12) % 3) / 4
            if el < 1 { return }
            if !cursorFree() { say("흥!"); tint = 0; return startWalk() }
            side = c.x >= px ? 1 : -1
            setAnim(.drag)
            let h = handOffset(side)
            throwFrom = NSPoint(x: px + h.x, y: py + h.y)
            warpCursor(throwFrom)
            let a = area
            let toRight = px < a.midX
            let tx = toRight ? rnd(Double(a.maxX - a.width * 0.3), Double(a.maxX - a.width * 0.08))
                             : rnd(Double(a.minX + a.width * 0.08), Double(a.minX + a.width * 0.3))
            throwTo = NSPoint(x: tx, y: rnd(Double(a.maxY - a.height * 0.55), Double(a.maxY - a.height * 0.2)))
            gBlocked = 0
            laughed = false
            gBlockUntil = t + 0.45 + 0.75 + cfg.blockSec
            if cfg.blockSec > 0 { setInputBlock(true) }
            NSLog("angry: 포인터를 던짐")
            setState(.angryThrow)
            return
        }
        let windup = 0.45, flight = 0.75
        if el < windup {
            setAnim(.drag)
            frame = Int(el / 0.12) % 4
            tint = 1
            shakeX = CGFloat(sin(t * 90) * 2)
            let k = CGFloat(el / windup)
            warpCursor(NSPoint(x: throwFrom.x - side * k * 26, y: throwFrom.y + k * 14))
            return
        }
        if el < windup + flight {
            if anim != .walk { say("우라!!", 1.2) }
            setAnim(.walk)
            frame = 1
            facing = throwTo.x >= px ? 1 : -1
            let u = (el - windup) / flight
            tint = CGFloat(1 - u)
            let e = 1 - (1 - u) * (1 - u)
            let sx = throwFrom.x - side * 26, sy = throwFrom.y + 14
            let arc = area.height * 0.35
            let x = sx + (throwTo.x - sx) * CGFloat(e)
            let y = sy + (throwTo.y - sy) * CGFloat(e) + arc * CGFloat(4 * e * (1 - e))
            warpCursor(NSPoint(x: x, y: min(y, area.maxY - 4)))
            return
        }
        tint = 0
        if !laughed { laughed = true; say("야하! 야하!", 2) }
        setAnim(.dance)
        advance(dt * 1.6)
        let b = el - windup - flight
        warpCursor(NSPoint(x: throwTo.x + CGFloat(sin(b * 23) * 7 + sin(b * 7) * 10),
                           y: throwTo.y - CGFloat(cos(b * 29) * 5) + CGFloat(abs(sin(b * 5)) * 12)))
        if t >= gBlockUntil {
            endAngry("던지기 끝")
            say("이~")
        }
    }

    func endAngry(_ why: String) {
        gBlockUntil = 0
        setInputBlock(false)
        NSLog("angry: 끝 (%@), 막은 입력 %d개", why, gBlocked)
        tint = 0
        vx = 0; vy = 0; thrown = false; bounces = 0
        setState(.fall)
    }

    // ---------- 걷기 ----------
    func startWalk() {
        setState(.walk)
        stateUntil = now() + rnd(4, 10)
        if Bool.random() { facing = -facing }
    }

    // ---------- 창 끌기 ----------
    func beginApproach(_ w: AXWin) {
        guard let f = axFrame(w), let sc = NSScreen.screens.first(where: { $0.frame.intersects(f) }) else { return }
        let vf = sc.visibleFrame
        side = (vf.maxX - f.maxX >= f.minX - vf.minX) ? 1 : -1
        target = w
        NSLog("drag: '%@' 창으로", w.title)
        setState(.approach)
    }

    func gripPoint(_ f: NSRect) -> NSPoint {
        let h = handOffset(side)
        let gx = side > 0 ? f.maxX - 6 : f.minX + 6, gy = f.maxY - 14
        return NSPoint(x: gx - h.x, y: gy - h.y)
    }

    func releaseDrag(_ why: String, _ text: String = "하아?") {
        NSLog("drag: 놓기 (%@)", why)
        target = nil
        say(text)
        vx = 0; vy = 0; thrown = false; bounces = 0
        setState(.release)
    }

    func tickDrag(_ t: Double, _ dt: Double) {
        guard let w = target, let f = axFrame(w) else { return releaseDrag("창이 사라짐") }
        if state == .approach {
            if t - t0 > 20 { return releaseDrag("접근 시간 초과") }
            setAnim(.walk)
            advance(dt * 1.4)
            let g = gripPoint(f)
            if walkToward(g.x, g.y, 45 * 2.2, dt) {
                setAnim(.drag)
                dragDist = CGFloat(rnd(150, 250))
                let vf = screenFor(NSPoint(x: f.midX, y: f.midY)).visibleFrame
                let room = side > 0 ? vf.maxX - f.maxX : f.minX - vf.minX
                dragDist = max(0, min(dragDist, room - 20))
                if dragDist < 30 { return releaseDrag("옮길 공간이 없음", "이~") }
                dragMoved = 0
                lastSetX = f.minX
                say("우라!")
                setState(.drag)
            }
            return
        }
        setAnim(.drag)
        advance(dt)
        if abs(f.minX - lastSetX) > 12 { return releaseDrag("사용자가 창을 움직임", "하아? 하아?") }
        let step = min(70 * CGFloat(dt), dragDist - dragMoved)
        let nx = f.minX + side * step
        if !axMove(w, toNS: NSPoint(x: nx, y: f.minY), height: f.height) { return releaseDrag("창을 옮기지 못함") }
        lastSetX = nx
        dragMoved += step
        let g = gripPoint(NSRect(x: nx, y: f.minY, width: f.width, height: f.height))
        px = g.x; py = g.y
        if dragMoved >= dragDist {
            target = nil
            say("이~")
            setState(.release)
        }
    }

    func maybePrank(_ t: Double, force: Bool) {
        if !force && (cfg.prank == "off" || t < nextPrank) { return }
        schedulePrank(t)
        let ws = axMovableWindows(excludeFrontmost: true)
        if let w = ws.randomElement() {
            say("야하!")
            beginApproach(w)
        } else {
            maybeGrabCursor(t, force: true)
        }
    }

    // ---------- 포인터 가져가기 ----------
    func cursorNear(_ c: NSPoint) -> Bool {
        let (l, r) = extents()
        let top = py + (kBaseY - sprites.top[anim.rawValue]) * s
        return c.x > px - l - 60 && c.x < px + r + 60 && c.y > py - 60 && c.y < top + 60
    }

    func maybeGrabCursor(_ t: Double, force: Bool, nearOnly: Bool = false) {
        guard cfg.cursorGrab || force, cursorFree() else { return }
        let c = NSEvent.mouseLocation
        let near = cursorNear(c)
        if nearOnly && !near { return }
        if !force && !near && t < nextCursor { return }
        if !force && t - cursorStillSince < (near ? 0.25 : 0.5) { return }
        let vf = screenFor(c).visibleFrame
        side = (vf.maxX - c.x >= c.x - vf.minX) ? 1 : -1
        cursorDist = CGFloat(rnd(150, 250))
        cursorMoved = 0
        lastChaseSay = t
        say("야하!")
        setState(.chase)
    }

    func endCursor(_ why: String, _ text: String) {
        NSLog("cursor: 끝 (%@)", why)
        say(text)
        nextCursor = now() + cfg.cursorInterval
        vx = 0; vy = 0; thrown = false; bounces = 0
        setState(.release)
    }

    func tickCursor(_ t: Double, _ dt: Double) {
        let c = NSEvent.mouseLocation
        if state == .chase {
            setAnim(.walk)
            let el = t - t0
            advance(dt * 1.8)
            let h = handOffset(side)
            let mult = min(12, 4 + el * 1.6)
            let arrived = walkToward(c.x - h.x, c.y - h.y, CGFloat(45 * mult), dt)
            if el > 3 && t - lastChaseSay > 4 { lastChaseSay = t; say(Bool.random() ? "우라우라" : "하아?", 1.2) }
            if arrived && cursorFree() && t - cursorStillSince >= 0.12 {
                setAnim(.drag)
                cursorSet = c
                say("우라!")
                setState(.cursorDrag)
            }
            return
        }
        setAnim(.drag)
        advance(dt)
        if NSEvent.pressedMouseButtons != 0 || hypot(c.x - cursorSet.x, c.y - cursorSet.y) > 3 {
            return endCursor("사용자가 움직임", "하아?")
        }
        let step = min(70 * 1.6 * CGFloat(dt), cursorDist - cursorMoved)
        var np = NSPoint(x: cursorSet.x + side * step, y: cursorSet.y)
        let vf = screenFor(np).visibleFrame
        np.x = max(vf.minX + 2, min(np.x, vf.maxX - 2))
        warpCursor(np)
        cursorSet = np
        cursorMoved += step
        let h = handOffset(side)
        px = np.x - h.x; py = np.y - h.y
        if cursorMoved >= cursorDist || step <= 0 { endCursor("다 끌었음", "이~") }
    }

    // ---------- 노래 ----------
    func tickSing(_ t: Double) {
        if t - lastMedia >= 0.5 {
            lastMedia = t
            media.poll(spotifyOnly: cfg.spotifyOnly)
            song = media.current
        }
        let key = song.ok && !song.title.isEmpty ? song.artist + "\n" + song.title : ""
        if key != songKey {
            songKey = key
            lyricIdx = -1
            songLyrics = nil
            if !key.isEmpty && cfg.sing {
                songLyrics = lyrics.get(title: song.title, artist: song.artist, album: song.album, duration: song.duration)
                NSLog("sing: 곡 바뀜 %@ - %@", song.title, song.artist)
                if song.playing && cfg.bubbles { bubble.say("♪ " + song.title, 2.5) }
            }
        }
        singing = cfg.sing && song.ok && song.playing && songLyrics?.status == .ready && !paused
        guard singing, let L = songLyrics?.lines else { return }
        let pos = song.positionAt(t) - cfg.lyricsOffset
        var idx = -1
        for (i, l) in L.enumerated() where l.t <= pos { idx = i }
        if idx == lyricIdx { return }
        lyricIdx = idx
        if idx < 0 || L[idx].text.isEmpty { return }
        let next = idx + 1 < L.count ? L[idx + 1].t : L[idx].t + 5
        if cfg.bubbles && state == .music { bubble.say("♪ " + L[idx].text, min(7, max(1, next - pos - 0.05))) }
    }

    // ---------- 메인 틱 ----------
    func tick() {
        let t = now()
        let dt = min(0.1, max(0, t - lastTick))
        lastTick = t
        let c = NSEvent.mouseLocation
        if c != cursorSeen { cursorSeen = c; cursorStillSince = t }
        shakeX = 0
        if state != .angry && state != .angryThrow { tint = 0 }
        if gTap != nil && t >= gBlockUntil && state != .angryThrow { setInputBlock(false) }
        if chatPendingAt > 0 && t >= chatPendingAt { chatPendingAt = 0; say("야하!"); agent.open() }
        if pressing && state != .held && t - pressT > 0.3 && NSEvent.pressedMouseButtons & 1 != 0 { beginHeld() }
        tickSing(t)
        let music = (cfg.mode != "off" && song.ok && song.playing && (song.spotify || !cfg.spotifyOnly)) || singing

        switch state {
        case .walk, .idle:
            if music { say(cfg.mode == "rap" ? "우라우라!" : "우라!"); setState(.music); break }
            if paused { setAnim(.walk); frame = 0; break }
            if state == .idle {
                setAnim(.walk); frame = 0
                if t > stateUntil { startWalk() }
            } else {
                setAnim(.walk)
                advance(dt)
                px += facing * 45 * CGFloat(dt)
                let a = area
                let (l, r) = extents()
                if px - l < a.minX { px = a.minX + l; facing = 1 }
                if px + r > a.maxX { px = a.maxX - r; facing = -1 }
                py = a.minY
                if t > stateUntil { setState(.idle); stateUntil = t + rnd(1.5, 4) }
            }
            maybePrank(t, force: false)
            if state == .walk || state == .idle { maybeGrabCursor(t, force: false) }
        case .music:
            if !music { startWalk(); break }
            setAnim(singing || cfg.mode == "rap" ? .rap : .dance)
            advance(dt)
            clampToScreen()
            if !paused { maybeGrabCursor(t, force: false, nearOnly: true) }
        case .approach, .drag:
            tickDrag(t, dt)
        case .release:
            setAnim(.walk); frame = 0
            if t - t0 > 0.5 { setState(.fall) }
        case .fall:
            tickFall(dt)
        case .chase, .cursorDrag:
            tickCursor(t, dt)
        case .held:
            setAnim(.walk)
            advance(dt * 2.5)
            let nx = c.x + grabOff.x
            if abs(nx - px) > 1 { facing = nx > px ? 1 : -1 }
            px = nx
            py = c.y + grabOff.y
            trailAdd(c)
            if NSEvent.pressedMouseButtons & 1 == 0 { pressing = false; drop() }  // 놓는 이벤트를 놓친 경우
        case .angry, .angryThrow:
            tickAngry(t, dt)
        }
        render()
        bubble.update(t, anchor: headTop, area: area, show: visible)
        let burning = state == .angryThrow && t - t0 >= 0.4 && t < gBlockUntil
        if burning || fire.visible { fire.update(t, cursor: NSEvent.mouseLocation, burning: burning) }
    }

    // ---------- 메뉴 막대 ----------
    func setupStatusItem() {
        status = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        let img = NSImage(cgImage: sprites.frames[0][0], size: NSSize(width: 22, height: 22))
        status.button?.image = img
        let menu = NSMenu()
        menu.delegate = self
        status.menu = menu
    }

    private func item(_ title: String, _ sel: Selector, _ on: Bool = false, _ obj: Any? = nil, enabled: Bool = true) -> NSMenuItem {
        let i = NSMenuItem(title: title, action: enabled ? sel : nil, keyEquivalent: "")
        i.target = self
        i.state = on ? .on : .off
        i.representedObject = obj
        return i
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        menu.addItem(item("우사기와 대화… (⌃⌥U)", #selector(mChat)))
        menu.addItem(.separator())
        menu.addItem(item(visible ? "캐릭터 숨기기" : "캐릭터 보이기", #selector(mToggleVis)))
        menu.addItem(item("이동 일시정지", #selector(mPause), paused))
        let mm = NSMenu()
        for (k, n) in [("dance", "춤"), ("rap", "랩"), ("off", "끄기")] { mm.addItem(item(n, #selector(mMode(_:)), cfg.mode == k, k)) }
        let mi = NSMenuItem(title: "음악 모드", action: nil, keyEquivalent: "")
        mi.submenu = mm
        menu.addItem(mi)
        let pm = NSMenu()
        for (k, n) in [("off", "끄기"), ("sometimes", "가끔 (5~10분마다)"), ("often", "자주 (1~3분마다)")] {
            pm.addItem(item(n, #selector(mPrank(_:)), cfg.prank == k, k))
        }
        pm.addItem(.separator())
        pm.addItem(item("지금 장난치기", #selector(mPrankNow)))
        let pi = NSMenuItem(title: "장난 모드", action: nil, keyEquivalent: "")
        pi.submenu = pm
        menu.addItem(pi)
        let dm = NSMenu()
        let ws = axMovableWindows(excludeFrontmost: false)
        if !axTrusted(prompt: false) { dm.addItem(item("손쉬운 사용 권한이 필요합니다", #selector(mAX))) }
        else if ws.isEmpty { dm.addItem(item("끌 수 있는 창이 없습니다", #selector(mNoop), enabled: false)) }
        for w in ws.prefix(20) {
            dm.addItem(item(String((w.app + " — " + w.title).prefix(60)), #selector(mDragWin(_:)), false, w.el))
        }
        let di = NSMenuItem(title: "창 선택 후 끌기", action: nil, keyEquivalent: "")
        di.submenu = dm
        menu.addItem(di)
        menu.addItem(item("말풍선", #selector(mBubbles), cfg.bubbles))
        menu.addItem(item("마우스 포인터 가져가기 (\(Int(cfg.cursorInterval))초마다)", #selector(mCursor), cfg.cursorGrab))
        menu.addItem(item("때리면 화내기 (포인터 던짐, \(Int(cfg.blockSec))초 입력 막힘)", #selector(mAngry), cfg.angry))
        menu.addItem(item("스포티파이 노래 따라 부르기", #selector(mSing), cfg.sing))
        menu.addItem(item("Gemini API 키…", #selector(mKey)))
        menu.addItem(item(axTrusted(prompt: false) ? "손쉬운 사용 권한: 허용됨" : "손쉬운 사용 권한 허용하기…", #selector(mAX)))
        menu.addItem(.separator())
        if song.ok {
            var np = "♪ " + song.title + " — " + song.artist
            if np.count > 60 { np = String(np.prefix(57)) + "..." }
            if singing { np += " (따라 부르는 중)" } else if songLyrics?.status == LyricsEntry.Status.none { np += " (가사 없음)" }
            menu.addItem(item(np, #selector(mNoop), enabled: false))
        } else if media.permissionDenied {
            menu.addItem(item("스포티파이 제어 권한이 꺼져 있음 (시스템 설정 > 개인정보 보호 > 자동화)", #selector(mNoop), enabled: false))
        }
        menu.addItem(item("상태: " + state.rawValue, #selector(mNoop), enabled: false))
        menu.addItem(.separator())
        menu.addItem(item("종료", #selector(mQuit)))
    }

    @objc func mNoop() {}
    @objc func mChat() { agent.open() }
    @objc func mToggleVis() { visible.toggle(); if visible { say("미야야야~") } }
    @objc func mPause() { paused.toggle() }
    @objc func mMode(_ i: NSMenuItem) { if let k = i.representedObject as? String { _ = agentSetMusicMode(k) } }
    @objc func mPrank(_ i: NSMenuItem) { if let k = i.representedObject as? String { cfg.prank = k; schedulePrank(now()) } }
    @objc func mPrankNow() { if !busy { maybePrank(now(), force: true) } }
    @objc func mDragWin(_ i: NSMenuItem) {
        guard let o = i.representedObject else { return }
        let el = o as! AXUIElement
        if let w = axMovableWindows(excludeFrontmost: false).first(where: { CFEqual($0.el, el) }), !busy { beginApproach(w) }
    }
    @objc func mBubbles() { cfg.bubbles.toggle() }
    @objc func mCursor() { cfg.cursorGrab.toggle(); nextCursor = now() + cfg.cursorInterval }
    @objc func mAngry() { cfg.angry.toggle() }
    @objc func mSing() { cfg.sing.toggle(); songKey = "" }
    @objc func mKey() { agent.keyDialog() }
    @objc func mAX() {
        if !axTrusted(prompt: true) {
            NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility")!)
        }
    }
    @objc func mQuit() { NSApp.terminate(nil) }

    var busy: Bool {
        [.approach, .drag, .chase, .cursorDrag, .held, .fall, .angry, .angryThrow].contains(state)
    }

    // ---------- 단축키 ⌃⌥U ----------
    func registerHotKey() {
        var ref: EventHotKeyRef?
        let id = EventHotKeyID(signature: OSType(0x5553_4147), id: 1)
        RegisterEventHotKey(UInt32(kVK_ANSI_U), UInt32(controlKey | optionKey), id, GetApplicationEventTarget(), 0, &ref)
        var spec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
        InstallEventHandler(GetApplicationEventTarget(), { _, _, _ -> OSStatus in
            DispatchQueue.main.async { gPet?.agent.open() }
            return noErr
        }, 1, &spec, nil, nil)
    }

    // ---------- AgentHost ----------
    func agentSay(_ text: String, _ seconds: Double) { say(text.count > 160 ? String(text.prefix(157)) + "…" : text, seconds) }
    func agentThinking(_ on: Bool) { if on && cfg.bubbles { bubble.say("음…", 60) } }
    func agentAnchor() -> NSPoint { headTop }
    func agentStatus() -> String {
        var s = state.rawValue + (visible ? ", 보임" : ", 숨김")
        if paused { s += ", 일시정지" }
        s += ", 음악 모드=" + cfg.mode
        return s
    }
    func agentListWindows() -> [String] { axMovableWindows(excludeFrontmost: false).map { $0.app + " — " + $0.title } }
    func agentDragWindow(_ title: String) -> String {
        guard axTrusted(prompt: false) else { return "오류:손쉬운 사용 권한이 필요함 (메뉴 막대 > 우사기 > 손쉬운 사용 권한)" }
        if !visible { return "오류:우사기가 숨겨져 있음" }
        if busy { return "오류:우사기가 다른 일을 하는 중" }
        let want = title.lowercased()
        guard let w = axMovableWindows(excludeFrontmost: false).first(where: {
            ($0.title + " " + $0.app).lowercased().contains(want)
        }) else { return "오류:그런 제목의 끌 수 있는 창이 없음" }
        beginApproach(w)
        return "'\(w.title)' 창으로 걸어가는 중"
    }
    func agentSetMusicMode(_ mode: String) -> String {
        guard ["dance", "rap", "off"].contains(mode) else { return "모르는 모드" }
        if mode != cfg.mode { say("푸루!") }
        cfg.mode = mode
        return "바꿨음: " + mode
    }
    func agentPetAction(_ a: String) -> String {
        if a == "cancel" {
            switch state {
            case .chase, .cursorDrag: endCursor("취소", "하아?")
            case .approach, .drag: releaseDrag("취소")
            default: break
            }
            return "멈췄음"
        }
        if busy { return "다른 일을 하는 중이라 못 함" }
        if a == "prank" { maybePrank(now(), force: true); return "장난 시작" }
        if a == "grab_cursor" { maybeGrabCursor(now(), force: true); return "포인터 쫓아가는 중" }
        return "모르는 동작"
    }
    func agentNowPlaying() -> String {
        if media.permissionDenied { return "스포티파이 제어 권한이 꺼져 있음 (시스템 설정 > 개인정보 보호 및 보안 > 자동화)" }
        guard song.ok else { return "스포티파이(또는 Music)가 꺼져 있거나 재생 중인 곡이 없음" }
        let p = Int(song.positionAt(now())), d = Int(song.duration)
        var s = (song.playing ? "재생 중: " : "멈춤: ") + song.title + " — " + song.artist
        s += String(format: ", %d:%02d / %d:%02d", p / 60, p % 60, d / 60, d % 60)
        if !cfg.sing { s += ", 따라 부르기 꺼짐" }
        else if songLyrics?.status == .ready { s += ", 가사 있음 → 우사기가 따라 부름" }
        else if songLyrics?.status == LyricsEntry.Status.none { s += ", 맞는 가사를 못 찾음" }
        else { s += ", 가사 찾는 중" }
        return s
    }
    func agentMedia(_ action: String) -> String {
        let cmd = ["play": "play", "pause": "pause", "play_pause": "playpause", "next": "next track", "previous": "previous track"][action]
        guard let c = cmd else { return "오류:모르는 동작" }
        if !song.ok, action == "play" || action == "play_pause" {
            NSWorkspace.shared.open(URL(string: "spotify:")!)
            return "스포티파이를 열었음. 사용자가 재생을 눌러야 할 수 있음"
        }
        return media.command(c) ? "보냄: " + action : "오류:스포티파이(또는 Music)를 제어하지 못함"
    }
}

var gPet: Pet?

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ n: Notification) {
        let p = Pet()
        gPet = p
        p.start()
    }
    func applicationWillTerminate(_ n: Notification) {
        gBlockUntil = 0
        setInputBlock(false)
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
