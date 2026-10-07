#pragma once

#include <filesystem>

namespace fmm_loose_file_bridge {

// RE9/CrossOver compatibility for older Fluffy Mod Manager builds.
// Returns true only when the requested loose file has been materialized
// into the game's natives tree from an active Fluffy mod.
bool ensure_file(const wchar_t* path);

}
