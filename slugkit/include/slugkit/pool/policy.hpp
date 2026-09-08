#pragma once

/// @file
/// The watermark arithmetic and the shape guard, kept as free functions so they
/// can be tested without a component context, an HTTP server or a database.

#include <slugkit/types.hpp>

#include <cstdint>
#include <regex>
#include <vector>

namespace slugkit::sdk::pool {

/// How many slugs to mint for a pool currently holding @p size.
///
/// Zero until the pool has fallen below @p low_water. Topping up on every tick
/// would mint constantly in tiny batches, spending a request and a slice of
/// quota each time, which is the opposite of holding a reserve.
///
/// Once it does refill, it refills to @p target rather than back to the
/// waterline, so the next refill is a whole `target - low_water` of demand away
/// instead of immediately.
///
/// The result never exceeds @p max_per_request: a server caps a single mint, so
/// a larger deficit is filled over several ticks rather than in one oversized
/// request that would be refused whole.
[[nodiscard]] auto ComputeRefillCount(
    std::int64_t size,
    std::int64_t low_water,
    std::int64_t target,
    std::int32_t max_per_request
) -> std::int32_t;

/// The result of checking minted slugs against the shape a series is expected
/// to produce.
struct ShapeCheck {
    std::vector<Slug> accepted;
    std::vector<Slug> rejected;
};

/// Splits @p slugs by whether they match @p pattern.
///
/// The point is to catch a series whose pattern has drifted while it is still a
/// configuration mistake, rather than later as a row that cannot be used for
/// whatever the caller needed a slug for. Storing the bad ones would defer the
/// failure to whoever draws them, by which time the cause is several days and
/// one deployment away.
[[nodiscard]] auto CheckShape(std::vector<Slug> slugs, const std::regex& pattern) -> ShapeCheck;

}  // namespace slugkit::sdk::pool
