#pragma once
// 작은 JSON 값/파서/직렬화 (UTF-8 std::string). Gemini API 요청·응답용.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;            // 배열 원소, 또는 객체 값
    std::vector<std::string> keys;  // 객체 키 (a 와 같은 순서)

    static Json Str_(std::string v) { Json j; j.t = Str; j.s = std::move(v); return j; }
    static Json Num_(double v) { Json j; j.t = Num; j.n = v; return j; }
    static Json Bool_(bool v) { Json j; j.t = Bool; j.b = v; return j; }
    static Json Arr_() { Json j; j.t = Arr; return j; }
    static Json Obj_() { Json j; j.t = Obj; return j; }

    Json& Set(const std::string& k, Json v) {
        t = Obj;
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == k) { a[i] = std::move(v); return a[i]; }
        keys.push_back(k);
        a.push_back(std::move(v));
        return a.back();
    }
    Json& Push(Json v) { t = Arr; a.push_back(std::move(v)); return a.back(); }
    const Json* Get(const std::string& k) const {
        if (t != Obj) return nullptr;
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == k) return &a[i];
        return nullptr;
    }
    std::string StrOr(const std::string& k, const std::string& def = "") const {
        const Json* v = Get(k);
        return v && v->t == Str ? v->s : def;
    }
    double NumOr(const std::string& k, double def = 0) const {
        const Json* v = Get(k);
        return v && v->t == Num ? v->n : def;
    }

    std::string Dump() const {
        std::string o;
        DumpTo(o);
        return o;
    }
    void DumpTo(std::string& o) const {
        switch (t) {
            case Null: o += "null"; break;
            case Bool: o += b ? "true" : "false"; break;
            case Num: {
                char buf[40];
                if (std::abs(n) < 1e15 && n == std::floor(n)) snprintf(buf, sizeof buf, "%.0f", n);
                else snprintf(buf, sizeof buf, "%.17g", n);
                o += buf;
                break;
            }
            case Str: Quote(o, s); break;
            case Arr:
                o += '[';
                for (size_t i = 0; i < a.size(); ++i) { if (i) o += ','; a[i].DumpTo(o); }
                o += ']';
                break;
            case Obj:
                o += '{';
                for (size_t i = 0; i < a.size(); ++i) {
                    if (i) o += ',';
                    Quote(o, keys[i]);
                    o += ':';
                    a[i].DumpTo(o);
                }
                o += '}';
                break;
        }
    }
    static void Quote(std::string& o, const std::string& v) {
        o += '"';
        for (unsigned char c : v) {
            switch (c) {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                default:
                    if (c < 0x20) { char b2[8]; snprintf(b2, sizeof b2, "\\u%04x", c); o += b2; }
                    else o += char(c);
            }
        }
        o += '"';
    }

    static bool Parse(const std::string& text, Json& out) {
        size_t i = 0;
        if (!ParseValue(text, i, out, 0)) return false;
        SkipWs(text, i);
        return i == text.size();
    }

private:
    static void SkipWs(const std::string& x, size_t& i) {
        while (i < x.size() && (x[i] == ' ' || x[i] == '\n' || x[i] == '\r' || x[i] == '\t')) ++i;
    }
    static void PutUtf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += char(cp);
        else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
        else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
    }
    static bool Hex4(const std::string& x, size_t i, unsigned& v) {
        if (i + 4 > x.size()) return false;
        v = 0;
        for (size_t k = i; k < i + 4; ++k) {
            char c = x[k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    static bool ParseString(const std::string& x, size_t& i, std::string& o) {
        if (x[i] != '"') return false;
        ++i;
        while (i < x.size()) {
            char c = x[i++];
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (i >= x.size()) return false;
            char e = x[i++];
            switch (e) {
                case '"': o += '"'; break;
                case '\\': o += '\\'; break;
                case '/': o += '/'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!Hex4(x, i, cp)) return false;
                    i += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= x.size() && x[i] == '\\' && x[i + 1] == 'u') {
                        unsigned lo;
                        if (Hex4(x, i + 2, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        }
                    }
                    PutUtf8(o, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    static bool ParseValue(const std::string& x, size_t& i, Json& out, int depth) {
        if (depth > 200) return false;
        SkipWs(x, i);
        if (i >= x.size()) return false;
        char c = x[i];
        if (c == '{') {
            out = Obj_();
            ++i;
            SkipWs(x, i);
            if (i < x.size() && x[i] == '}') { ++i; return true; }
            while (true) {
                SkipWs(x, i);
                std::string k;
                if (i >= x.size() || !ParseString(x, i, k)) return false;
                SkipWs(x, i);
                if (i >= x.size() || x[i] != ':') return false;
                ++i;
                Json v;
                if (!ParseValue(x, i, v, depth + 1)) return false;
                out.keys.push_back(std::move(k));
                out.a.push_back(std::move(v));
                SkipWs(x, i);
                if (i < x.size() && x[i] == ',') { ++i; continue; }
                if (i < x.size() && x[i] == '}') { ++i; return true; }
                return false;
            }
        }
        if (c == '[') {
            out = Arr_();
            ++i;
            SkipWs(x, i);
            if (i < x.size() && x[i] == ']') { ++i; return true; }
            while (true) {
                Json v;
                if (!ParseValue(x, i, v, depth + 1)) return false;
                out.a.push_back(std::move(v));
                SkipWs(x, i);
                if (i < x.size() && x[i] == ',') { ++i; continue; }
                if (i < x.size() && x[i] == ']') { ++i; return true; }
                return false;
            }
        }
        if (c == '"') { out = Str_(""); return ParseString(x, i, out.s); }
        if (x.compare(i, 4, "true") == 0) { out = Bool_(true); i += 4; return true; }
        if (x.compare(i, 5, "false") == 0) { out = Bool_(false); i += 5; return true; }
        if (x.compare(i, 4, "null") == 0) { out = Json(); i += 4; return true; }
        size_t st = i;
        while (i < x.size() && (isdigit((unsigned char)x[i]) || x[i] == '-' || x[i] == '+' || x[i] == '.' || x[i] == 'e' || x[i] == 'E')) ++i;
        if (st == i) return false;
        out = Num_(strtod(x.substr(st, i - st).c_str(), nullptr));
        return true;
    }
};
