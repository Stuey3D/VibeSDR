// wf_combine.cpp — port of src/assets/wfCombine.ts.
#include "wf_combine.h"
#include "wf_row_place.h"   // jsRound
#include <algorithm>
#include <cmath>
#include <cstring>

namespace wf {

static const double PEAK_FRAC = std::pow(10.0, -COMBINE_PEAK_DB / 10);

static void ensure(RowCombiner& c, int len) {
  if ((int)c.sum.size() < len) { c.sum.assign(len, 0); c.peak.assign(len, 0); }
  if ((int)c.one.size() < len) c.one.assign(len, 0);
}
static void setLut(RowCombiner& c, double k) {
  if (c.lutK == k) return;
  for (int b = 0; b < 256; b++) c.lut[b] = (float)std::pow(10.0, b * k / 10);
  c.lutK = k;
}
static void fold(RowCombiner& c, const uint8_t* row, int len) {
  for (int i = 0; i < len; i++) {
    const float p = c.lut[row[i]];
    c.sum[i] = (float)((double)c.sum[i] + (double)p);
    if (p > c.peak[i]) c.peak[i] = p;
  }
}

bool combineAdd(RowCombiner& c, const uint8_t* row, int len, double lo, double span, double k) {
  bool kept = true;
  if (c.n > 0 && (len != c.len || lo != c.lo || span != c.span)) { c.n = 0; kept = false; }
  ensure(c, len);
  if (c.n == 0) {
    c.len = len; c.lo = lo; c.span = span; c.k = k > 0 ? k : dbPerUnit(0, 0);
    std::memcpy(c.one.data(), row, len);
    c.n = 1;
    return kept;
  }
  setLut(c, c.k);
  if (c.n == 1) {
    std::fill(c.sum.begin(), c.sum.begin() + len, 0.f); std::fill(c.peak.begin(), c.peak.begin() + len, 0.f);
    fold(c, c.one.data(), len);
  }
  fold(c, row, len);
  c.n++;
  return true;
}

int combineTake(RowCombiner& c, uint8_t* out, int outLen) {
  const int n = c.n, len = c.len;
  c.n = 0;
  if (n == 0 || outLen != len) return 0;
  if (n == 1) { std::memcpy(out, c.one.data(), len); return 1; }
  const double inv = 1.0 / n, scale = 10 / c.k;
  for (int i = 0; i < len; i++) {
    const double m = (double)c.sum[i] * inv, pk = (double)c.peak[i] * PEAK_FRAC;
    const double v = jsRound(std::log10(m > pk ? m : pk) * scale);
    out[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
  }
  return n;
}

int combineInto(RowCombiner& comb, const std::function<CombFrame*(long long)>& get, long long afterId, long long id) {
  CombFrame* tgt = get(id);
  if (!tgt) return 0;
  if (id - afterId <= 1) return 1;
  combineReset(comb);
  for (long long j = std::max(afterId + 1, id - COMBINE_MAX_FRAMES + 1); j <= id; j++) {
    CombFrame* f = get(j);
    if (f) combineAdd(comb, f->row.data(), (int)f->row.size(), f->c, f->h, f->k);
  }
  return combineTake(comb, tgt->row.data(), (int)tgt->row.size());
}

}  // namespace wf
