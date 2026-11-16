#include <slick-trader/web/workspace_store.hpp>
#include <slick/logger.hpp>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace slick::trader {

using json = nlohmann::json;

namespace {

constexpr size_t MAX_NAME = 64;
constexpr size_t MAX_TOOL = 32;
constexpr size_t MAX_PARAM = 128;
constexpr size_t MAX_WINDOWS = 128;

// Blanks a name doesn't start or end with: ASCII ones, and no-break and ideographic spaces in UTF-8.
constexpr std::string_view BLANKS[] = {" ", "\t", "\r", "\n", "\f", "\v", "\xC2\xA0", "\xE3\x80\x80"};

// `name` without the blanks around it: "Morning " is Morning, as the dashboard trims it too.
std::string_view trim(std::string_view name) {
    for (bool cut = true; cut;) {
        cut = false;
        for (const auto blank : BLANKS) {
            if (name.starts_with(blank)) {
                name.remove_prefix(blank.size());
                cut = true;
            }
            if (name.ends_with(blank)) {
                name.remove_suffix(blank.size());
                cut = true;
            }
        }
    }
    return name;
}

bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > MAX_NAME) {
        return false;
    }
    for (const unsigned char c : name) {
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

// `windows` as kept: the fields a window reopens with and no others. Empty `error` when it is fine.
json clean_windows(const json& windows, std::string& error) {
    if (!windows.is_array() || windows.size() > MAX_WINDOWS) {
        error = "windows must be an array of at most " + std::to_string(MAX_WINDOWS);
        return {};
    }
    json out = json::array();
    for (const auto& w : windows) {
        const auto tool = w.is_object() ? w.find("tool") : w.end();
        if (!w.is_object() || tool == w.end() || !tool->is_string() || tool->get_ref<const std::string&>().empty() ||
            tool->get_ref<const std::string&>().size() > MAX_TOOL) {
            error = "each window needs a tool name";
            return {};
        }
        json kept = {{"tool", *tool}};
        for (const char* key : {"left", "top", "width", "height"}) {
            const auto value = w.find(key);
            if (value == w.end() || !value->is_number_integer()) {
                error = std::string("each window needs an integer ") + key;
                return {};
            }
            kept[key] = *value;
        }
        if (const auto params = w.find("params"); params != w.end()) {
            if (!params->is_object()) {
                error = "params must be an object";
                return {};
            }
            for (const auto& [key, value] : params->items()) {
                if (!value.is_string() || key.size() > MAX_PARAM || value.get_ref<const std::string&>().size() > MAX_PARAM) {
                    error = "params must be short strings";
                    return {};
                }
            }
            kept["params"] = *params;
        }
        out.push_back(std::move(kept));
    }
    return out;
}

json blank() {
    const std::string name(WorkspaceStore::DEFAULT);
    return {{"last", name}, {"workspaces", {{name, {{"windows", json::array()}}}}}};
}

bool is_last(const json& doc, const std::string& name) {
    return doc["last"].get_ref<const std::string&>() == name;
}

} // namespace

void WorkspaceStore::load(std::string path) {
    path_ = std::move(path);
    doc_ = blank();
    std::error_code ec;
    if (path_.empty() || !std::filesystem::exists(path_, ec)) {
        return;
    }
    std::ifstream in(path_);
    const json file = json::parse(in, nullptr, false);
    if (!file.is_object() || !file.contains("workspaces") || !file["workspaces"].is_object()) {
        LOG_WARN("WebServer: {} is not a workspaces file: starting with Default alone", path_);
        return;
    }
    // A hand edit that broke one workspace loses that one only. A name is kept trimmed, so of
    // "Morning" and "Morning " the first in the file stays.
    auto& kept = doc_["workspaces"];
    std::set<std::string> loaded;
    for (const auto& [raw, workspace] : file["workspaces"].items()) {
        const std::string name(trim(raw));
        std::string error = loaded.contains(name) ? "another workspace has that name" : "";
        json windows = error.empty() && valid_name(name) && workspace.is_object() && workspace.contains("windows")
                     ? clean_windows(workspace["windows"], error) : json();
        if (windows.is_null()) {
            LOG_WARN("WebServer: dropping workspace '{}' from {}: {}", raw, path_,
                     error.empty() ? "not a workspace" : error);
            continue;
        }
        loaded.insert(name);
        kept[name] = {{"windows", std::move(windows)}};
    }
    if (const auto last = file.find("last"); last != file.end() && last->is_string()) {
        const std::string name(trim(last->get_ref<const std::string&>()));
        if (kept.contains(name)) {
            doc_["last"] = name;
        }
    }
}

WorkspaceStore::Result WorkspaceStore::save(std::string_view name, const json& windows) {
    name = trim(name);
    if (!valid_name(name)) {
        return {400, "a workspace name is 1 to 64 characters, none of them control characters"};
    }
    std::string error;
    json kept = clean_windows(windows, error);
    if (!error.empty()) {
        return {400, std::move(error)};
    }
    json before = doc_;
    const std::string key(name);
    doc_["workspaces"][key] = {{"windows", std::move(kept)}};
    doc_["last"] = key;
    return commit(std::move(before));
}

WorkspaceStore::Result WorkspaceStore::remove(std::string_view name) {
    name = trim(name);
    if (name == DEFAULT) {
        return {400, "Default can't be deleted"};
    }
    const std::string key(name);
    if (!doc_["workspaces"].contains(key)) {
        return {404, "no workspace " + key};
    }
    json before = doc_;
    doc_["workspaces"].erase(key);
    if (is_last(doc_, key)) {
        doc_["last"] = std::string(DEFAULT);
    }
    return commit(std::move(before));
}

WorkspaceStore::Result WorkspaceStore::use(std::string_view name) {
    const std::string key(trim(name));
    if (!doc_["workspaces"].contains(key)) {
        return {404, "no workspace " + key};
    }
    if (is_last(doc_, key)) {
        return {};
    }
    json before = doc_;
    doc_["last"] = key;
    return commit(std::move(before));
}

WorkspaceStore::Result WorkspaceStore::commit(json before) {
    if (path_.empty()) {
        return {};
    }
    namespace fs = std::filesystem;
    const fs::path file(path_);
    fs::path temp = file;
    temp += ".tmp";
    std::error_code ec;
    if (file.has_parent_path()) {
        fs::create_directories(file.parent_path(), ec);   // the write below reports a failure
    }
    bool written = false;
    {
        std::ofstream out(temp, std::ios::trunc);
        out << doc_.dump(2) << '\n';
        out.close();
        written = static_cast<bool>(out);
    }
    if (written) {
        fs::rename(temp, file, ec);   // over the old one in one step: a crash leaves either whole
    }
    if (!written || ec) {
        fs::remove(temp, ec);
        doc_ = std::move(before);
        LOG_ERROR("WebServer: cannot write workspaces to {}", path_);
        return {500, "cannot write " + path_};
    }
    return {};
}

} // namespace slick::trader
