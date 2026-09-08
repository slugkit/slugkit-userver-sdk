#include <slugkit/pool/policy.hpp>

#include <algorithm>
#include <utility>

namespace slugkit::sdk::pool {

auto ComputeRefillCount(
    std::int64_t size,
    std::int64_t low_water,
    std::int64_t target,
    std::int32_t max_per_request
) -> std::int32_t {
    if (size >= low_water) {
        return 0;
    }
    const auto deficit = target - size;
    if (deficit <= 0) {
        return 0;
    }
    return static_cast<std::int32_t>(std::min<std::int64_t>(deficit, max_per_request));
}

auto CheckShape(std::vector<Slug> slugs, const std::regex& pattern) -> ShapeCheck {
    ShapeCheck result;
    result.accepted.reserve(slugs.size());
    for (auto& slug : slugs) {
        if (std::regex_match(slug.GetUnderlying(), pattern)) {
            result.accepted.push_back(std::move(slug));
        } else {
            result.rejected.push_back(std::move(slug));
        }
    }
    return result;
}

}  // namespace slugkit::sdk::pool
