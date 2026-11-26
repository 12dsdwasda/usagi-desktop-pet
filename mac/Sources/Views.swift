import AppKit

// ---------- 스프라이트 ----------
// held: 마우스(장갑)에 들린 모습. 0~3 위로 들려 흔들림, 4~7 옆으로 끌려감. 우사기는 없어서 걷기 그림을 쓴다.
enum Anim: Int { case walk = 0, dance, rap, drag, held }

struct AnimMeta {
    var durations: [Double]
    var anchorX: CGFloat
    var left: CGFloat
    var right: CGFloat
}

// 고를 수 있는 캐릭터. persona 는 Gemini 에이전트에게 주는 소개·말투.
struct CharInfo { let id: String; let name: String; let persona: String }
let kChars: [CharInfo] = [
    CharInfo(id: "usagi", name: "우사기", persona: "작은 토끼 캐릭터 '우사기'다. 밝고 장난스럽게 말하고, 말버릇(야하!, 우라!, 푸루!, 하아?, 이~)을 가끔 섞어."),
    CharInfo(id: "hachiware", name: "하치와레", persona: "머리 위가 파란 고양이 캐릭터 '하치와레'다. 긍정적이고 다정하며 말을 잘한다. 친구를 응원하듯 말해."),
    CharInfo(id: "chiikawa", name: "치이카와", persona: "작고 하얀 캐릭터 '치이카와'다. 수줍고 겁이 많지만 열심히 한다. 아주 짧게 말하고, 가끔 '와…', '흐에…' 같은 소리를 섞어."),
    CharInfo(id: "rakko", name: "랏코", persona: "이마에 X 흉터가 있는 해달 캐릭터 '랏코'다. 아주 강한 토벌가답게 침착하고 듬직하게, 멋있게 말해."),
    CharInfo(id: "shisa", name: "시사", persona: "사자 갈기가 있는 캐릭터 '시사'다. 성실하고 씩씩하며 열심히 일한다. 기운차게 말해."),
    CharInfo(id: "momonga", name: "모몽가", persona: "파란 꼬리가 복슬복슬한 하늘다람쥐 캐릭터 '모몽가'다. 귀여운 척하며 응석 부리고 조금 제멋대로 말해."),
]

let kFrame: CGFloat = 384
let kBaseY: CGFloat = 344  // 프레임 위에서부터 발바닥까지
private let kUsagiMeta: [AnimMeta] = [
    AnimMeta(durations: [0.14, 0.11, 0.14, 0.11], anchorX: 188, left: 92, right: 291),
    AnimMeta(durations: [0.19, 0.13, 0.19, 0.13], anchorX: 192, left: 69, right: 315),
    AnimMeta(durations: [0.15, 0.10, 0.13, 0.11], anchorX: 183, left: 76, right: 307),
    AnimMeta(durations: [0.24, 0.15, 0.23, 0.16], anchorX: 265, left: 176, right: 365),
]
// 지금 캐릭터의 값 (Sprites.load 가 바꾼다)
var kMeta: [AnimMeta] = kUsagiMeta + [kUsagiMeta[0]]
var kGripX: CGFloat = 178, kGripY: CGFloat = 274
var kHasHeld = false
var kHoldX: CGFloat = 192, kHoldY: CGFloat = 40

// 아주 단순한 ini 읽기 (char.ini 는 ASCII)
private func readIni(_ url: URL) -> [String: [String: String]] {
    guard let text = try? String(contentsOf: url, encoding: .utf8) else { return [:] }
    var out: [String: [String: String]] = [:]
    var sec = ""
    for raw in text.components(separatedBy: .newlines) {
        let l = raw.trimmingCharacters(in: .whitespaces)
        if l.isEmpty || l.hasPrefix(";") { continue }
        if l.hasPrefix("[") && l.hasSuffix("]") { sec = String(l.dropFirst().dropLast()); continue }
        if let eq = l.firstIndex(of: "=") {
            out[sec, default: [:]][String(l[..<eq])] = String(l[l.index(after: eq)...])
        }
    }
    return out
}

final class Sprites {
    var frames: [[CGImage]] = []
    var alpha: [[[UInt8]]] = []  // 프레임별 알파 (행 0 = 위)
    var top: [CGFloat] = []      // 애니메이션별 몸 맨 위 행

    func load(character: String) -> Bool {
        let usagi = character == "usagi"
        let sub: String? = usagi ? nil : "characters/" + character
        var meta = kUsagiMeta + [kUsagiMeta[0]]
        var grip = (CGFloat(178), CGFloat(274)), hold = (CGFloat(192), CGFloat(40))
        let hasHeld = !usagi && Bundle.main.url(forResource: "held", withExtension: "png", subdirectory: sub) != nil
        if !usagi {
            guard let iu = Bundle.main.url(forResource: "char", withExtension: "ini", subdirectory: sub) else { return false }
            let ini = readIni(iu)
            func f(_ s: String, _ k: String, _ d: CGFloat) -> CGFloat { CGFloat(Double(ini[s]?[k] ?? "") ?? Double(d)) }
            for (i, n) in ["walk", "dance", "rap", "drag", "held"].enumerated() {
                let durs = (ini[n]?["durations"] ?? "").split(separator: ",").compactMap { Double($0) }.map { $0 / 1000 }
                if durs.isEmpty { continue }
                meta[i] = AnimMeta(durations: durs, anchorX: f(n, "anchorX", 192), left: f(n, "left", 0), right: f(n, "right", kFrame))
            }
            grip = (f("drag", "gripX", 178), f("drag", "gripY", 274))
            hold = (f("held", "holdX", 192), f("held", "holdY", 40))
        }
        var nf: [[CGImage]] = [], na: [[[UInt8]]] = [], nt: [CGFloat] = []
        for name in ["walk", "dance", "rap", "drag", "held"] {
            if name == "held" && !hasHeld {
                nf.append(nf[0]); na.append(na[0]); nt.append(nt[0])
                continue
            }
            guard let url = Bundle.main.url(forResource: name, withExtension: "png", subdirectory: sub),
                  let src = CGImageSourceCreateWithURL(url as CFURL, nil),
                  let sheet = CGImageSourceCreateImageAtIndex(src, 0, nil) else { return false }
            var fs: [CGImage] = []
            var als: [[UInt8]] = []
            var minRow = Int(kFrame)
            for i in 0..<max(1, sheet.width / Int(kFrame)) {
                guard let f = sheet.cropping(to: CGRect(x: CGFloat(i) * kFrame, y: 0, width: kFrame, height: kFrame)) else { return false }
                fs.append(f)
                let n = Int(kFrame)
                var buf = [UInt8](repeating: 0, count: n * n)
                buf.withUnsafeMutableBytes { raw in
                    if let ctx = CGContext(data: raw.baseAddress, width: n, height: n, bitsPerComponent: 8, bytesPerRow: n,
                                           space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.alphaOnly.rawValue) {
                        ctx.draw(f, in: CGRect(x: 0, y: 0, width: n, height: n))
                    }
                }
                for row in 0..<n where row < minRow {
                    if buf[(row * n)..<(row * n + n)].contains(where: { $0 > 40 }) { minRow = row; break }
                }
                als.append(buf)
            }
            nf.append(fs)
            na.append(als)
            nt.append(CGFloat(minRow))
        }
        if !hasHeld { meta[Anim.held.rawValue] = meta[0] }
        for i in meta.indices where meta[i].durations.count < nf[i].count {
            meta[i].durations += Array(repeating: meta[i].durations.last ?? 0.15, count: nf[i].count - meta[i].durations.count)
        }
        frames = nf; alpha = na; top = nt
        kMeta = meta
        (kGripX, kGripY) = grip
        (kHoldX, kHoldY) = hold
        kHasHeld = hasHeld
        return true
    }
}

// ---------- 우사기 그림 ----------
final class PetView: NSView {
    var image: CGImage?
    var flip = false
    var tint: CGFloat = 0
    weak var owner: Pet?

    override func draw(_ dirtyRect: NSRect) {
        guard let img = image, let ctx = NSGraphicsContext.current?.cgContext else { return }
        ctx.clear(bounds)
        ctx.saveGState()
        if flip {
            ctx.translateBy(x: bounds.width, y: 0)
            ctx.scaleBy(x: -1, y: 1)
        }
        ctx.interpolationQuality = .high
        ctx.draw(img, in: bounds)
        if tint > 0 {  // 화나서 빨개짐
            ctx.setBlendMode(.sourceAtop)
            ctx.setFillColor(NSColor(calibratedRed: 1, green: 0.12, blue: 0.08, alpha: tint * 0.5).cgColor)
            ctx.fill(bounds)
        }
        ctx.restoreGState()
    }

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    override func mouseDown(with event: NSEvent) { owner?.petMouseDown(event, in: self) }
    override func mouseDragged(with event: NSEvent) { owner?.petMouseDragged(event) }
    override func mouseUp(with event: NSEvent) { owner?.petMouseUp(event) }
}

func makeOverlayPanel(_ rect: NSRect, mouse: Bool) -> NSPanel {
    let p = NSPanel(contentRect: rect, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
    p.isOpaque = false
    p.backgroundColor = .clear
    p.hasShadow = false
    p.level = .statusBar
    p.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
    p.ignoresMouseEvents = !mouse
    p.isFloatingPanel = true
    p.hidesOnDeactivate = false
    p.becomesKeyOnlyIfNeeded = true
    return p
}

// ---------- 말풍선 ----------
final class BubbleView: NSView {
    var text = NSAttributedString()
    let pad: CGFloat = 10, tail: CGFloat = 9

    override func draw(_ dirtyRect: NSRect) {
        let r = NSRect(x: 2, y: tail + 2, width: bounds.width - 4, height: bounds.height - tail - 4)
        let path = NSBezierPath(roundedRect: r, xRadius: 11, yRadius: 11)
        let cx = bounds.midX
        let t = NSBezierPath()
        t.move(to: NSPoint(x: cx - tail * 0.8, y: r.minY + 1))
        t.line(to: NSPoint(x: cx, y: 2))
        t.line(to: NSPoint(x: cx + tail * 0.8, y: r.minY + 1))
        t.close()
        NSColor(calibratedRed: 1, green: 0.99, blue: 0.965, alpha: 0.97).setFill()
        path.fill()
        t.fill()
        NSColor(calibratedRed: 0.16, green: 0.12, blue: 0.1, alpha: 1).setStroke()
        path.lineWidth = 2.2
        path.stroke()
        // 꼬리 테두리 (윗변은 말풍선과 붙음)
        let te = NSBezierPath()
        te.move(to: NSPoint(x: cx - tail * 0.8, y: r.minY))
        te.line(to: NSPoint(x: cx, y: 2))
        te.line(to: NSPoint(x: cx + tail * 0.8, y: r.minY))
        te.lineWidth = 2.2
        te.stroke()
        NSColor(calibratedRed: 1, green: 0.99, blue: 0.965, alpha: 1).setFill()
        NSRect(x: cx - tail * 0.8 + 1.2, y: r.minY - 0.5, width: tail * 1.6 - 2.4, height: 2.6).fill()
        text.draw(with: NSRect(x: r.minX + pad, y: r.minY + pad, width: r.width - 2 * pad, height: r.height - 2 * pad),
                  options: [.usesLineFragmentOrigin])
    }
}

final class Bubble {
    let panel: NSPanel
    let view = BubbleView()
    private var until = 0.0, start = 0.0
    private(set) var text = ""

    init() {
        panel = makeOverlayPanel(NSRect(x: 0, y: 0, width: 10, height: 10), mouse: false)
        panel.contentView = view
    }

    func say(_ s: String, _ seconds: Double) {
        text = s
        start = now()
        until = start + seconds
        let font = NSFont.boldSystemFont(ofSize: 14)
        let ps = NSMutableParagraphStyle()
        ps.lineBreakMode = .byWordWrapping
        view.text = NSAttributedString(string: s, attributes: [.font: font, .foregroundColor: NSColor(calibratedWhite: 0.13, alpha: 1),
                                                               .paragraphStyle: ps])
        let b = view.text.boundingRect(with: NSSize(width: 300, height: 2000), options: [.usesLineFragmentOrigin])
        let w = ceil(b.width) + 2 * view.pad + 6, h = ceil(b.height) + 2 * view.pad + view.tail + 6
        panel.setContentSize(NSSize(width: w, height: h))
        view.frame = NSRect(x: 0, y: 0, width: w, height: h)
        view.needsDisplay = true
    }

    func update(_ t: Double, anchor: NSPoint, area: NSRect, show: Bool) {
        if text.isEmpty || t >= until || !show {
            if panel.isVisible { panel.orderOut(nil) }
            if t >= until { text = "" }
            return
        }
        let age = t - start, left = until - t
        panel.alphaValue = CGFloat(max(0, min(1, min(age / 0.12, left / 0.25))))
        let rise = CGFloat(1 - min(1, age / 0.15)) * 6
        let size = panel.frame.size
        var x = anchor.x - size.width / 2
        x = max(area.minX + 2, min(x, area.maxX - size.width - 2))
        var y = anchor.y - rise
        y = min(y, area.maxY - size.height - 2)
        panel.setFrameOrigin(NSPoint(x: x, y: y))
        if !panel.isVisible { panel.orderFrontRegardless() }
    }
}

// ---------- 포인터 불꽃 ----------
final class FireView: NSView {
    struct P { var x, y, vx, vy, born, life, r: Double }
    var ps: [P] = []
    var origin = NSPoint.zero
    var t = 0.0

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        ctx.clear(bounds)
        ctx.setBlendMode(.plusLighter)
        for p in ps {
            let a = (t - p.born) / p.life
            let rad = p.r * (1 - 0.55 * a)
            let col: (CGFloat, CGFloat, CGFloat) = a < 0.18 ? (1, 0.97, 0.7) : a < 0.45 ? (1, 0.62, 0.12) : (0.95, 0.25, 0.05)
            let alpha = CGFloat(pow(1 - a, 1.2) * 0.75)
            let x = p.x - Double(origin.x), y = p.y - Double(origin.y)
            // 부드러운 원: 겹친 동심원
            for k in 0..<3 {
                let f = 1.0 - Double(k) * 0.3
                ctx.setFillColor(NSColor(calibratedRed: col.0, green: col.1, blue: col.2, alpha: alpha * 0.4).cgColor)
                ctx.fillEllipse(in: CGRect(x: x - rad * f, y: y - rad * f, width: 2 * rad * f, height: 2 * rad * f))
            }
        }
    }
}

final class Fire {
    let panel: NSPanel
    let view = FireView()
    private var last = 0.0, acc = 0.0

    init() {
        panel = makeOverlayPanel(NSRect(x: 0, y: 0, width: 280, height: 340), mouse: false)
        panel.level = .screenSaver
        view.frame = NSRect(x: 0, y: 0, width: 280, height: 340)
        panel.contentView = view
    }

    var visible: Bool { panel.isVisible }

    func update(_ t: Double, cursor c: NSPoint, burning: Bool) {
        let dt = last > 0 ? min(0.05, max(0, t - last)) : 0
        last = t
        if burning {
            acc += dt * 420
            while acc >= 1 {
                acc -= 1
                let py = Double.random(in: -4...26), px = Double.random(in: -6...max(-5, py * 0.75 + 4))
                if Double.random(in: 0...1) < 0.08 {
                    view.ps.append(.init(x: c.x + px, y: c.y - py, vx: .random(in: -120...120), vy: .random(in: 200...380), born: t,
                                         life: .random(in: 0.4...0.8), r: .random(in: 2.5...4)))
                } else {
                    view.ps.append(.init(x: c.x + px, y: c.y - py, vx: .random(in: -40...40), vy: .random(in: 90...260), born: t,
                                         life: .random(in: 0.35...0.8), r: .random(in: 9...17)))
                }
            }
        } else { acc = 0 }
        for i in view.ps.indices {
            view.ps[i].vy += 90 * dt
            view.ps[i].vx *= pow(0.3, dt)
            view.ps[i].x += view.ps[i].vx * dt
            view.ps[i].y += view.ps[i].vy * dt
        }
        view.ps.removeAll { t - $0.born >= $0.life }
        if view.ps.isEmpty {
            if panel.isVisible { panel.orderOut(nil) }
            return
        }
        let o = NSPoint(x: c.x - 140, y: c.y - 85)
        panel.setFrameOrigin(o)
        view.origin = o
        view.t = t
        view.needsDisplay = true
        if !panel.isVisible { panel.orderFrontRegardless() }
    }
}
