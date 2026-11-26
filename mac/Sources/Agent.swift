import AppKit
import IOKit.ps
import Security

// ---------- Gemini API 키 (키체인) ----------
enum GeminiKey {
    static let service = "UsagiDesktopPet", account = "gemini"
    static func load() -> String? {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                kSecAttrAccount as String: account, kSecReturnData as String: true,
                                kSecMatchLimit as String: kSecMatchLimitOne]
        var r: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &r) == errSecSuccess, let d = r as? Data,
              let s = String(data: d, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines), !s.isEmpty else {
            return ProcessInfo.processInfo.environment["GEMINI_API_KEY"]
        }
        return s
    }
    static func save(_ key: String) -> Bool {
        let base: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                   kSecAttrAccount as String: account]
        SecItemDelete(base as CFDictionary)
        let k = key.trimmingCharacters(in: .whitespacesAndNewlines)
        if k.isEmpty { return true }
        var add = base
        add[kSecValueData as String] = Data(k.utf8)
        return SecItemAdd(add as CFDictionary, nil) == errSecSuccess
    }
}

// ---------- 에이전트가 우사기에게 시키는 일 ----------
protocol AgentHost: AnyObject {
    func agentSay(_ text: String, _ seconds: Double)
    func agentThinking(_ on: Bool)
    func agentName() -> String
    func agentPersona() -> String
    func agentCharId() -> String
    func agentAnchor() -> NSPoint
    func agentStatus() -> String
    func agentListWindows() -> [String]
    func agentDragWindow(_ title: String) -> String
    func agentSetMusicMode(_ mode: String) -> String
    func agentPetAction(_ action: String) -> String
    func agentNowPlaying() -> String
    func agentMedia(_ action: String) -> String
}

private let kTools = """
[{"functionDeclarations":[
{"name":"open_url","description":"기본 브라우저로 웹 주소(http/https)를 연다.","parameters":{"type":"object","properties":{"url":{"type":"string"}},"required":["url"]}},
{"name":"search_web","description":"브라우저에서 구글 검색 결과를 연다. 검색 결과 내용은 돌려받지 못한다.","parameters":{"type":"object","properties":{"query":{"type":"string"}},"required":["query"]}},
{"name":"open_app","description":"자주 쓰는 Mac 앱을 실행한다.","parameters":{"type":"object","properties":{"app":{"type":"string","enum":["safari","calculator","textedit","finder","settings","notes","music","spotify","screenshot"]}},"required":["app"]}},
{"name":"open_folder","description":"사용자 폴더를 Finder 로 연다.","parameters":{"type":"object","properties":{"folder":{"type":"string","enum":["desktop","documents","downloads","pictures","music","movies"]}},"required":["folder"]}},
{"name":"set_timer","description":"타이머/알림. 시간이 되면 우사기가 말풍선과 소리로 알려준다. 프로그램이 꺼지면 사라진다.","parameters":{"type":"object","properties":{"seconds":{"type":"number","description":"1~86400초"},"message":{"type":"string"}},"required":["seconds","message"]}},
{"name":"list_timers","description":"대기 중인 타이머 목록.","parameters":{"type":"object","properties":{}}},
{"name":"get_status","description":"현재 날짜와 시각, 배터리, 켜진 시간, 우사기 상태.","parameters":{"type":"object","properties":{}}},
{"name":"list_windows","description":"우사기가 끌고 갈 수 있는 창 목록 (손쉬운 사용 권한 필요).","parameters":{"type":"object","properties":{}}},
{"name":"drag_window","description":"우사기가 걸어가서 제목이나 앱 이름에 이 글자가 들어간 창을 끌고 간다.","parameters":{"type":"object","properties":{"title_contains":{"type":"string"}},"required":["title_contains"]}},
{"name":"set_music_mode","description":"음악이 나올 때 우사기의 반응.","parameters":{"type":"object","properties":{"mode":{"type":"string","enum":["dance","rap","off"]}},"required":["mode"]}},
{"name":"media_control","description":"스포티파이(또는 Apple Music) 재생과 시스템 볼륨 조절.","parameters":{"type":"object","properties":{"action":{"type":"string","enum":["play","pause","play_pause","next","previous","volume_up","volume_down","mute"]}},"required":["action"]}},
{"name":"spotify_now_playing","description":"지금 재생 중인 곡, 가수, 재생 위치, 우사기가 따라 부를 가사가 있는지.","parameters":{"type":"object","properties":{}}},
{"name":"spotify_search","description":"스포티파이 앱에서 검색을 연다. 재생은 사용자가 결과에서 눌러야 한다.","parameters":{"type":"object","properties":{"query":{"type":"string"}},"required":["query"]}},
{"name":"write_note","description":"문서 폴더의 '우사기 메모'에 텍스트 파일로 저장하고 텍스트 편집기로 연다.","parameters":{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}},
{"name":"copy_to_clipboard","description":"텍스트를 클립보드에 복사한다.","parameters":{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}},
{"name":"read_clipboard","description":"클립보드의 텍스트를 읽는다. 사용자가 분명히 요청했을 때만 쓴다.","parameters":{"type":"object","properties":{}}},
{"name":"pet_action","description":"우사기의 장난 동작. prank=창 끌기 장난, grab_cursor=마우스 포인터 가져가기, cancel=하던 일 멈추기.","parameters":{"type":"object","properties":{"action":{"type":"string","enum":["prank","grab_cursor","cancel"]}},"required":["action"]}}
]}]
"""

final class Agent: NSObject, NSWindowDelegate {
    weak var host: AgentHost?
    var models = ["gemini-3-flash-preview", "gemini-flash-latest", "gemini-3.5-flash"]
    private var history: [[String: Any]] = []
    private var name: String { host?.agentName() ?? "우사기" }
    private var busy = false, gen = 0, rounds = 0
    private var timers: [(id: Int, due: Double, msg: String, timer: Timer)] = []
    private var nextTimer = 1

    private var window: NSPanel?
    private var log: BubbleListView!
    private var header: ChatHeaderView!
    private var deco: DecoWindow?
    private var input: NSTextField!
    private var sendBtn: NSButton!

    // ---------- 대화창 ----------
    func open() {
        if window == nil { build() }
        guard let w = window else { return }
        NSApp.activate(ignoringOtherApps: true)
        w.makeKeyAndOrderFront(nil)
        w.makeFirstResponder(input)
    }

    private func build() {
        let a = host?.agentAnchor() ?? NSPoint(x: 400, y: 200)
        let sc = NSScreen.screens.first { $0.frame.contains(a) } ?? NSScreen.main!
        let vf = sc.visibleFrame
        let W: CGFloat = 460, H: CGFloat = 400, HH: CGFloat = 42, m: CGFloat = 10, bar: CGFloat = 36
        var x = a.x + (vf.maxX - a.x >= a.x - vf.minX ? 170 : -170 - W)
        x = max(vf.minX, min(x, vf.maxX - W))
        let rect = NSRect(x: x, y: vf.minY + 12, width: W, height: min(H, vf.height - 110))
        let w = NSPanel(contentRect: rect, styleMask: [.titled, .closable, .resizable, .fullSizeContentView, .utilityWindow],
                        backing: .buffered, defer: false)
        w.title = name + "와 대화"
        w.titleVisibility = .hidden
        w.titlebarAppearsTransparent = true
        for b in [NSWindow.ButtonType.closeButton, .miniaturizeButton, .zoomButton] { w.standardWindowButton(b)?.isHidden = true }
        w.isMovableByWindowBackground = false
        w.backgroundColor = ChatColor.bg
        w.appearance = NSAppearance(named: .aqua)
        w.minSize = NSSize(width: 360, height: 280)
        w.level = .floating
        w.isReleasedWhenClosed = false
        w.hidesOnDeactivate = false
        w.delegate = self
        let cv = NSView(frame: NSRect(x: 0, y: 0, width: W, height: rect.height))
        cv.wantsLayer = true
        cv.layer?.backgroundColor = ChatColor.bg.cgColor
        header = ChatHeaderView(frame: NSRect(x: 0, y: rect.height - HH, width: W, height: HH))
        header.autoresizingMask = [.width, .minYMargin]
        header.title = name + "와 대화"
        header.onClose = { [weak w] in w?.orderOut(nil) }
        let scroll = NSScrollView(frame: NSRect(x: 0, y: bar + 2 * m, width: W, height: rect.height - HH - bar - 2 * m - 2))
        scroll.autoresizingMask = [.width, .height]
        scroll.hasVerticalScroller = true
        scroll.scrollerStyle = .overlay
        scroll.drawsBackground = true
        scroll.backgroundColor = ChatColor.bg
        scroll.borderType = .noBorder
        log = BubbleListView(frame: NSRect(x: 0, y: 0, width: scroll.contentSize.width, height: scroll.contentSize.height))
        log.autoresizingMask = [.width]
        scroll.documentView = log
        scroll.contentView.postsBoundsChangedNotifications = true
        NotificationCenter.default.addObserver(forName: NSView.frameDidChangeNotification, object: scroll, queue: .main) { [weak self] _ in
            self?.log?.relayout()
        }
        scroll.postsFrameChangedNotifications = true
        let bw: CGFloat = 66, kw: CGFloat = 44, gap: CGFloat = 6
        let pillW = W - 2 * m - 2 * bw - kw - 3 * gap
        let pill = PillView(frame: NSRect(x: m, y: m + 2, width: pillW, height: bar - 2))
        pill.autoresizingMask = [.width]
        input = NSTextField(frame: NSRect(x: m + 14, y: m + 2 + (bar - 2 - 20) / 2, width: pillW - 28, height: 20))
        input.autoresizingMask = [.width]
        input.isBordered = false
        input.drawsBackground = false
        input.focusRingType = .none
        input.font = NSFont.systemFont(ofSize: 13.5)
        input.textColor = ChatColor.petText
        input.placeholderString = name + "한테 말하기 (Enter)"
        input.target = self
        input.action = #selector(send)
        let sendB = PillButton(title: "보내기", target: self, action: #selector(send))
        let newB = PillButton(title: "새 대화", target: self, action: #selector(newChat))
        let keyB = PillButton(title: "키", target: self, action: #selector(keyDialog))
        sendB.fill = ChatColor.send; newB.fill = ChatColor.new; keyB.fill = ChatColor.key
        sendBtn = sendB
        var bx = W - m - kw - 2 * bw - 2 * gap
        for (b, bwid) in [(sendB, bw), (newB, bw), (keyB, kw)] {
            b.isBordered = false
            b.frame = NSRect(x: bx, y: m, width: bwid, height: bar)
            b.autoresizingMask = [.minXMargin]
            cv.addSubview(b)
            bx += bwid + gap
        }
        cv.addSubview(scroll)
        cv.addSubview(pill)
        cv.addSubview(input)
        cv.addSubview(header)
        w.contentView = cv
        window = w
        deco = DecoWindow(chat: w, current: { [weak self] in self?.host?.agentCharId() ?? "usagi" },
                          busy: { [weak self] in self?.busy ?? false })
        newChat()
    }

    private func append(_ s: String) {
        guard let log = log else { return }
        if s.hasPrefix("나: ") {
            log.add(.user, who: "나", id: "", text: String(s.dropFirst(3)))
        } else if s.hasPrefix(name + ": ") {
            log.add(.pet, who: name, id: host?.agentCharId() ?? "usagi", text: String(s.dropFirst(name.count + 2)))
        } else if s.hasPrefix("  [") {
            log.add(.tool, who: "", id: "", text: String(s.dropFirst(2)))
        } else {
            var t = s
            if t.hasPrefix("(") && t.hasSuffix(")") { t = String(t.dropFirst().dropLast()) }
            log.add(.system, who: "", id: "", text: t)
        }
    }

    private func updateUi() {
        sendBtn?.isEnabled = !busy
        sendBtn?.needsDisplay = true
        let t = name + (busy ? "와 대화 · 생각 중…" : "와 대화")
        window?.title = t
        header?.title = t
        log?.typingWho = name
        log?.typingId = host?.agentCharId() ?? "usagi"
        if log?.typing != busy { log?.typing = busy }
        input?.placeholderString = name + "한테 말하기 (Enter)"
    }

    @objc func newChat() {
        history = []
        gen += 1
        busy = false
        rounds = 0
        log?.clear()
        append("새 대화 시작! " + name + "한테 뭐든 말해 봐. 예: \"유튜브 열어줘\", \"10분 뒤에 물 마시라고 알려줘\", \"지금 무슨 노래야?\"")
        host?.agentThinking(false)
        updateUi()
    }

    @objc func send() {
        let t = input.stringValue
        if busy { return }
        input.stringValue = ""
        submit(t)
    }

    @objc func keyDialog() {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "Gemini API 키"
        alert.informativeText = "Google AI Studio(aistudio.google.com/apikey)에서 받은 키를 붙여 넣어. 키체인에 저장돼. 비우고 저장하면 지워져."
        let f = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 320, height: 24))
        f.placeholderString = GeminiKey.load() == nil ? "키 붙여 넣기" : "(저장된 키 있음 — 바꾸려면 새로 입력)"
        alert.accessoryView = f
        alert.addButton(withTitle: "저장")
        alert.addButton(withTitle: "취소")
        alert.window.initialFirstResponder = f
        if alert.runModal() == .alertFirstButtonReturn {
            let ok = GeminiKey.save(f.stringValue)
            append(ok ? (f.stringValue.isEmpty ? "(API 키를 지웠어.)" : "(API 키를 키체인에 저장했어.)") : "(API 키를 저장하지 못했어.)")
        }
    }

    func submit(_ raw: String) {
        let text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.isEmpty { return }
        if busy { append("(아직 생각 중이야. 잠깐만!)"); return }
        if GeminiKey.load() == nil {
            append("(Gemini API 키가 없어. '키' 버튼으로 먼저 넣어 줘.)")
            keyDialog()
            if GeminiKey.load() == nil { return }
        }
        append("나: " + text)
        while history.count > 40 {
            history.removeFirst()
            while let f = history.first, !isUserText(f) { history.removeFirst() }
        }
        history.append(["role": "user", "parts": [["text": text]]])
        rounds = 0
        request()
    }

    private func isUserText(_ c: [String: Any]) -> Bool {
        guard c["role"] as? String == "user", let p = c["parts"] as? [[String: Any]], let f = p.first else { return false }
        return f["text"] != nil
    }

    private func systemPrompt() -> String {
        let df = DateFormatter()
        df.locale = Locale(identifier: "ko_KR")
        df.dateFormat = "yyyy-MM-dd (E) HH:mm"
        return """
        너는 사용자의 Mac 바탕화면 위를 돌아다니는 \(host?.agentPersona() ?? "")
        너는 동시에 사용자를 돕는 에이전트다.
        - 항상 한국어 반말로 말해.
        - 답은 말풍선에 나오니 보통 1~2문장, 80자 안쪽으로 짧게. 사용자가 자세한 설명을 원할 때만 길게.
        - 마크다운, 표, 이모지는 쓰지 마.
        - 컴퓨터에서 할 일은 도구로 직접 해. 도구로 못 하는 일(파일 삭제, 설치, 결제, 로그인, 메일 보내기 등)은 못 한다고 솔직히 말해.
        - 하지 않은 일을 했다고 말하지 마. 도구 결과에 오류가 있으면 그대로 알려.
        - 웹 검색은 브라우저에 결과를 띄우는 것까지만 할 수 있다.
        - 스포티파이에서 노래가 나오면 너는 가사를 말풍선으로 따라 부른다.
        - 지금 시각: \(df.string(from: Date()))
        """
    }

    private func request() {
        guard let key = GeminiKey.load() else { return }
        let tools = (try? JSONSerialization.jsonObject(with: Data(kTools.utf8))) ?? []
        let body: [String: Any] = ["systemInstruction": ["parts": [["text": systemPrompt()]]], "contents": history, "tools": tools]
        guard let data = try? JSONSerialization.data(withJSONObject: body) else { return }
        busy = true
        updateUi()
        host?.agentThinking(true)
        attempt(0, data, key, gen)
    }

    private func attempt(_ i: Int, _ body: Data, _ key: String, _ g: Int) {
        let m = models[i]
        var r = URLRequest(url: URL(string: "https://generativelanguage.googleapis.com/v1beta/models/\(m):generateContent")!,
                           timeoutInterval: 60)
        r.httpMethod = "POST"
        r.setValue("application/json; charset=utf-8", forHTTPHeaderField: "Content-Type")
        r.setValue(key, forHTTPHeaderField: "x-goog-api-key")
        r.httpBody = body
        URLSession.shared.dataTask(with: r) { data, resp, err in
            let st = (resp as? HTTPURLResponse)?.statusCode ?? 0
            DispatchQueue.main.async {
                guard g == self.gen else { return }
                if [0, 404, 429, 500, 503, 504].contains(st) && i + 1 < self.models.count {
                    NSLog("agent: %@ → HTTP %d, 다음 모델", m, st)
                    return self.attempt(i + 1, body, key, g)
                }
                self.onResult(st, data, err)
            }
        }.resume()
    }

    private func fail(_ why: String) {
        append("(문제가 생겼어: \(why))")
        busy = false
        host?.agentThinking(false)
        host?.agentSay("하아?", 2)
        while let l = history.last, l["role"] as? String == "user" { history.removeLast() }
        updateUi()
    }

    private func onResult(_ st: Int, _ data: Data?, _ err: Error?) {
        busy = false
        host?.agentThinking(false)
        let j = data.flatMap { try? JSONSerialization.jsonObject(with: $0) } as? [String: Any]
        if st != 200 {
            var why = st == 0 ? (err?.localizedDescription ?? "연결 실패") : "HTTP \(st)"
            if let e = j?["error"] as? [String: Any], let m = e["message"] as? String { why += " " + String(m.prefix(200)) }
            if [400, 401, 403].contains(st) { why += " — API 키를 확인해 줘." }
            return fail(why)
        }
        guard let cands = j?["candidates"] as? [[String: Any]], let cand = cands.first,
              let content = cand["content"] as? [String: Any], let parts = content["parts"] as? [[String: Any]], !parts.isEmpty else {
            return fail("빈 답")
        }
        history.append(content)  // thoughtSignature 까지 그대로 돌려줘야 한다
        var text = ""
        var calls: [[String: Any]] = []
        for p in parts {
            if p["thought"] as? Bool == true { continue }
            if let t = p["text"] as? String { text += t }
            if let fc = p["functionCall"] as? [String: Any] { calls.append(fc) }
        }
        text = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if !text.isEmpty {
            append(name + ": " + text)
            host?.agentSay(text, min(14, max(3, 2.5 + Double(text.count) * 0.07)))
        }
        if !calls.isEmpty {
            if rounds >= 6 { return fail("도구를 너무 많이 써서 멈춤") }
            rounds += 1
            var rparts: [[String: Any]] = []
            for fc in calls {
                let name = fc["name"] as? String ?? ""
                let args = fc["args"] as? [String: Any] ?? [:]
                let result = runTool(name, args)
                let summary = (result["error"] as? String).map { "실패: " + $0 } ?? (result["result"] as? String ?? "")
                append("  [\(name)] " + String(summary.prefix(120)))
                var fr: [String: Any] = ["name": name, "response": result]
                if let id = fc["id"] { fr["id"] = id }
                rparts.append(["functionResponse": fr])
            }
            history.append(["role": "user", "parts": rparts])
            return request()
        }
        if text.isEmpty {
            append(name + ": 이~")
            host?.agentSay("이~", 2)
        }
        updateUi()
    }

    // ---------- 도구 ----------
    private func ok(_ s: String) -> [String: Any] { ["result": s] }
    private func err(_ s: String) -> [String: Any] { ["error": s] }

    private func openApp(_ bundle: String) -> Bool {
        guard let url = NSWorkspace.shared.urlForApplication(withBundleIdentifier: bundle) else { return false }
        NSWorkspace.shared.openApplication(at: url, configuration: NSWorkspace.OpenConfiguration())
        return true
    }

    private func fmt(_ sec: Double) -> String {
        let s = Int(sec.rounded())
        if s >= 3600 { return "\(s / 3600)시간 \(s % 3600 / 60)분" }
        if s >= 60 { return "\(s / 60)분 \(s % 60)초" }
        return "\(s)초"
    }

    private func battery() -> String {
        guard let info = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
              let list = IOPSCopyPowerSourcesList(info)?.takeRetainedValue() as? [CFTypeRef] else { return "알 수 없음" }
        for ps in list {
            if let d = IOPSGetPowerSourceDescription(info, ps)?.takeUnretainedValue() as? [String: Any],
               let cap = d[kIOPSCurrentCapacityKey] as? Int {
                let charging = (d[kIOPSPowerSourceStateKey] as? String) == kIOPSACPowerValue
                return "\(cap)%" + (charging ? " (전원 연결)" : "")
            }
        }
        return "없음(데스크톱)"
    }

    private func runTool(_ name: String, _ a: [String: Any]) -> [String: Any] {
        let s = { (k: String) in a[k] as? String ?? "" }
        switch name {
        case "open_url":
            let u = s("url")
            guard u.hasPrefix("http://") || u.hasPrefix("https://"), let url = URL(string: u) else { return err("http/https 주소만 열 수 있음") }
            return NSWorkspace.shared.open(url) ? ok("열었음: " + u) : err("열지 못함")
        case "search_web":
            var c = URLComponents(string: "https://www.google.com/search")!
            c.queryItems = [URLQueryItem(name: "q", value: s("query"))]
            return NSWorkspace.shared.open(c.url!) ? ok("브라우저에 검색 결과를 띄움 (내용은 모름)") : err("브라우저를 열지 못함")
        case "open_app":
            let apps = ["safari": "com.apple.Safari", "calculator": "com.apple.calculator", "textedit": "com.apple.TextEdit",
                        "finder": "com.apple.finder", "settings": "com.apple.systempreferences", "notes": "com.apple.Notes",
                        "music": "com.apple.Music", "spotify": "com.spotify.client", "screenshot": "com.apple.screenshot.launcher"]
            guard let b = apps[s("app")] else { return err("모르는 앱") }
            return openApp(b) ? ok("실행함") : err("그 앱이 설치돼 있지 않음")
        case "open_folder":
            let f: [String: FileManager.SearchPathDirectory] = ["desktop": .desktopDirectory, "documents": .documentDirectory,
                                                               "downloads": .downloadsDirectory, "pictures": .picturesDirectory,
                                                               "music": .musicDirectory, "movies": .moviesDirectory]
            guard let d = f[s("folder")], let url = FileManager.default.urls(for: d, in: .userDomainMask).first else { return err("모르는 폴더") }
            return NSWorkspace.shared.open(url) ? ok("열었음") : err("열지 못함")
        case "set_timer":
            let sec = (a["seconds"] as? Double) ?? 0
            guard sec >= 1 && sec <= 86400 else { return err("1초~24시간 사이만 가능") }
            let msg = s("message").isEmpty ? "시간 됐어!" : s("message")
            let id = nextTimer
            nextTimer += 1
            let t = Timer.scheduledTimer(withTimeInterval: sec, repeats: false) { [weak self] _ in self?.fire(id) }
            timers.append((id, now() + sec, msg, t))
            return ok(fmt(sec) + " 뒤에 알려줄게")
        case "list_timers":
            if timers.isEmpty { return ok("대기 중인 타이머 없음") }
            return ok(timers.map { "\($0.msg) (\(fmt(max(0, $0.due - now()))) 남음)" }.joined(separator: "\n"))
        case "get_status":
            let df = DateFormatter()
            df.locale = Locale(identifier: "ko_KR")
            df.dateFormat = "yyyy-MM-dd (E) HH:mm"
            return ok("지금: \(df.string(from: Date()))\n배터리: \(battery())\nMac 켜진 지: \(fmt(ProcessInfo.processInfo.systemUptime))\n\(name): \(host?.agentStatus() ?? "")")
        case "list_windows":
            guard axTrusted(prompt: false) else { return err("손쉬운 사용 권한이 없어서 창 목록을 못 봄 (메뉴 막대 > 우사기 > 손쉬운 사용 권한)") }
            let v = host?.agentListWindows() ?? []
            return ok(v.isEmpty ? "끌 수 있는 창 없음" : v.prefix(25).joined(separator: "\n"))
        case "drag_window":
            let r = host?.agentDragWindow(s("title_contains")) ?? "오류:우사기 없음"
            return r.hasPrefix("오류:") ? err(String(r.dropFirst(3))) : ok(r)
        case "set_music_mode":
            return ok(host?.agentSetMusicMode(s("mode")) ?? "")
        case "media_control":
            let act = s("action")
            if act.hasPrefix("volume") || act == "mute" {
                let src: String
                switch act {
                case "volume_up": src = "set volume output volume ((output volume of (get volume settings)) + 10)"
                case "volume_down": src = "set volume output volume ((output volume of (get volume settings)) - 10)"
                default: src = "set volume output muted (not (output muted of (get volume settings)))"
                }
                var e: NSDictionary?
                NSAppleScript(source: src)?.executeAndReturnError(&e)
                return e == nil ? ok("바꿨음") : err("볼륨을 바꾸지 못함")
            }
            let r = host?.agentMedia(act) ?? "오류:없음"
            return r.hasPrefix("오류:") ? err(String(r.dropFirst(3))) : ok(r)
        case "spotify_now_playing":
            return ok(host?.agentNowPlaying() ?? "")
        case "spotify_search":
            let q = s("query").addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? ""
            guard !q.isEmpty, let url = URL(string: "spotify:search:" + q) else { return err("검색어가 비어 있음") }
            return NSWorkspace.shared.open(url) ? ok("스포티파이에서 검색 결과를 열었음. 사용자가 곡을 눌러 재생해야 함")
                                                : err("스포티파이 앱을 열지 못함 (설치 안 됨?)")
        case "write_note":
            let t = s("text")
            guard !t.isEmpty else { return err("내용이 비어 있음") }
            guard let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else { return err("문서 폴더 없음") }
            let dir = docs.appendingPathComponent("우사기 메모")
            try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            let df = DateFormatter()
            df.dateFormat = "yyyyMMdd-HHmmss"
            let f = dir.appendingPathComponent(df.string(from: Date()) + ".txt")
            do { try String(t.prefix(4000)).write(to: f, atomically: true, encoding: .utf8) } catch { return err("파일을 만들지 못함") }
            NSWorkspace.shared.open(f)
            return ok("저장하고 열었음: " + f.path)
        case "copy_to_clipboard":
            NSPasteboard.general.clearContents()
            return NSPasteboard.general.setString(s("text"), forType: .string) ? ok("복사함") : err("복사하지 못함")
        case "read_clipboard":
            let t = NSPasteboard.general.string(forType: .string) ?? ""
            return ok(t.isEmpty ? "클립보드에 텍스트 없음" : String(t.prefix(6000)))
        case "pet_action":
            return ok(host?.agentPetAction(s("action")) ?? "")
        default:
            return err("없는 도구")
        }
    }

    private func fire(_ id: Int) {
        guard let i = timers.firstIndex(where: { $0.id == id }) else { return }
        let msg = timers[i].msg
        timers.remove(at: i)
        NSSound(named: "Glass")?.play()
        append(name + ": 띵! " + msg)
        host?.agentSay("띵! " + msg, 10)
    }
}
