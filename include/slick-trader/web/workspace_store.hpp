#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace slick::trader {

// The dashboard's workspaces: each a named set of tool windows, where each sat and what it showed,
// kept in one JSON file as {"last":<name>,"workspaces":{<name>:{"windows":[...]}}}. "Default" is
// always there and can't be removed. `last` is the one the main window opens as it starts.
// Single-threaded: the WebServer's event loop owns it.
class WorkspaceStore {
public:
    static constexpr std::string_view DEFAULT = "Default";

    // 200, or the HTTP status a change is refused with and why.
    struct Result {
        int status{200};
        std::string error;
        bool ok() const { return status == 200; }
    };

    // Reads `path`, where every change is written. A file missing, or unreadable (with a warning),
    // leaves Default alone. An empty path keeps the workspaces in memory only.
    void load(std::string path);

    const nlohmann::json& doc() const { return doc_; }

    // Keeps `windows` as workspace `name`, which becomes the last used.
    Result save(std::string_view name, const nlohmann::json& windows);
    Result remove(std::string_view name);
    // Marks `name` the last used.
    Result use(std::string_view name);

private:
    // Writes doc_ to path_ by a temp file renamed over it. On failure doc_ goes back to `before`.
    Result commit(nlohmann::json before);

    std::string path_;
    nlohmann::json doc_;
};

} // namespace slick::trader
