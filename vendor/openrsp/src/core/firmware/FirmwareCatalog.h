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

// The device firmware images (opaque 8051 blobs, loaded unmodified). Image per
// hardware version (uploadFirmware):
//   hwVer 1                 -> fw_rsp1.bin   (6008 B)
//   hwVer 2, 4, 6, 7, 255   -> fw_common.bin (6083 B; the RSP1B image)
//   anything else (3/duo)   -> fw_rspduo.bin (6115 B)
// Images are verified by SHA-256 at load.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace sdrplay {
namespace firmware {

struct ImageInfo {
    const char *fileName;
    std::uint32_t size;
    const char *sha256; // lowercase hex
};

const ImageInfo &imageForHwVer(std::uint8_t hwVer);

// True if data has exactly the catalogued size and SHA-256.
bool verifyImage(const ImageInfo &img, const std::uint8_t *data, std::size_t len);

// All catalogued images (3).
const ImageInfo *allImages(int *count);

// Parses a sha256sum-format manifest (firmware/manifest.sha256, and the own
// firmware's own/manifest.sha256): one "<64 hex>  <file>" (or
// "<64 hex> *<file>") per line; empty lines are allowed. Hex is lowercased.
// false (and *out partially filled) on any malformed line or duplicate file.
bool parseManifest(const std::string &text, std::map<std::string, std::string> *out);

// Largest image the ROM loader may receive: code RAM ends where the
// firmware's variables start (XDATA 0x1800).
constexpr std::size_t kMaxImageSize = 0x1800;

} // namespace firmware
} // namespace sdrplay
