#pragma once

/// @file
/// Keeps slug pools ahead of demand.

#include <slugkit/pool/refill.hpp>
#include <slugkit/types.hpp>

#include <userver/components/component_base.hpp>
#include <userver/utils/periodic_task.hpp>
#include <userver/yaml_config/schema.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace slugkit::sdk::pool {

class Storage;

/// Mints ahead of demand so that request paths never wait on SlugKit.
///
/// A periodic task, not a top-up on the path that needs a slug, and that is the
/// whole design. Minting inside the request would leave SlugKit on the critical
/// path of the very operation the pool exists to protect — the outage would
/// simply move one function call down. Here the request draws a local row and
/// this task worries about where the next one comes from.
///
/// Minting is batched: one call fetches as many slugs as the pool is short. A
/// request per slug would multiply both the latency and the quota cost of a
/// refill by the size of the deficit, at exactly the moment the pool is under
/// pressure.
///
/// Failure is not fatal, and deliberately not equally loud. A refill that
/// cannot reach SlugKit logs a warning and waits for the next tick — the pool
/// is the buffer, and treating a transient outage as an incident would waste
/// it. A pool that has actually run dry is an error, because by then whatever
/// draws from it is already failing.
///
/// Static config:
///
/// ```yaml
/// slugkit-pool:
///     storage: slugkit-pool-postgres   # component name of the store
///     client: slugkit-client           # optional, defaults to slugkit-client
///     period: 5m                       # optional, default 5m
///     refill-on-start: true            # optional, default true
///     series:
///         - series: my-user-series
///           low-water: 500              # optional, default 500
///           target: 1000                # optional, default 1000
///           max-per-request: 500        # optional, default 500
///           pattern: '^[a-z0-9-]{3,63}$'  # optional shape guard
///           org-id: 0190...            # optional, multi-org keys only
/// ```
///
/// The three defaults are chosen together: the band between the waterline and
/// target is `target - low-water` = 500 wide, and the per-request cap matches
/// it, so an ordinary refill is a single mint rather than several ticks.
/// Raising `target` without raising the cap quietly gives that up. (A pool that
/// has drained far past its waterline, or has never been filled, still takes
/// more than one tick — the cap is what stops one oversized request.)
///
/// Watermarks are per series because demand is: two series in one service can
/// easily differ by an order of magnitude in how fast they drain, and a single
/// global figure would either hold a wasteful reserve of one or an inadequate
/// reserve of the other.
class Replenisher final : public userver::components::ComponentBase {
public:
    using BaseType = userver::components::ComponentBase;
    constexpr static auto kName = "slugkit-pool";

    Replenisher(
        const userver::components::ComponentConfig& config,
        const userver::components::ComponentContext& context
    );

    ~Replenisher() override;

    static auto GetStaticConfigSchema() -> userver::yaml_config::Schema;

    /// One pass over every configured series.
    ///
    /// Public so a test can drive it directly and assert what was minted, rather
    /// than waiting on a timer. Under testsuite the periodic timer is not
    /// started at all and this is registered as a testsuite task instead.
    ///
    /// @returns how many slugs were added across all series.
    auto Replenish() -> std::int64_t;

private:
    ClientMinter minter_;
    Storage& storage_;
    std::vector<SeriesSettings> series_;
    userver::utils::PeriodicTask task_;
};

}  // namespace slugkit::sdk::pool
