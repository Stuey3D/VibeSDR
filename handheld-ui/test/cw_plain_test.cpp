// cw_plain_test.cpp — cw_plain.h (the R36S's EXPLAIN reading) against src/utils/cwVocab.ts plainEnglish's OWN output on
// the corpus gen_cw_vocab.mjs --golden writes (every guide example, every code alone / asked / repeated / between calls,
// glued prosigns, other languages, a seeded random mix). Run: test/run_cw_plain_test.sh
#include "../src/cw_plain.h"
#include <cstdio>
#include <fstream>
#include <iostream>

static std::string unesc(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) { const char c = s[++i]; r += c == 'n' ? '\n' : c == 't' ? '\t' : c; }
        else r += s[i];
    }
    return r;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: cw_plain_test golden.tsv\n"); return 2; }
    std::ifstream in(argv[1]);
    std::string line;
    int n = 0, bad = 0;
    while (std::getline(in, line)) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string src = unesc(line.substr(0, tab)), want = unesc(line.substr(tab + 1));
        const std::string got = cwPlainEnglish(src);
        n++;
        if (got != want && bad++ < 15) std::fprintf(stderr, "✗ [%s]\n   want [%s]\n   got  [%s]\n", src.c_str(), want.c_str(), got.c_str());
    }
    std::printf("cw_plain: %d / %d match plainEnglish\n", n - bad, n);
    return bad || n < 1000 ? 1 : 0;
}
