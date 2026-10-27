// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

// Reads the same mapped disc path that the guest sees, including active file
// replacements. Called on the guest thread after DVD initialization.
std::vector<uint8_t> DVDReadVrAsset(const char* dvdPath);
