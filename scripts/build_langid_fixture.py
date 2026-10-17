#!/usr/bin/env python3
"""build_langid_fixture.py — the TRANSCRIBE AUTO regression fixture (test/fixtures/langid/h2-10s.json.gz) from the
language-ID bake-off's bench output (docs/v12/LANGUAGE-ID-BAKEOFF.md, 2026-10-17).

Keeps only what LanguageID.swift scores on — every 10 s window of every labelled clip (skip clips left out), one row per
language: words, mean word confidence, words/s, NaturalLanguage P(own language), NaturalLanguage's top language. No
transcript text. scripts/test_languageID_swift.sh replays it through the Swift scorer + lock tracker and must get the
bake-off's numbers.

  python3 scripts/build_langid_fixture.py ~/VibeSDR-test-audio/lang/tools/labels.json full.jsonl full2.jsonl \
      noise.jsonl noise2.jsonl noise3.jsonl      # (gunzip lang/tools/results first; NOT both.jsonl — it repeats en-GB)
"""
import sys, json, gzip
labels = json.load(open(sys.argv[1]))
files, rows = [], []
for p in sys.argv[2:]:
    for line in open(p):
        r = json.loads(line)
        if 'error' in r or r['file'] not in labels or labels[r['file']]['lang'] == 'skip': continue
        if r['win'][1] != 10.0: continue
        if r['file'] not in files: files.append(r['file'])
        rows.append([files.index(r['file']), r['win'][0], r['lang'], r['words'], r['mean'], r['wps'], r['nl'], r['nlTop']])
out = {'source': 'docs/v12/LANGUAGE-ID-BAKEOFF.md (2026-10-17) — lidbench 10 s windows',
       'files': [{'name': f, 'lang': labels[f]['lang']} for f in files],
       'cols': ['file', 'start', 'lang', 'words', 'mean', 'wps', 'nl', 'nlTop'], 'rows': rows}
with gzip.open('test/fixtures/langid/h2-10s.json.gz', 'wt') as g:
    json.dump(out, g, separators=(',', ':'))
print(f'{len(files)} clips, {len(rows)} rows')
