#pragma once

/// @file
/// The pool store, as the replenisher sees it.

#include <slugkit/types.hpp>

#include <cstdint>
#include <vector>

namespace slugkit::sdk::pool {

/// What a pool store must offer the replenisher: how many slugs it holds for a
/// series, and a way to add more.
///
/// Deliberately no `Draw`. Drawing is where the atomicity guarantee lives, and
/// that guarantee is not the same shape in every store — a SQL store draws
/// inside the *caller's* transaction, so a slug returns to the pool when the
/// work that claimed it rolls back, and there is no way to express "the
/// caller's transaction" that also fits a store without one. Each store
/// therefore offers its own draw, typed to whatever it actually needs, and this
/// interface covers only the part the replenisher is generic over.
///
/// Implementations are userver components. The replenisher finds one through
/// the component name in its `storage` config field, so swapping stores is a
/// configuration change rather than a rebuild.
class Storage {
public:
    virtual ~Storage();

    /// How many slugs are held for @p series. Also the gauge worth alerting on,
    /// long before it reaches zero.
    [[nodiscard]] virtual auto Size(const SeriesSlug& series) const -> std::int64_t = 0;

    /// Adds slugs to @p series' pool, ignoring any already present.
    ///
    /// Ignoring rather than failing is what lets a replenisher retry after an
    /// uncertain outcome: a mint that succeeded and a store that timed out
    /// leaves the same slugs to be offered again, and that retry must converge.
    ///
    /// @returns how many were actually added.
    virtual auto Refill(const SeriesSlug& series, const std::vector<Slug>& slugs) -> std::int64_t = 0;
};

}  // namespace slugkit::sdk::pool
