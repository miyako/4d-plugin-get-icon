#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace icons {
enum class Kind { file, folder };
enum class OverlayPolicy { system, noShortcut, none };
struct Result {
    std::vector<unsigned char> png;
    std::string error;
    explicit operator bool() const { return !png.empty(); }
};
inline bool validSize(std::int32_t size) {
    return size > 0 && std::uint64_t(size) * std::uint64_t(size) * 4 <= UINT32_MAX;
}
Result load(const std::u16string& path, std::int32_t size, Kind kind,
            OverlayPolicy overlays = OverlayPolicy::system);
}
