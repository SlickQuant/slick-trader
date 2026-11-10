#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace slick::trader {

// Lets an unordered map keyed by std::string be searched with a std::string_view, such as an
// event's instrument, with no string built: declare it with StringHash and std::equal_to<>.
struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

// A set of strings searched the same way.
using StringSet = std::unordered_set<std::string, StringHash, std::equal_to<>>;

// And a map keyed by them.
template <typename V>
using StringMap = std::unordered_map<std::string, V, StringHash, std::equal_to<>>;

} // namespace slick::trader
