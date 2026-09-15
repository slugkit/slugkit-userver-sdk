#include <slugkit/pool/replenisher.hpp>

#include <stdexcept>
#include <utility>

#include <boost/uuid/string_generator.hpp>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/components/statistics_storage.hpp>
#include <userver/logging/log.hpp>
#include <userver/testsuite/tasks.hpp>
#include <userver/utils/flags.hpp>
#include <userver/utils/statistics/labels.hpp>
#include <userver/utils/statistics/rate.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <slugkit/components/client.hpp>
#include <slugkit/pool/storage.hpp>

namespace slugkit::sdk::pool {

namespace {

// Chosen together: the band between the waterline and target is 500 wide and
// the per-request cap is 500, so an ordinary refill is one mint rather than
// several ticks. Raising `target` without raising the cap quietly gives that up.
constexpr std::int64_t kDefaultLowWater = 500;
constexpr std::int64_t kDefaultTarget = 1000;
constexpr std::int32_t kDefaultMaxPerRequest = 500;

}  // namespace

Replenisher::Replenisher(
    const userver::components::ComponentConfig& config,
    const userver::components::ComponentContext& context
)
    : ComponentBase{config, context}
    , minter_{context.FindComponent<components::Client>(config["client"].As<std::string>(std::string{components::Client::kName}))}
    , storage_{context.FindComponent<Storage>(config["storage"].As<std::string>())} {
    const auto& client = context.FindComponent<components::Client>(
        config["client"].As<std::string>(std::string{components::Client::kName})
    );

    for (const auto& entry : config["series"]) {
        SeriesSettings series;
        // The series is the client's when its secdist block names one: a
        // consumer minting from a single series has that series and its key from
        // the same account, so both live in the same block and neither is a
        // config var. An entry may still name its own, which is what a pool
        // keeping several series does.
        const auto named = entry["series"].As<std::string>("");
        if (!named.empty()) {
            series.series = SeriesSlug{named};
        } else if (client.Series().has_value()) {
            series.series = *client.Series();
        } else {
            throw std::runtime_error{
                "slugkit-pool: an entry in `series` names no series and the client's secdist block carries none"};
        }

        if (auto org_id = entry["org-id"].As<std::optional<std::string>>(); org_id.has_value()) {
            series.org_id = OrgId{boost::uuids::string_generator{}(*org_id)};
        }

        series.low_water = entry["low-water"].As<std::int64_t>(kDefaultLowWater);
        series.target = entry["target"].As<std::int64_t>(kDefaultTarget);
        series.max_per_request = entry["max-per-request"].As<std::int32_t>(kDefaultMaxPerRequest);

        if (series.low_water > series.target) {
            // Otherwise every tick refills and is still under the waterline, so
            // the task mints forever. Catch it at startup, where it is a typo.
            throw std::runtime_error{
                "slugkit-pool: `low-water` must not exceed `target` for series '" + series.series.GetUnderlying() + "'"};
        }
        if (series.max_per_request < 1) {
            throw std::runtime_error{
                "slugkit-pool: `max-per-request` must be at least 1 for series '" + series.series.GetUnderlying() + "'"};
        }

        if (auto pattern = entry["pattern"].As<std::optional<std::string>>(); pattern.has_value()) {
            try {
                series.pattern.emplace(*pattern, std::regex::optimize);
            } catch (const std::regex_error& e) {
                throw std::runtime_error{
                    "slugkit-pool: `pattern` for series '" + series.series.GetUnderlying() +
                    "' is not a valid regex: " + e.what()};
            }
            series.pattern_source = std::move(*pattern);
        }

        // Constructed in place: SeriesState holds atomics, so it cannot be
        // built as a temporary and moved in.
        auto state = std::make_unique<SeriesState>();
        state->settings = std::move(series);
        series_.push_back(std::move(state));
    }

    if (series_.empty()) {
        throw std::runtime_error{"slugkit-pool: `series` must list at least one series to keep topped up"};
    }

    statistics_holder_ = context.FindComponent<userver::components::StatisticsStorage>().GetStorage().RegisterWriter(
        config["metrics-prefix"].As<std::string>("slug-pool"),
        [this](userver::utils::statistics::Writer& writer) { WriteStatistics(writer); }
    );

    // Named after the component, not the class: a service that keeps two pools
    // appends two replenishers under two names, and a fixed name would collide in
    // the testsuite registry, which refuses a second task of the same name. An
    // unnamed replenisher is still `slugkit-pool`.
    const auto task_name = config.Name();

    auto& testsuite_tasks = userver::testsuite::GetTestsuiteTasks(context);
    if (testsuite_tasks.IsEnabled()) {
        // Leave the timer stopped so a test fires a pass on demand and asserts
        // what the task did, rather than how long it waited.
        testsuite_tasks.RegisterTask(task_name, [this] { Replenish(); });
    } else {
        const auto period = config["period"].As<std::chrono::milliseconds>(std::chrono::minutes{5});
        userver::utils::Flags<userver::utils::PeriodicTask::Flags> flags{userver::utils::PeriodicTask::Flags::kStrong};
        if (config["refill-on-start"].As<bool>(true)) {
            // A fresh deployment starts with an empty pool, so without this the
            // first draw fails for a whole period. kNow only schedules the first
            // pass immediately — a failure there is logged like any other tick
            // and never blocks or fails service startup.
            flags |= userver::utils::PeriodicTask::Flags::kNow;
        }
        task_.Start(task_name, {period, flags}, [this] { Replenish(); });
    }
}

Replenisher::~Replenisher() {
    task_.Stop();
    statistics_holder_.Unregister();
}

auto Replenisher::GetStaticConfigSchema() -> userver::yaml_config::Schema {
    return userver::yaml_config::MergeSchemas<userver::components::ComponentBase>(R"(
type: object
description: Keeps slug pools topped up from SlugKit ahead of demand
additionalProperties: false
properties:
    storage:
        type: string
        description: component name of the pool store to fill
    client:
        type: string
        description: component name of the SlugKit client to mint through
        defaultDescription: slugkit-client
    period:
        type: string
        description: how often to check the pools
        defaultDescription: 5m
    refill-on-start:
        type: boolean
        description: run one pass at startup so a fresh deployment is not empty for a whole period
        defaultDescription: true
    metrics-prefix:
        type: string
        description: prefix for the pool metrics
        defaultDescription: slug-pool
    series:
        type: array
        description: the series to keep topped up, each with its own watermarks
        items:
            type: object
            description: one series
            additionalProperties: false
            properties:
                series:
                    type: string
                    description: the SlugKit series slug to mint from; also the pool key. Omit to take the series from the client's secdist block.
                    defaultDescription: the client's secdist series
                org-id:
                    type: string
                    description: target organisation; only meaningful for multi-org API keys
                low-water:
                    type: integer
                    description: refill once the pool falls below this many slugs
                    defaultDescription: 500
                target:
                    type: integer
                    description: refill up to this many slugs
                    defaultDescription: 1000
                max-per-request:
                    type: integer
                    description: largest single mint; a bigger deficit is filled over several ticks
                    defaultDescription: 500
                pattern:
                    type: string
                    description: optional regex every minted slug must match, to catch a drifted series pattern on the way in
)");
}

auto Replenisher::Replenish() -> std::int64_t {
    std::int64_t added = 0;
    for (const auto& state : series_) {
        const auto outcome = RefillSeries(minter_, storage_, state->settings);

        auto& stats = state->stats;
        if (outcome.size_before.has_value()) {
            // The size *after* the pass, which is what a dashboard wants: it is
            // the figure the next draw will see.
            stats.size.store(*outcome.size_before + outcome.added, std::memory_order_relaxed);
        }
        stats.added.fetch_add(static_cast<std::uint64_t>(outcome.added), std::memory_order_relaxed);
        stats.discarded.fetch_add(static_cast<std::uint64_t>(outcome.discarded), std::memory_order_relaxed);
        if (outcome.size_failed) {
            stats.size_failures.fetch_add(1, std::memory_order_relaxed);
        }
        if (outcome.mint_failed) {
            stats.mint_failures.fetch_add(1, std::memory_order_relaxed);
        }
        if (outcome.store_failed) {
            stats.store_failures.fetch_add(1, std::memory_order_relaxed);
        }

        added += outcome.added;
    }
    return added;
}

void Replenisher::WriteStatistics(userver::utils::statistics::Writer& writer) const {
    using userver::utils::statistics::LabelView;
    using userver::utils::statistics::Rate;

    for (const auto& state : series_) {
        const LabelView series{"slugkit_series", state->settings.series.GetUnderlying()};
        const auto& stats = state->stats;

        // Skipped rather than reported as zero until a pass has run: a pool that
        // has not been looked at yet is not a pool that is empty, and alerting
        // on the difference matters during a deploy.
        if (const auto size = stats.size.load(std::memory_order_relaxed); size >= 0) {
            writer["size"].ValueWithLabels(size, series);
        }

        // The watermarks are emitted so a dashboard can show headroom, and an
        // alert can be written against `size / low-water`, without anyone having
        // to hardcode figures that live in this service's config.
        writer["low-water"].ValueWithLabels(state->settings.low_water, series);
        writer["target"].ValueWithLabels(state->settings.target, series);

        writer["added"].ValueWithLabels(Rate{stats.added.load(std::memory_order_relaxed)}, series);
        writer["discarded"].ValueWithLabels(Rate{stats.discarded.load(std::memory_order_relaxed)}, series);

        writer["errors"].ValueWithLabels(
            Rate{stats.size_failures.load(std::memory_order_relaxed)}, {series, LabelView{"slugkit_stage", "size"}}
        );
        writer["errors"].ValueWithLabels(
            Rate{stats.mint_failures.load(std::memory_order_relaxed)}, {series, LabelView{"slugkit_stage", "mint"}}
        );
        writer["errors"].ValueWithLabels(
            Rate{stats.store_failures.load(std::memory_order_relaxed)}, {series, LabelView{"slugkit_stage", "store"}}
        );
    }
}

}  // namespace slugkit::sdk::pool
