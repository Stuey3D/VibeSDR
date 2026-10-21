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

// sdrplay_api_GetErrorString: the enum names verbatim, and "sdrplay_api_UnknownError" for
// any value outside the sdrplay_api_ErrT range (negative values included).
#pragma once

#include "core/api/ApiTypes.h"

namespace sdrplay {
namespace api {

const char *errorString(int err);

} // namespace api
} // namespace sdrplay
