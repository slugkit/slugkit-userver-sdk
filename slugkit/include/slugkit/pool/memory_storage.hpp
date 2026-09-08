#pragma once

/// @file
/// An in-process pool store.

#include <slugkit/pool/storage.hpp>

#include <userver/components/component_base.hpp>
#include <userver/concurrent/variable.hpp>
#include <userver/yaml_config/schema.hpp>

#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace slugkit::sdk::pool {

/// The bookkeeping behind @ref MemoryStorage, free of any component wiring so
/// it can be exercised directly.
///
/// Safe to share between coroutines.
class MemoryPool final {
public:
    [[nodiscard]] auto Size(const SeriesSlug& series) const -> std::int64_t;

    /// Adds slugs, skipping any already held. @returns how many were added.
    auto Refill(const SeriesSlug& series, const std::vector<Slug>& slugs) -> std::int64_t;

    /// Takes the oldest slug held for @p series, or nullopt when empty.
    ///
    /// Oldest-first so a pool cannot grow a stagnant tail that only gets issued
    /// long after it was minted.
    [[nodiscard]] auto Draw(const SeriesSlug& series) -> std::optional<Slug>;

private:
    struct SeriesPool {
        std::deque<Slug> queue;
        /// Mirrors `queue` so a refill can reject duplicates without a linear
        /// scan. The two are only ever mutated together.
        std::unordered_set<std::string> present;
    };

    userver::concurrent::Variable<std::unordered_map<std::string, SeriesPool>> pools_;
};

/// A pool held in memory.
///
/// Two uses. It makes the replenisher exercisable without a database, and it is
/// a reasonable store for a single-instance service that would rather absorb a
/// provider outage than take on a schema for it.
///
/// It is neither durable nor shared: a restart empties it, and two replicas hold
/// two independent pools. Anything that needs a slug to survive a deploy, or
/// needs replicas to agree on who holds what, wants @ref PostgresStorage.
///
/// Static config:
///
/// ```yaml
/// slugkit-pool-memory: {}
/// ```
class MemoryStorage final : public userver::components::ComponentBase, public Storage {
public:
    using BaseType = userver::components::ComponentBase;
    constexpr static auto kName = "slugkit-pool-memory";

    MemoryStorage(
        const userver::components::ComponentConfig& config,
        const userver::components::ComponentContext& context
    );

    ~MemoryStorage() override;

    static auto GetStaticConfigSchema() -> userver::yaml_config::Schema;

    [[nodiscard]] auto Size(const SeriesSlug& series) const -> std::int64_t override;

    auto Refill(const SeriesSlug& series, const std::vector<Slug>& slugs) -> std::int64_t override;

    /// @see MemoryPool::Draw
    [[nodiscard]] auto Draw(const SeriesSlug& series) -> std::optional<Slug>;

private:
    MemoryPool pool_;
};

}  // namespace slugkit::sdk::pool
