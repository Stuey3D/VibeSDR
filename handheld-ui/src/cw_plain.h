// cw_plain.h — THE PLAIN-ENGLISH READING of decoded CW (the CW box's EXPLAIN key), a line-for-line port of
// src/utils/cwVocab.ts plainEnglish / plainLine / plainWord / tokens, over the vocabulary GENERATED from that same file
// (cw_vocab.h — node handheld-ui/test/gen_cw_vocab.mjs). test/cw_plain_test.cpp holds it to plainEnglish's own output on
// ~2500 lines (run_cw_plain_test.sh). ★ Change the reading in cwVocab.ts, regenerate, re-run — never here alone.
//
// ★★ Stuart, 2026-10-29: "callsigns, names and numbers untouched, unknown/garbled text left as sent". Recognised codes
//    become their plain words; everything else is passed through exactly as decoded. On a line that reads as another
//    language, codes that are also ordinary words there stay as sent. A code repeated (CQ CQ CQ) is said once.
#pragma once
#include "cw_vocab.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cwplain {

inline const std::unordered_map<std::string, const CwVocabTerm*>& byCode() {
    static const auto m = [] {
        std::unordered_map<std::string, const CwVocabTerm*> r;
        for (const auto& t : kCwVocab) r.emplace(t.code, &t);   // ★ the generator already kept the first of each code
        return r;
    }();
    return m;
}
inline bool foreignWord(const std::string& w) {
    static const auto f = [] { std::unordered_set<std::string> s; for (auto* x : kCwForeignWords) s.insert(x); return s; }();
    static const auto e = [] { std::unordered_set<std::string> s; for (auto* x : kCwEnglishToo) s.insert(x); return s; }();
    return f.count(w) && !e.count(w);
}
inline bool readsAsOtherLanguage(const std::vector<std::string>& w) {
    int n = 0;
    for (auto& x : w) if (foreignWord(x)) n++;
    return n >= 2;
}
inline bool isProsignChar(const std::string& t) { return t == "=" || t == "+" || t == "("; }
/** JS /\s+/ — the whitespace a decoder line can hold. */
inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; }

/** Splits glued prosign characters off a word: "=OPIS" → "=", "OPIS"; "HW?+M0PKZ" → "HW?", "+", "M0PKZ". */
inline std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    auto endWord = [&] { if (!cur.empty()) { out.push_back(cur); cur.clear(); } };
    for (char ch : line) {
        if (isSpace(ch)) { endWord(); continue; }
        if (ch == '=' || ch == '+' || ch == '(') { endWord(); out.push_back(std::string(1, ch)); }
        else cur += ch;
    }
    endWord();
    return out;
}

inline bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

inline std::string plainWord(const std::string& tok, const std::string* next, bool foreign) {
    const auto& m = byCode();
    if (isProsignChar(tok)) {
        auto it = m.find(tok);
        if (it == m.end()) return tok;
        return std::string(it->second->plain) == "—" ? "—" : "(" + std::string(it->second->plain) + ")";
    }
    if (allDigits(tok) && tok != "73" && tok != "88" && tok != "72") return tok;   // ★ numbers untouched (599, 3560)
    const bool ask = tok.size() > 1 && tok.back() == '?';
    const std::string base = ask ? tok.substr(0, tok.size() - 1) : tok;
    auto it = m.find(base);
    if (it == m.end() || !*it->second->plain) return tok;
    const CwVocabTerm& t = *it->second;
    if (foreign && (t.ambiguous || t.kind == 'a' || t.kind == 't')) return tok;   // ★ only Q-codes, prosigns, 73 elsewhere
    if (base == "NR" && next && !next->empty() && (*next)[0] >= '0' && (*next)[0] <= '9') return "number";
    if (t.kind == 'p' && base[0] == '<') return "(" + std::string(t.plain) + ")";
    if (ask) return t.ask ? std::string(t.ask) : std::string(t.plain) + "?";
    return t.plain;
}

inline std::string plainLine(const std::string& line) {
    const std::vector<std::string> w = tokens(line);
    if (w.empty()) return line;
    const bool foreign = readsAsOtherLanguage(w);
    std::vector<std::string> out;
    std::string lastPlain;
    auto push = [&](const std::string& s, bool isPlain) {
        if (isPlain && s == lastPlain) return;   // ★ CQ CQ CQ → "calling anyone" once
        out.push_back(s);
        lastPlain = isPlain ? s : "";
    };
    for (size_t i = 0; i < w.size();) {
        bool matched = false;
        if (!foreign) {
            for (const auto& p : kCwPhrases) {
                const size_t n = (size_t)p.n;
                if (i + n > w.size()) continue;
                bool ok = true, q = false;
                for (size_t k = 0; k < n && ok; k++) {
                    const std::string& t = w[i + k];
                    if (t == p.words[k]) continue;
                    if (k == n - 1 && t == std::string(p.words[k]) + "?") { q = true; continue; }
                    ok = false;
                }
                if (ok) { push(std::string(p.plain) + (q ? "?" : ""), true); i += n; matched = true; break; }
            }
        }
        if (matched) continue;
        const std::string& tok = w[i];
        const std::string p = plainWord(tok, i + 1 < w.size() ? &w[i + 1] : nullptr, foreign);
        push(p, p != tok);   // ★ only a code said once — a callsign sent twice stays as sent
        i++;
    }
    std::string r;
    for (size_t k = 0; k < out.size(); k++) { if (k) r += ' '; r += out[k]; }
    return r;
}

}  // namespace cwplain

/** ★★ The plain-English reading of decoded CW, line by line (cwVocab.ts plainEnglish). */
inline std::string cwPlainEnglish(const std::string& text) {
    std::string r;
    size_t a = 0;
    for (;;) {
        const size_t b = text.find('\n', a);
        r += cwplain::plainLine(text.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        r += '\n';
        a = b + 1;
    }
    return r;
}
