import AppKit
import ApplicationServices

// 다른 앱 창 옮기기·입력 막기는 '손쉬운 사용' 권한이 있어야 한다.
func axTrusted(prompt: Bool) -> Bool {
    let key = kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String
    return AXIsProcessTrustedWithOptions([key: prompt] as CFDictionary)
}

struct AXWin {
    let el: AXUIElement
    let pid: pid_t
    let app: String
    let title: String
}

private func axCopy(_ el: AXUIElement, _ a: String) -> CFTypeRef? {
    var v: CFTypeRef?
    return AXUIElementCopyAttributeValue(el, a as CFString, &v) == .success ? v : nil
}

private func axValue<T>(_ el: AXUIElement, _ a: String, _ type: AXValueType, _ initial: T) -> T? {
    guard let v = axCopy(el, a), CFGetTypeID(v) == AXValueGetTypeID() else { return nil }
    var out = initial
    return AXValueGetValue(v as! AXValue, type, &out) ? out : nil
}

var primaryHeight: CGFloat { NSScreen.screens.first?.frame.height ?? 0 }

// 창 테두리 (AppKit 좌표: 왼쪽 아래 원점)
func axFrame(_ w: AXWin) -> NSRect? {
    guard let p = axValue(w.el, kAXPositionAttribute, .cgPoint, CGPoint.zero),
          let s = axValue(w.el, kAXSizeAttribute, .cgSize, CGSize.zero) else { return nil }
    return NSRect(x: p.x, y: primaryHeight - p.y - s.height, width: s.width, height: s.height)
}

// AppKit 좌표의 창 왼쪽 아래 → AX 위치(왼쪽 위)
@discardableResult
func axMove(_ w: AXWin, toNS origin: NSPoint, height: CGFloat) -> Bool {
    var p = CGPoint(x: origin.x, y: primaryHeight - origin.y - height)
    guard let v = AXValueCreate(.cgPoint, &p) else { return false }
    return AXUIElementSetAttributeValue(w.el, kAXPositionAttribute as CFString, v) == .success
}

func axMovableWindows(excludeFrontmost: Bool) -> [AXWin] {
    guard axTrusted(prompt: false) else { return [] }
    let me = ProcessInfo.processInfo.processIdentifier
    let front = NSWorkspace.shared.frontmostApplication?.processIdentifier
    var out: [AXWin] = []
    for app in NSWorkspace.shared.runningApplications where app.activationPolicy == .regular && !app.isHidden {
        let pid = app.processIdentifier
        if pid == me || (excludeFrontmost && pid == front) { continue }
        let ae = AXUIElementCreateApplication(pid)
        AXUIElementSetMessagingTimeout(ae, 0.5)
        guard let ws = axCopy(ae, kAXWindowsAttribute) as? [AXUIElement] else { continue }
        for el in ws {
            if (axCopy(el, kAXMinimizedAttribute) as? Bool) == true { continue }
            if (axCopy(el, "AXFullScreen") as? Bool) == true { continue }
            if let sr = axCopy(el, kAXSubroleAttribute) as? String, sr != (kAXStandardWindowSubrole as String) { continue }
            let w = AXWin(el: el, pid: pid, app: app.localizedName ?? "", title: (axCopy(el, kAXTitleAttribute) as? String) ?? "")
            guard let f = axFrame(w), f.width >= 120, f.height >= 80 else { continue }
            // 화면을 꽉 채운(최대화) 창은 제외
            if let sc = NSScreen.screens.first(where: { $0.frame.intersects(f) }), f.width >= sc.visibleFrame.width - 4,
               f.height >= sc.visibleFrame.height - 4 { continue }
            out.append(w)
        }
    }
    return out
}

// ---------- 던진 뒤 입력 막기 ----------
var gBlockUntil = 0.0
var gBlocked = 0
var gTap: CFMachPort?
private var gTapSource: CFRunLoopSource?

private let tapCallback: CGEventTapCallBack = { _, type, event, _ in
    if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
        if let t = gTap { CGEvent.tapEnable(tap: t, enable: true) }
        return Unmanaged.passUnretained(event)
    }
    if now() < gBlockUntil {
        gBlocked += 1
        return nil  // 삼킴
    }
    return Unmanaged.passUnretained(event)
}

// 마우스 이동은 권한 없이 막고(포인터와 마우스 분리), 클릭·키보드는 손쉬운 사용 권한이 있을 때 이벤트 탭으로 막는다
func setInputBlock(_ on: Bool) {
    CGAssociateMouseAndMouseCursorPosition(on ? 0 : 1)
    if on, gTap == nil, axTrusted(prompt: false) {
        var mask: CGEventMask = 0
        for t: CGEventType in [.keyDown, .leftMouseDown, .rightMouseDown, .otherMouseDown, .scrollWheel] { mask |= 1 << t.rawValue }
        if let tap = CGEvent.tapCreate(tap: .cgSessionEventTap, place: .headInsertEventTap, options: .defaultTap,
                                       eventsOfInterest: mask, callback: tapCallback, userInfo: nil) {
            gTap = tap
            gTapSource = CFMachPortCreateRunLoopSource(nil, tap, 0)
            CFRunLoopAddSource(CFRunLoopGetMain(), gTapSource, .commonModes)
            CGEvent.tapEnable(tap: tap, enable: true)
        }
    }
    if !on, let tap = gTap {
        CGEvent.tapEnable(tap: tap, enable: false)
        if let s = gTapSource { CFRunLoopRemoveSource(CFRunLoopGetMain(), s, .commonModes) }
        CFMachPortInvalidate(tap)
        gTap = nil
        gTapSource = nil
    }
}
