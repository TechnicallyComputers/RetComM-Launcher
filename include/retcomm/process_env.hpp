#pragma once

#include <string>
#include <vector>

namespace retcomm {

// AppImage AppRun sets LD_LIBRARY_PATH / PATH to the mount's usr/{lib,bin}.
// Native children (cmake, git, system curl helpers, game exes) then load the
// wrong libs — e.g. git-remote-https fails with curl_global_trace / CURL_OPENSSL_4.
//
// sanitize_env_for_external_child(): mutate the current process (use in a
// fork child, or briefly under AppImageEnvGuard).
//
// AppImageEnvGuard: save → sanitize → restore on scope exit so the RetComM
// AppImage parent keeps its bundled libs for SDL/UI.
void sanitize_env_for_external_child();

struct AppImageEnvGuard {
    AppImageEnvGuard();
    ~AppImageEnvGuard();
    AppImageEnvGuard(const AppImageEnvGuard&) = delete;
    AppImageEnvGuard& operator=(const AppImageEnvGuard&) = delete;

private:
    struct SavedEnv {
        const char* key = nullptr;
        bool had = false;
        std::string value;
    };
    bool active_ = false;
    std::vector<SavedEnv> saved_;
};

} // namespace retcomm
