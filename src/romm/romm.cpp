#include "retcomm/romm.hpp"
#include "retcomm/config.hpp"

namespace retcomm {

RommClient::RommClient(RommConfig cfg) : cfg_(std::move(cfg)) {}

bool RommClient::ping() {
    if (!cfg_.enabled()) {
        last_error_ = "RomM not configured (set base_url in config.json)";
        return false;
    }
    last_error_ = "RomM HTTP client not implemented yet (would GET " + cfg_.base_url +
                  "/api/heartbeat or /api/stats)";
    return false;
}

bool RommClient::list_platforms(std::vector<RommPlatform>& out) {
    (void)out;
    if (!cfg_.enabled()) {
        last_error_ = "RomM not configured";
        return false;
    }
    last_error_ = "STUB: GET " + cfg_.base_url + "/api/platforms";
    return false;
}

bool RommClient::list_roms(int platform_id, std::vector<RommRom>& out) {
    (void)platform_id;
    (void)out;
    if (!cfg_.enabled()) {
        last_error_ = "RomM not configured";
        return false;
    }
    last_error_ = "STUB: GET " + cfg_.base_url + "/api/roms?platform_id=…";
    return false;
}

RommConfig load_romm_config(const std::filesystem::path& config_path) {
    return load_app_config(config_path).romm;
}

} // namespace retcomm
