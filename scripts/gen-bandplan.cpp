// ★★★ ONE DEFINITION OF THE BAND PLAN. The directory page draws a ruler with band names on it, and
//     a second hand-kept table is exactly how the landing page and the directory would come to
//     disagree about what a server offers. So the page's plan is GENERATED from the same header
//     the server uses — every region, not just the one the build machine sits in.
//  ★ Run via scripts/gen-bandplan.sh, which also fails if the checked-in JSON has drifted.
#include <cstdio>
#include <string>
#include "../android/app/src/main/cpp/vibe_bands.h"

static std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}

int main() {
    printf("{\n  \"generatedFrom\": \"android/app/src/main/cpp/vibe_bands.h\",\n  \"regions\": {\n");
    for (int r = 1; r <= 3; r++) {
        printf("    \"%d\": [", r);
        const auto& bands = vibebands::namedBands(r);
        for (size_t i = 0; i < bands.size(); i++) {
            // ★★ THE TYPE COMES FROM THE LABEL, and it is derived HERE rather than in the page so
            //    there is one rule rather than one per reader. vibe_bands.h does not carry a type
            //    — it does not need one to allocate spectrum — but the dial colours by type the
            //    way the app's own band plan does (BAND_HEX in src/constants/bandPlan.ts:
            //    ham red, broadcast blue, utility green, CB orange).
            const std::string lab = bands[i].label;
            const char* type = lab.find("amateur") != std::string::npos ? "ham"
                             : lab.find("CB") != std::string::npos ? "cb"
                             : (lab.find("broadcast") != std::string::npos ||
                                lab.find("Broadcast") != std::string::npos ||
                                lab.find("DAB") != std::string::npos) ? "broadcast"
                             : "utility";
            printf("%s\n      {\"id\":\"%s\",\"label\":\"%s\",\"type\":\"%s\",\"lo\":%.0f,\"hi\":%.0f}",
                   i ? "," : "", esc(bands[i].id).c_str(), esc(bands[i].label).c_str(), type,
                   bands[i].lo, bands[i].hi);
        }
        printf("\n    ]%s\n", r == 3 ? "" : ",");
    }
    printf("  }");
    // ★★★ THE REGION RULE, SAMPLED RATHER THAN RETYPED. The page has to answer "which plan does a
    //     receiver at this position use", and that answer lives in ituRegion() in the header. A
    //     hand-ported copy in JavaScript is a second definition and would drift the first time the
    //     Siberia carve-out moved — so the generator SWEEPS the real function and emits the
    //     breakpoints it actually produces. Wrong is then impossible without the header changing.
    printf(",\n  \"regionByLon\": {\n");
    for (int band = 0; band < 2; band++) {
        const double lat = band ? 60.0 : 0.0;        // above and below the Russia carve-out
        printf("    \"%s\": [", band ? "high" : "low");
        int prev = -1; bool first = true;
        for (int i = 0; i <= 3600; i++) {
            const double lon = -180.0 + i * 0.1;
            const int r = vibebands::ituRegion(lat, lon, true);
            if (r != prev) {
                printf("%s{\"from\":%.1f,\"region\":%d}", first ? "" : ",", lon, r);
                prev = r; first = false;
            }
        }
        printf("]%s\n", band ? "" : ",");
    }
    printf("  },\n  \"highLatMin\": 41\n");
    printf("}\n");
    return 0;
}
