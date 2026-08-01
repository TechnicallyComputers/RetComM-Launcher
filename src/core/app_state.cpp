#include "retcomm/app_state.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <system_error>

namespace retcomm {
namespace {

using nlohmann::json;

void load_string_map(const json& j, const char* key,
                     std::unordered_map<std::string, std::string>& out) {
    if (!j.contains(key) || !j.at(key).is_object()) return;
    for (auto it = j.at(key).begin(); it != j.at(key).end(); ++it) {
        if (it.value().is_string()) out[it.key()] = it.value().get<std::string>();
    }
}

} // namespace

AppState load_app_state(const fs::path& path) {
    AppState st;
    std::ifstream in(path);
    if (!in) return st;
    try {
        json j;
        in >> j;
        st.schema_version = j.value("schema_version", 1);
        load_string_map(j, "preferred_save", st.preferred_save);
        load_string_map(j, "preferred_save_card2", st.preferred_save_card2);
        load_string_map(j, "preferred_bios", st.preferred_bios);
        // Alternate nested form: titles.<id>.preferred_save / preferred_save_card2
        if (j.contains("titles") && j.at("titles").is_object()) {
            for (auto it = j.at("titles").begin(); it != j.at("titles").end(); ++it) {
                if (!it.value().is_object()) continue;
                const auto& tj = it.value();
                if (tj.contains("preferred_save") && tj.at("preferred_save").is_string()) {
                    if (!st.preferred_save.count(it.key()))
                        st.preferred_save[it.key()] = tj.at("preferred_save").get<std::string>();
                }
                if (tj.contains("preferred_save_card2") &&
                    tj.at("preferred_save_card2").is_string()) {
                    if (!st.preferred_save_card2.count(it.key()))
                        st.preferred_save_card2[it.key()] =
                            tj.at("preferred_save_card2").get<std::string>();
                }
                if (tj.contains("preferred_bios") && tj.at("preferred_bios").is_string()) {
                    if (!st.preferred_bios.count(it.key()))
                        st.preferred_bios[it.key()] = tj.at("preferred_bios").get<std::string>();
                }
            }
        }
    } catch (...) {
        return AppState{};
    }
    return st;
}

bool save_app_state(const fs::path& path, const AppState& state, std::string* error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    json j;
    j["schema_version"] = state.schema_version;
    j["preferred_save"] = json::object();
    for (const auto& [id, save] : state.preferred_save) {
        if (!id.empty() && !save.empty()) j["preferred_save"][id] = save;
    }
    j["preferred_save_card2"] = json::object();
    for (const auto& [id, save] : state.preferred_save_card2) {
        // Persist explicit blank as empty string only when key was set — we erase blanks.
        if (!id.empty() && !save.empty()) j["preferred_save_card2"][id] = save;
    }
    j["preferred_bios"] = json::object();
    for (const auto& [id, bios] : state.preferred_bios) {
        if (!id.empty() && !bios.empty()) j["preferred_bios"][id] = bios;
    }

    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) {
            if (error) *error = "cannot write " + tmp.string();
            return false;
        }
        out << j.dump(2) << '\n';
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        if (error) *error = "cannot replace " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

std::string preferred_save_for(const AppState& state, const std::string& title_id) {
    const auto it = state.preferred_save.find(title_id);
    if (it == state.preferred_save.end()) return {};
    return it->second;
}

void set_preferred_save(AppState& state, const std::string& title_id, const std::string& save_id) {
    if (title_id.empty()) return;
    if (save_id.empty())
        state.preferred_save.erase(title_id);
    else
        state.preferred_save[title_id] = save_id;
}

std::string preferred_save_card2_for(const AppState& state, const std::string& title_id) {
    const auto it = state.preferred_save_card2.find(title_id);
    if (it == state.preferred_save_card2.end()) return {};
    return it->second;
}

void set_preferred_save_card2(AppState& state, const std::string& title_id,
                              const std::string& save_id) {
    if (title_id.empty()) return;
    if (save_id.empty())
        state.preferred_save_card2.erase(title_id);
    else
        state.preferred_save_card2[title_id] = save_id;
}

std::string preferred_bios_for(const AppState& state, const std::string& title_id) {
    const auto it = state.preferred_bios.find(title_id);
    if (it == state.preferred_bios.end()) return {};
    return it->second;
}

void set_preferred_bios(AppState& state, const std::string& title_id,
                        const std::string& bios_choice) {
    if (title_id.empty()) return;
    if (bios_choice.empty())
        state.preferred_bios.erase(title_id);
    else
        state.preferred_bios[title_id] = bios_choice;
}

} // namespace retcomm
