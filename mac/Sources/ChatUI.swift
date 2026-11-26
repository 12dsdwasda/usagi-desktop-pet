import AppKit

// 귀여운 대화창: 말풍선 기록, 분홍 머리 띠, 알약 단추, 창 위에 올라앉은 캐릭터들
enum ChatColor {
    static let bg = NSColor(srgbRed: 1, green: 0.973, blue: 0.953, alpha: 1)
    static let dots = NSColor(srgbRed: 0.988, green: 0.894, blue: 0.918, alpha: 1)
    static let petLine = NSColor(srgbRed: 0.957, green: 0.769, blue: 0.824, alpha: 1)
    static let petText = NSColor(srgbRed: 0.29, green: 0.227, blue: 0.251, alpha: 1)
    static let userFill = NSColor(srgbRed: 1, green: 0.78, blue: 0.839, alpha: 1)
    static let userText = NSColor(srgbRed: 0.36, green: 0.18, blue: 0.235, alpha: 1)
    static let sysFill = NSColor(srgbRed: 0.933, green: 0.91, blue: 1, alpha: 1)
    static let sysText = NSColor(srgbRed: 0.47, green: 0.416, blue: 0.596, alpha: 1)
    static let toolFill = NSColor(srgbRed: 0.867, green: 0.961, blue: 0.925, alpha: 1)
    static let toolText = NSColor(srgbRed: 0.247, green: 0.478, blue: 0.388, alpha: 1)
    static let name = NSColor(srgbRed: 0.839, green: 0.439, blue: 0.573, alpha: 1)
    static let avatarBg = NSColor(srgbRed: 1, green: 0.894, blue: 0.925, alpha: 1)
    static let send = NSColor(srgbRed: 1, green: 0.588, blue: 0.706, alpha: 1)
    static let new = NSColor(srgbRed: 0.549, green: 0.831, blue: 0.729, alpha: 1)
    static let key = NSColor(srgbRed: 0.745, green: 0.675, blue: 0.941, alpha: 1)
}

// 캐릭터별 걷기 프레임 (모든 프레임을 같은 테두리로 자름)
enum ArtCache {
    private static var cache: [String: [CGImage]] = [:]
    static func frames(_ id: String) -> [CGImage] {
        if let c = cache[id] { return c }
        let sub: String? = id == "usagi" ? nil : "characters/" + id
        guard let url = Bundle.main.url(forResource: "walk", withExtension: "png", subdirectory: sub),
              let src = CGImageSourceCreateWithURL(url as CFURL, nil),
              let sheet = CGImageSourceCreateImageAtIndex(src, 0, nil) else { cache[id] = []; return [] }
        let fw = sheet.height, n = max(1, sheet.width / fw), W = sheet.width, H = sheet.height
        var buf = [UInt8](repeating: 0, count: W * H)
        buf.withUnsafeMutableBytes { raw in
            if let ctx = CGContext(data: raw.baseAddress, width: W, height: H, bitsPerComponent: 8, bytesPerRow: W,
                                   space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.alphaOnly.rawValue) {
                ctx.draw(sheet, in: CGRect(x: 0, y: 0, width: W, height: H))
            }
        }
        var x0 = fw, y0 = H, x1 = -1, y1 = -1
        for y in 0..<H {
            for x in 0..<(n * fw) where buf[y * W + x] > 40 {
                let lx = x % fw
                x0 = min(x0, lx); x1 = max(x1, lx); y0 = min(y0, y); y1 = max(y1, y)
            }
        }
        var out: [CGImage] = []
        if x1 >= 0 {
            for i in 0..<n {
                if let c = sheet.cropping(to: CGRect(x: i * fw + x0, y: y0, width: x1 - x0 + 1, height: y1 - y0 + 1)) { out.append(c) }
            }
        }
        cache[id] = out
        return out
    }
}

func roundPath(_ r: NSRect, _ rad: CGFloat) -> NSBezierPath {
    let k = min(rad, min(r.width, r.height) / 2)
    return NSBezierPath(roundedRect: r, xRadius: k, yRadius: k)
}

// ---------- 말풍선 기록 ----------
final class BubbleListView: NSView {
    enum Kind { case user, pet, system, tool }
    struct Msg { let k: Kind; let who: String; let id: String; let text: String; var h: CGFloat = 0; var size = NSSize.zero }
    private(set) var msgs: [Msg] = []
    var typing = false { didSet { relayout() } }
    var typingWho = "", typingId = "usagi"
    private var timer: Timer?
    override var isFlipped: Bool { true }

    let M: CGFloat = 10, AV: CGFloat = 34, PX: CGFloat = 11, PY: CGFloat = 7, GAP: CGFloat = 10, NAMEH: CGFloat = 16
    let body = NSFont.systemFont(ofSize: 13.5), small = NSFont.systemFont(ofSize: 11.5), nameFont = NSFont.boldSystemFont(ofSize: 11.5)

    func add(_ k: Kind, who: String, id: String, text: String) {
        msgs.append(Msg(k: k, who: who, id: id, text: text))
        if msgs.count > 300 { msgs.removeFirst(50) }
        relayout()
    }
    func clear() { msgs = []; relayout() }

    private func attrs(_ m: Msg) -> [NSAttributedString.Key: Any] {
        let isSmall = m.k == .system || m.k == .tool
        let c: NSColor = m.k == .user ? ChatColor.userText : m.k == .pet ? ChatColor.petText : m.k == .tool ? ChatColor.toolText : ChatColor.sysText
        return [.font: isSmall ? small : body, .foregroundColor: c]
    }
    private func pad(_ m: Msg) -> (CGFloat, CGFloat) { m.k == .system || m.k == .tool ? (9, 4) : (PX, PY) }

    private func measure(_ m: inout Msg, _ W: CGFloat) {
        var maxText = m.k == .pet ? W - (2 * M + AV + 8 + 30) : m.k == .user ? W * 0.74 : W * 0.86
        maxText = max(40, maxText - 2 * PX)
        let r = (m.text as NSString).boundingRect(with: NSSize(width: maxText, height: 100000), options: [.usesLineFragmentOrigin],
                                                  attributes: attrs(m))
        let (px, py) = pad(m)
        m.size = NSSize(width: ceil(r.width) + 2 * px + 1, height: ceil(r.height) + 2 * py)
        m.h = m.size.height + GAP + (m.k == .pet ? NAMEH : 0)
        if m.k == .pet { m.h = max(m.h, AV + GAP) }
    }

    func relayout() {
        guard let sv = enclosingScrollView else { return }
        let W = sv.contentSize.width
        var total = M
        for i in msgs.indices { measure(&msgs[i], W); total += msgs[i].h }
        if typing { total += AV + GAP + NAMEH }
        setFrameSize(NSSize(width: W, height: max(total, sv.contentSize.height)))
        needsDisplay = true
        scroll(NSPoint(x: 0, y: max(0, frame.height - sv.contentSize.height)))
        if typing && timer == nil {
            timer = Timer.scheduledTimer(withTimeInterval: 0.13, repeats: true) { [weak self] _ in self?.needsDisplay = true }
        } else if !typing { timer?.invalidate(); timer = nil }
    }

    private func avatar(_ id: String, _ r: NSRect) {
        ChatColor.avatarBg.setFill()
        NSBezierPath(ovalIn: r).fill()
        if let im = ArtCache.frames(id).first, let ctx = NSGraphicsContext.current?.cgContext {
            let iw = CGFloat(im.width), ih = CGFloat(im.height), side = min(iw, ih * 0.8)
            if let head = im.cropping(to: CGRect(x: (iw - side) / 2, y: 0, width: side, height: side)) {
                ctx.saveGState()
                NSBezierPath(ovalIn: r).addClip()
                // 뒤집힌 좌표계라 그림을 한 번 더 뒤집어 그린다
                let d = NSRect(x: r.minX - r.width * 0.06, y: r.minY + r.height * 0.08, width: r.width * 1.12, height: r.height * 1.12)
                ctx.translateBy(x: 0, y: d.maxY + d.minY)
                ctx.scaleBy(x: 1, y: -1)
                ctx.interpolationQuality = .high
                ctx.draw(head, in: d)
                ctx.restoreGState()
            }
        }
        ChatColor.petLine.setStroke()
        let p = NSBezierPath(ovalIn: r)
        p.lineWidth = 2
        p.stroke()
    }

    private func drawMsg(_ m: Msg, _ y: CGFloat, _ W: CGFloat, typingPhase: Int = -1) {
        let (px, py) = pad(m)
        switch m.k {
        case .pet:
            avatar(m.id, NSRect(x: M, y: y, width: AV, height: AV))
            (m.who as NSString).draw(at: NSPoint(x: M + AV + 9, y: y - 1), withAttributes: [.font: nameFont, .foregroundColor: ChatColor.name])
            let b = NSRect(x: M + AV + 8, y: y + NAMEH, width: m.size.width, height: m.size.height)
            NSColor.white.setFill()
            let p = roundPath(b, 12)
            p.fill()
            ChatColor.petLine.setStroke()
            p.lineWidth = 1.6
            p.stroke()
            if typingPhase >= 0 {
                ChatColor.name.setFill()
                for i in 0..<3 {
                    let bounce: CGFloat = typingPhase % 3 == i ? -3 : 0
                    NSBezierPath(ovalIn: NSRect(x: b.minX + px + CGFloat(i) * 11, y: b.midY - 3 + bounce, width: 6, height: 6)).fill()
                }
            } else {
                (m.text as NSString).draw(with: b.insetBy(dx: px, dy: py), options: [.usesLineFragmentOrigin], attributes: attrs(m))
            }
        case .user:
            let b = NSRect(x: W - M - m.size.width, y: y, width: m.size.width, height: m.size.height)
            ChatColor.userFill.setFill()
            roundPath(b, 12).fill()
            (m.text as NSString).draw(with: b.insetBy(dx: px, dy: py), options: [.usesLineFragmentOrigin], attributes: attrs(m))
        case .system, .tool:
            let b = NSRect(x: (W - m.size.width) / 2, y: y, width: m.size.width, height: m.size.height)
            (m.k == .tool ? ChatColor.toolFill : ChatColor.sysFill).setFill()
            roundPath(b, b.height / 2).fill()
            (m.text as NSString).draw(with: b.insetBy(dx: px, dy: py), options: [.usesLineFragmentOrigin], attributes: attrs(m))
        }
    }

    override func draw(_ dirtyRect: NSRect) {
        ChatColor.bg.setFill()
        dirtyRect.fill()
        ChatColor.dots.setFill()
        let step: CGFloat = 26
        var yy = step / 2
        while yy < bounds.height {
            var xx: CGFloat = Int(yy / step) % 2 == 1 ? step : step / 2
            while xx < bounds.width { NSBezierPath(ovalIn: NSRect(x: xx, y: yy, width: 3.5, height: 3.5)).fill(); xx += step }
            yy += step
        }
        var y = M
        for m in msgs {
            if y + m.h >= dirtyRect.minY && y <= dirtyRect.maxY { drawMsg(m, y, bounds.width) }
            y += m.h
        }
        if typing {
            let t = Msg(k: .pet, who: typingWho, id: typingId, text: "", h: 0, size: NSSize(width: 48, height: 30))
            drawMsg(t, y, bounds.width, typingPhase: Int(Date().timeIntervalSince1970 / 0.26))
        }
    }
}

// ---------- 머리 띠 ----------
final class ChatHeaderView: NSView {
    var title = "" { didSet { needsDisplay = true } }
    var onClose: (() -> Void)?
    private var hot = false
    override var isFlipped: Bool { true }
    var closeRect: NSRect { NSRect(x: bounds.width - 36, y: (bounds.height - 24) / 2 - 2, width: 24, height: 24) }

    override func draw(_ dirtyRect: NSRect) {
        NSGradient(starting: NSColor(srgbRed: 1, green: 0.769, blue: 0.839, alpha: 1),
                   ending: NSColor(srgbRed: 1, green: 0.871, blue: 0.808, alpha: 1))?.draw(in: bounds, angle: 0)
        ChatColor.bg.setFill()
        let r: CGFloat = 7
        var x = -r
        while x < bounds.width + r { NSBezierPath(ovalIn: NSRect(x: x, y: bounds.height - r, width: 2 * r, height: 2 * r)).fill(); x += 2 * r }
        ("\u{2661} " + title as NSString).draw(at: NSPoint(x: 14, y: bounds.height / 2 - 11),
                                               withAttributes: [.font: NSFont.boldSystemFont(ofSize: 13.5),
                                                                .foregroundColor: NSColor(srgbRed: 0.478, green: 0.275, blue: 0.36, alpha: 1)])
        (hot ? ChatColor.send : NSColor(white: 1, alpha: 0.67)).setFill()
        NSBezierPath(ovalIn: closeRect).fill()
        (hot ? NSColor.white : ChatColor.name).setStroke()
        let c = closeRect.insetBy(dx: 7, dy: 7), p = NSBezierPath()
        p.move(to: NSPoint(x: c.minX, y: c.minY)); p.line(to: NSPoint(x: c.maxX, y: c.maxY))
        p.move(to: NSPoint(x: c.maxX, y: c.minY)); p.line(to: NSPoint(x: c.minX, y: c.maxY))
        p.lineWidth = 2
        p.lineCapStyle = .round
        p.stroke()
    }
    override func updateTrackingAreas() {
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: bounds, options: [.mouseMoved, .mouseEnteredAndExited, .activeAlways], owner: self))
    }
    override func mouseMoved(with e: NSEvent) {
        let h = closeRect.contains(convert(e.locationInWindow, from: nil))
        if h != hot { hot = h; needsDisplay = true }
    }
    override func mouseExited(with e: NSEvent) { if hot { hot = false; needsDisplay = true } }
    override func mouseDown(with e: NSEvent) {
        if closeRect.contains(convert(e.locationInWindow, from: nil)) { onClose?() } else { window?.performDrag(with: e) }
    }
}

// 입력칸 뒤 흰 알약
final class PillView: NSView {
    override func draw(_ dirtyRect: NSRect) {
        let p = roundPath(bounds.insetBy(dx: 1, dy: 1), bounds.height / 2)
        NSColor.white.setFill()
        p.fill()
        ChatColor.petLine.setStroke()
        p.lineWidth = 1.6
        p.stroke()
    }
}

// 말랑한 알약 단추
final class PillButton: NSButton {
    var fill = ChatColor.send
    override func draw(_ dirtyRect: NSRect) {
        let down = isHighlighted
        let c = isEnabled ? fill : NSColor(srgbRed: 0.886, green: 0.863, blue: 0.875, alpha: 1)
        let shadow = c.blended(withFraction: 0.18, of: .black) ?? c
        let r = bounds.insetBy(dx: 1, dy: 1)
        shadow.setFill()
        roundPath(NSRect(x: r.minX, y: r.minY, width: r.width, height: r.height - 2), (r.height - 2) / 2).fill()
        (down ? shadow : c).setFill()
        let top = NSRect(x: r.minX, y: r.minY + (down ? 1 : 3), width: r.width, height: r.height - 3)
        roundPath(top, top.height / 2).fill()
        let ps = NSMutableParagraphStyle()
        ps.alignment = .center
        let a: [NSAttributedString.Key: Any] = [.font: NSFont.boldSystemFont(ofSize: 12.5), .foregroundColor: NSColor.white, .paragraphStyle: ps]
        let th = (title as NSString).size(withAttributes: a).height
        (title as NSString).draw(in: NSRect(x: top.minX, y: top.midY - th / 2, width: top.width, height: th), withAttributes: a)
    }
}

// ---------- 창 위에 올라앉은 캐릭터들 ----------
final class DecoView: NSView {
    var current: () -> String = { "usagi" }
    var busy: () -> Bool = { false }
    private let t0 = Date()

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        ctx.clear(bounds)
        ctx.interpolationQuality = .high
        let cur = current()
        let ids = [cur] + ["usagi", "hachiware", "chiikawa", "rakko", "shisa", "momonga"].filter { $0 != cur }
        let t = Date().timeIntervalSince(t0), b = busy()
        var x: CGFloat = 12
        for (i, id) in ids.enumerated() {
            let fr = ArtCache.frames(id)
            if fr.isEmpty { continue }
            let main = i == 0
            let im = fr[main ? Int(t * 1000 / 130) % fr.count : 0]
            let ih: CGFloat = main ? 86 : 58, iw = ih * CGFloat(im.width) / CGFloat(im.height)
            if x + iw > bounds.width - 46 { break }
            let bob = CGFloat(abs(sin(t * (b ? 9 : 2.6) + Double(i) * 1.3))) * (b ? 7 : 2.5)
            let r = NSRect(x: x, y: bob, width: iw, height: ih)
            ctx.saveGState()
            if !main {  // 옆 친구들은 주인공 쪽(왼쪽)을 본다
                ctx.translateBy(x: r.midX, y: 0)
                ctx.scaleBy(x: -1, y: 1)
                ctx.translateBy(x: -r.midX, y: 0)
            }
            ctx.draw(im, in: r)
            ctx.restoreGState()
            x += iw - (main ? 2 : 8)
        }
    }
}

final class DecoWindow {
    let panel: NSPanel
    let view = DecoView()
    private var timer: Timer?
    weak var chat: NSWindow?

    init(chat: NSWindow, current: @escaping () -> String, busy: @escaping () -> Bool) {
        self.chat = chat
        panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 10, height: 96), styleMask: [.borderless, .nonactivatingPanel],
                        backing: .buffered, defer: false)
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.ignoresMouseEvents = true
        view.current = current
        view.busy = busy
        panel.contentView = view
        chat.addChildWindow(panel, ordered: .above)
        timer = Timer.scheduledTimer(withTimeInterval: 0.04, repeats: true) { [weak self] _ in self?.tick() }
        RunLoop.main.add(timer!, forMode: .common)
    }

    func tick() {
        guard let c = chat, c.isVisible, !c.isMiniaturized else { return }
        let f = NSRect(x: c.frame.minX, y: c.frame.maxY - 9, width: c.frame.width, height: 96)
        if panel.frame != f { panel.setFrame(f, display: false) }
        view.needsDisplay = true
    }
}
