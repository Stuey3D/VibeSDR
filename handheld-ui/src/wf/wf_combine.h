// wf_combine.h — when the fixed clock thins a feed, the frames that fall into one row are COMBINED (linear-power mean,
// peaks kept within 6 dB) instead of dropped. Port of src/assets/wfCombine.ts (RowCombiner, combineAdd/Take, combineInto).
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace wf {

constexpr double WF_CLIP_UNITS = 14.97;
constexpr double COMBINE_PEAK_DB = 6;
constexpr int COMBINE_MAX_FRAMES = 24;

inline double dbPerUnit(double dbMin, double dbMax) {
  return (dbMax > dbMin ? dbMax - dbMin : 60) * (255 - WF_CLIP_UNITS) / (255.0 * 255.0);
}

struct RowCombiner {
  std::vector<float> sum, peak;
  int n = 0, len = 0; double lo = 0, span = 0, k = 0;
  float lut[256] = {}; double lutK = -1;
  std::vector<uint8_t> one;
};
inline void combineReset(RowCombiner& c) { c.n = 0; }
bool combineAdd(RowCombiner& c, const uint8_t* row, int len, double lo, double span, double k);
int combineTake(RowCombiner& c, uint8_t* out, int outLen);

/** A real frame kept for the clock: its row, the geometry it is placed by (centre, Hz per bin), its dB per unit. */
struct CombFrame { std::vector<uint8_t> row; double c = 0, h = 0, k = 0; };
/** combineInto (the app's merge in pumpClock): frames afterId+1 … id combined into frame id's row. */
int combineInto(RowCombiner& comb, const std::function<CombFrame*(long long)>& get, long long afterId, long long id);

}  // namespace wf
