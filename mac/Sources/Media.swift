import AppKit

// 스포티파이 / Apple Music 에서 지금 재생 중인 곡 (AppleScript, 처음 한 번 '제어 허용' 확인 창이 뜬다)
struct NowPlaying {
    var ok = false, spotify = false, playing = false
    var app = "", title = "", artist = "", album = ""
    var position = 0.0, duration = 0.0, sampledAt = 0.0
    func positionAt(_ t: Double) -> Double { position + (playing ? t - sampledAt : 0) }
}

final class MediaWatcher {
    private(set) var current = NowPlaying()
    private(set) var permissionDenied = false
    private var scripts: [String: NSAppleScript] = [:]

    private func script(_ bundle: String) -> String {
        """
        with timeout of 2 seconds
        tell application id "\(bundle)"
        if player state is stopped then return {"stopped", "", "", "", 0, 0}
        set t to current track
        return {player state as text, name of t, artist of t, album of t, player position, duration of t}
        end tell
        end timeout
        """
    }

    private func run(_ src: String) -> NSAppleEventDescriptor? {
        let s: NSAppleScript
        if let c = scripts[src] { s = c } else {
            guard let n = NSAppleScript(source: src) else { return nil }
            scripts[src] = n
            s = n
        }
        var err: NSDictionary?
        let d = s.executeAndReturnError(&err)
        if let e = err {
            let code = (e[NSAppleScript.errorNumber] as? Int) ?? 0
            if code == -1743 { permissionDenied = true }
            return nil
        }
        return d
    }

    private func read(_ bundle: String, spotify: Bool) -> NowPlaying? {
        guard let d = run(script(bundle)), d.numberOfItems >= 6 else { return nil }
        var n = NowPlaying()
        n.ok = true
        n.spotify = spotify
        n.app = spotify ? "Spotify" : "Music"
        let st = d.atIndex(1)?.stringValue ?? ""
        n.playing = st == "playing"
        if st == "stopped" { return nil }
        n.title = d.atIndex(2)?.stringValue ?? ""
        n.artist = d.atIndex(3)?.stringValue ?? ""
        n.album = d.atIndex(4)?.stringValue ?? ""
        n.position = d.atIndex(5)?.doubleValue ?? 0
        let dur = d.atIndex(6)?.doubleValue ?? 0
        n.duration = spotify ? dur / 1000 : dur  // 스포티파이는 밀리초
        return n
    }

    func poll(spotifyOnly: Bool) {
        let running = Set(NSWorkspace.shared.runningApplications.compactMap { $0.bundleIdentifier })
        var np = NowPlaying()
        if running.contains("com.spotify.client"), let s = read("com.spotify.client", spotify: true) {
            np = s
            permissionDenied = false
        }
        if !spotifyOnly, !(np.ok && np.playing), running.contains("com.apple.Music"), let m = read("com.apple.Music", spotify: false),
           m.playing || !np.ok {
            np = m
        }
        np.sampledAt = now()
        current = np
    }

    // play, pause, playpause, next track, previous track
    func command(_ c: String) -> Bool {
        let bundle = current.ok && !current.spotify ? "com.apple.Music" : "com.spotify.client"
        guard NSWorkspace.shared.runningApplications.contains(where: { $0.bundleIdentifier == bundle }) else { return false }
        return run("tell application id \"\(bundle)\" to \(c)") != nil
    }
}

// ---------- 시간이 맞춰진 가사 (LRCLIB) ----------
struct LyricLine { let t: Double; let text: String }

final class LyricsEntry {
    enum Status { case pending, ready, none }
    var status = Status.pending
    var lines: [LyricLine] = []
}

func parseLrc(_ lrc: String) -> [LyricLine] {
    var out: [LyricLine] = []
    for raw in lrc.split(separator: "\n", omittingEmptySubsequences: false) {
        var line = Substring(raw)
        var times: [Double] = []
        while line.hasPrefix("["), let close = line.firstIndex(of: "]") {
            let tag = line[line.index(after: line.startIndex)..<close]
            let parts = tag.split(separator: ":")
            if parts.count == 2, let m = Double(parts[0]), let s = Double(parts[1]) { times.append(m * 60 + s) }
            line = line[line.index(after: close)...]
        }
        let text = line.trimmingCharacters(in: .whitespacesAndNewlines)
        for t in times { out.append(LyricLine(t: t, text: text)) }
    }
    return out.sorted { $0.t < $1.t }
}

final class LyricsFetcher {
    private var cache: [String: LyricsEntry] = [:]

    private func cleanTitle(_ t: String) -> String {
        var s = t
        for sep in [" - ", " (", " ["] {
            if let r = s.range(of: sep), r.lowerBound > s.startIndex { s = String(s[..<r.lowerBound]) }
        }
        return s
    }

    private func get(_ path: String, _ q: [String: String], _ done: @escaping (Any?) -> Void) {
        var c = URLComponents(string: "https://lrclib.net" + path)!
        c.queryItems = q.map { URLQueryItem(name: $0.key, value: $0.value) }
        var r = URLRequest(url: c.url!, timeoutInterval: 15)
        r.setValue("UsagiDesktopPet/1.1 (https://github.com/12dsdwasda/usagi-desktop-pet)", forHTTPHeaderField: "User-Agent")
        URLSession.shared.dataTask(with: r) { data, resp, _ in
            let ok = (resp as? HTTPURLResponse)?.statusCode == 200
            let j = ok ? data.flatMap { try? JSONSerialization.jsonObject(with: $0) } : nil
            DispatchQueue.main.async { done(j) }
        }.resume()
    }

    private func take(_ j: [String: Any], _ duration: Double) -> [LyricLine]? {
        guard let lrc = j["syncedLyrics"] as? String, !lrc.isEmpty else { return nil }
        let d = (j["duration"] as? Double) ?? 0
        if duration > 0 && d > 0 && abs(d - duration) > 8 { return nil }
        let l = parseLrc(lrc)
        return l.isEmpty ? nil : l
    }

    func get(title: String, artist: String, album: String, duration: Double) -> LyricsEntry {
        let key = artist + "\n" + title
        if let e = cache[key] { return e }
        let e = LyricsEntry()
        cache[key] = e
        var q = ["track_name": title, "artist_name": artist]
        if !album.isEmpty { q["album_name"] = album }
        if duration > 0 { q["duration"] = String(Int(duration.rounded())) }
        let finish: ([LyricLine]?) -> Void = { lines in
            e.lines = lines ?? []
            e.status = lines == nil ? .none : .ready
            NSLog("lyrics: %@ - %@ → %@ (%d줄)", title, artist, lines == nil ? "없음" : "찾음", e.lines.count)
        }
        get("/api/get", q) { [weak self] j in
            guard let self = self else { return }
            if let d = j as? [String: Any], let l = self.take(d, duration) { return finish(l) }
            let titles = Array(Set([title, self.cleanTitle(title)]))
            var left = titles.count
            var found: [LyricLine]?
            for t in titles {
                self.get("/api/search", ["track_name": t, "artist_name": artist]) { j in
                    if found == nil, let arr = j as? [[String: Any]] {
                        for it in arr { if let l = self.take(it, duration) { found = l; break } }
                    }
                    left -= 1
                    if left == 0 || found != nil, e.status == .pending { finish(found) }
                }
            }
        }
        return e
    }
}
