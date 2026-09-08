#include <slugkit/pool/postgres_storage.hpp>

#include <optional>
#include <regex>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/component.hpp>
#include <userver/storages/postgres/transaction.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace slugkit::sdk::pool {

namespace pg = userver::storages::postgres;

namespace {

constexpr auto kDefaultTable = "public.slug_pool";

/// The table name is interpolated into the queries rather than bound, because a
/// relation name cannot be a parameter. It comes from static config, so it is
/// operator-supplied rather than user input, but it is still checked: a typo
/// that happened to contain a quote would otherwise turn into a confusing
/// runtime syntax error a long way from its cause.
auto ValidateTableName(const std::string& table) -> const std::string& {
    static const std::regex kIdentifier{R"(^[a-z_][a-z0-9_]*(\.[a-z_][a-z0-9_]*)?$)"};
    if (!std::regex_match(table, kIdentifier)) {
        throw std::runtime_error{
            "slugkit-pool-postgres: `table` must be a lowercase identifier, optionally schema-qualified, got '" +
            table + "'. Anything else — a quoted or mixed-case identifier, a different column layout — wants explicit "
                    "`queries` instead."};
    }
    return table;
}

struct GeneratedQueries {
    std::string size;
    std::string refill;
    std::string draw;
};

/// The three queries for a table shaped exactly as the class documentation
/// describes. The table name is interpolated rather than bound because a
/// relation name cannot be a parameter; @ref ValidateTableName is what keeps
/// that safe.
auto Generate(const std::string& table) -> GeneratedQueries {
    GeneratedQueries generated;

    generated.size = fmt::format("select count(*) from {} where series = $1", table);

    // `do nothing` rather than an error: a replenisher whose mint succeeded but
    // whose insert timed out retries with the same slugs, and that retry has to
    // converge rather than fail on its own previous success.
    generated.refill = fmt::format(
        "insert into {} (series, slug) select $1, unnest($2::text[]) on conflict (series, slug) do nothing", table
    );

    // `for update skip locked` over a single-row subselect so two concurrent
    // draws take two different slugs instead of one waiting on the other. The
    // subselect is what keeps the lock to one row rather than the whole scan.
    generated.draw = fmt::format(
        R"(delete from {0}
 where series = $1
   and slug = (
       select slug from {0}
        where series = $1
        order by minted_at, slug
        for update skip locked
        limit 1
   )
returning slug)",
        table
    );

    return generated;
}

}  // namespace

PostgresStorage::PostgresStorage(
    const userver::components::ComponentConfig& config,
    const userver::components::ComponentContext& context
)
    : ComponentBase{config, context}
    , cluster_{context.FindComponent<userver::components::Postgres>(config["postgres"].As<std::string>())
                   .GetCluster()} {
    const auto queries = config["queries"];
    const auto custom = [&queries](std::string_view key) -> std::optional<std::string> {
        auto value = queries[std::string{key}].As<std::optional<std::string>>();
        if (value.has_value() && value->empty()) {
            return std::nullopt;
        }
        return value;
    };

    auto size = custom("size");
    auto refill = custom("refill");
    auto draw = custom("draw");

    // Only touch `table` for the queries that were not supplied. A caller who
    // overrides all three has their own schema, and should not have to satisfy a
    // validator for a name nothing is going to use.
    if (!size.has_value() || !refill.has_value() || !draw.has_value()) {
        const auto table = config["table"].As<std::string>(kDefaultTable);
        const auto generated = Generate(ValidateTableName(table));
        if (!size.has_value()) {
            size = generated.size;
        }
        if (!refill.has_value()) {
            refill = generated.refill;
        }
        if (!draw.has_value()) {
            draw = generated.draw;
        }
    }

    size_query_ = std::move(*size);
    refill_query_ = std::move(*refill);
    draw_query_ = std::move(*draw);
}

PostgresStorage::~PostgresStorage() = default;

auto PostgresStorage::GetStaticConfigSchema() -> userver::yaml_config::Schema {
    return userver::yaml_config::MergeSchemas<userver::components::ComponentBase>(R"(
type: object
description: A slug pool stored in a PostgreSQL table owned by the service
additionalProperties: false
properties:
    postgres:
        type: string
        description: component name of the database holding the pool table
    table:
        type: string
        description: the pool table, optionally schema-qualified; the service owns its migration. Used to generate any query not given explicitly below.
        defaultDescription: public.slug_pool
    queries:
        type: object
        description: explicit queries, for a pool that does not match the generated table shape. Each falls back to the query generated from `table`.
        additionalProperties: false
        properties:
            size:
                type: string
                description: "$1 = series; returns one row, one integer column: how many slugs are available to draw"
                defaultDescription: generated from `table`
            refill:
                type: string
                description: "$1 = series, $2 = slugs as text[]; the affected-row count is taken as the number added, and it must ignore slugs already held"
                defaultDescription: generated from `table`
            draw:
                type: string
                description: "$1 = series; returns zero or one row whose first column is the slug, claiming exactly that row without blocking on one another draw is claiming"
                defaultDescription: generated from `table`
)");
}

auto PostgresStorage::Size(const SeriesSlug& series) const -> std::int64_t {
    return cluster_->Execute(pg::ClusterHostType::kMaster, size_query_, series.GetUnderlying())
        .AsSingleRow<std::int64_t>();
}

auto PostgresStorage::Refill(const SeriesSlug& series, const std::vector<Slug>& slugs) -> std::int64_t {
    if (slugs.empty()) {
        return 0;
    }
    std::vector<std::string> values;
    values.reserve(slugs.size());
    for (const auto& slug : slugs) {
        values.push_back(slug.GetUnderlying());
    }
    const auto result = cluster_->Execute(pg::ClusterHostType::kMaster, refill_query_, series.GetUnderlying(), values);
    return static_cast<std::int64_t>(result.RowsAffected());
}

auto PostgresStorage::Draw(const SeriesSlug& series, pg::Transaction& transaction) const -> std::optional<Slug> {
    const auto rows = transaction.Execute(draw_query_, series.GetUnderlying());
    if (rows.IsEmpty()) {
        return std::nullopt;
    }
    return Slug{rows.Front()[0].As<std::string>()};
}

}  // namespace slugkit::sdk::pool
