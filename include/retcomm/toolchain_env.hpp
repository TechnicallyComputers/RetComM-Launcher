#pragma once

#include "retcomm/paths.hpp"

#include <filesystem>
#include <string>

namespace retcomm {

namespace fs = std::filesystem;

// After a toolchain pack is present under the shared RetComM cache, mirror the
// retcomm-toolchains zip installer:
//   1. Refresh toolchains/<id>/latest → pack_root (symlink / Windows junction;
//      never a recursive copy of the pack)
//   2. Idempotently add …/latest/bin (or pack_root/bin) to the user login PATH
//      (Unix: marked block in shell rc + path.sh hook;
//       Windows: HKCU Environment Path + RETCOMM_TOOLCHAIN_DIR)
//
// Safe to call on every successful ensure (already-on-PATH is a no-op).
// Returns false only on hard failures; soft profile/PATH issues still return
// true with a note in *message when provided.
bool publish_toolchain_user_env(const Paths& paths, const std::string& pack_id,
                                const fs::path& pack_root, std::string* message = nullptr);

} // namespace retcomm
