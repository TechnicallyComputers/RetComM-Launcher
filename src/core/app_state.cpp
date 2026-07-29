#include "retcomm/app_state.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <system_error>

namespace retcomm {
namespace {

using nlohmann::json;

} // namespace

AppState load_app_state(const fs::path& path) {
    AppState st;
    std::ifstream in(path);
    if (!in) return st;
    try {
        json j;
        in >> j;
        st.schema_version = j.value("schema_version", 1);
        if (j.contains("preferred_save") && j.at("preferred_save").is_object()) {
            for (auto it = j.at("preferred_save").begin(); it != j.at("preferred_save").end();
                 ++it) {
                if (it.value().is_string())
                    st.preferred_save[it.key()] = it.value().get<std::string>();
            }
        }
        // Alternate nested form: titles.<id>.preferred_save
        if (j.contains("titles") && j.at("titles").is_object()) {
            for (auto it = j.at("titles").begin(); it != j.at("titles").end(); ++it) {
                if (!it.value().is_object()) continue;
                const auto& tj = it.value();
                if (tj.contains("preferred_save") && tj.at("preferred_save").is_string()) {
                    if (!st.preferred_save.count(it.key()))
                        st.preferred_save[it.key()] = tj.at("preferred_save").get<std::string>();
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

} // namespace retcomm
