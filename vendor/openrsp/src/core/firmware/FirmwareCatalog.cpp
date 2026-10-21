// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the OpenRSP authors
//
// This file is part of OpenRSP.
//
// OpenRSP is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// OpenRSP is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with OpenRSP. If
// not, see <https://www.gnu.org/licenses/>.

#include "core/firmware/FirmwareCatalog.h"

#include "core/util/Sha256.h"

#include <sdrplay_api.h>

#include <sstream>

namespace sdrplay {
namespace firmware {

namespace {
// Keep in sync with firmware/manifest.sha256 (checked by a unit test).
const ImageInfo kImages[] = {
    {"fw_rsp1.bin", 6008, "35691d3566fb411fdfce5d303bac7d07173a467beaeff5f75639657293115953"},
    {"fw_common.bin", 6083, "b8fc1ce88cdfe23d531a362d751a568838d76b9a73f41a069e2c5fda58c4aac0"},
    {"fw_rspduo.bin", 6115, "f2a9451acd81fd8f09c5a0e506335d5d1ec9ab2c4ed53dd98de5cfff2a249387"},
};
} // namespace

const ImageInfo &imageForHwVer(std::uint8_t hwVer)
{
    switch (hwVer) {
    case SDRPLAY_RSP1_ID:
        return kImages[0];
    case SDRPLAY_RSP2_ID:
    case SDRPLAY_RSPdx_ID:
    case SDRPLAY_RSP1B_ID:
    case SDRPLAY_RSPdxR2_ID:
    case SDRPLAY_RSP1A_ID:
        return kImages[1];
    default: // SDRPLAY_RSPduo_ID and unknown values
        return kImages[2];
    }
}

bool verifyImage(const ImageInfo &img, const std::uint8_t *data, std::size_t len)
{
    if (!data || len != img.size) return false;
    return util::Sha256::hexDigest(data, len) == img.sha256;
}

const ImageInfo *allImages(int *count)
{
    if (count) *count = static_cast<int>(sizeof kImages / sizeof kImages[0]);
    return kImages;
}

bool parseManifest(const std::string &text, std::map<std::string, std::string> *out)
{
    if (!out) return false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        if (line.size() < 66) return false;
        std::string hash = line.substr(0, 64);
        for (char &c : hash) {
            if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        }
        // sha256sum: hash, one space, then ' ' (text) or '*' (binary), then the name.
        if (line[64] != ' ' || (line[65] != ' ' && line[65] != '*')) return false;
        const std::string file = line.substr(66);
        if (file.empty() || !out->emplace(file, hash).second) return false;
    }
    return true;
}

} // namespace firmware
} // namespace sdrplay
