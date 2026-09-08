#pragma once

/// @file
/// A slug pool held in PostgreSQL.

#include <slugkit/pool/storage.hpp>

#include <userver/components/component_base.hpp>
#include <userver/storages/postgres/postgres_fwd.hpp>
#include <userver/yaml_config/schema.hpp>

#include <optional>
#include <string>

namespace slugkit::sdk::pool {

/// A pool stored in a table you own.
///
/// The SDK does not create or migrate the table — your schema is yours, and a
/// library that quietly issues DDL against it is a library you cannot reason
/// about during a deploy. Create it with your own migration tooling:
///
/// ```sql
/// create table <schema>.slug_pool (
///     series    text        not null,
///     slug      text        not null,
///     minted_at timestamptz not null default now(),
///     primary key (series, slug)
/// );
///
/// -- Drawn oldest-first, so a pool cannot grow a stagnant tail that only gets
/// -- issued long after it was minted.
/// create index slug_pool_draw_idx on <schema>.slug_pool (series, minted_at, slug);
/// ```
///
/// A row is an unclaimed slug and nothing else. Drawing deletes it: the claim is
/// recorded by whatever now carries the slug, and a second table saying the same
/// thing could only ever disagree with the first.
///
/// Static config:
///
/// ```yaml
/// slugkit-pool-postgres:
///     postgres: postgres-main          # component name of the database
///     table: myschema.slug_pool        # optional, default public.slug_pool
/// ```
///
/// ## Bringing your own queries
///
/// `table` is a convenience that writes the three queries for you, and it only
/// works if the table is shaped exactly as above. When it is not — different
/// column names, a pool that is one partition of a larger table, an extra
/// discriminator to filter on, a quoted or mixed-case identifier — override the
/// queries individually and keep whatever schema you already have:
///
/// ```yaml
/// slugkit-pool-postgres:
///     postgres: postgres-main
///     queries:
///         size: select count(*) from "SlugReserve" where kind = $1 and claimed_at is null
///         refill: >
///             insert into "SlugReserve" (kind, value)
///             select $1, unnest($2::text[]) on conflict do nothing
///         draw: >
///             update "SlugReserve" set claimed_at = now()
///              where id = (select id from "SlugReserve"
///                           where kind = $1 and claimed_at is null
///                           order by created_at for update skip locked limit 1)
///             returning value
/// ```
///
/// Each is optional and falls back to the one generated from `table`. What the
/// SDK requires of them is the contract, not the shape:
///
/// | Query | Parameters | Must return | Must guarantee |
/// |---|---|---|---|
/// | `size` | `$1` series `text` | one row, one integer column | counts only slugs still available to draw |
/// | `refill` | `$1` series `text`, `$2` slugs `text[]` | nothing; the affected-row count is taken as the number added | ignores slugs already held, so a replenisher retrying after an uncertain outcome converges instead of failing |
/// | `draw` | `$1` series `text` | zero or one row whose first column is the slug | claims exactly the row it returns, and does not block on a row another draw is already claiming |
///
/// The `draw` query runs inside the caller's transaction, so a claim it makes is
/// undone if the caller rolls back — that is what stops a failed provisioning
/// from burning a slug, and a custom query keeps it as long as the claim is an
/// ordinary write. `skip locked` on the row selection is what keeps concurrent
/// draws from queueing behind one another.
class PostgresStorage final : public userver::components::ComponentBase, public Storage {
public:
    using BaseType = userver::components::ComponentBase;
    constexpr static auto kName = "slugkit-pool-postgres";

    PostgresStorage(
        const userver::components::ComponentConfig& config,
        const userver::components::ComponentContext& context
    );

    ~PostgresStorage() override;

    static auto GetStaticConfigSchema() -> userver::yaml_config::Schema;

    [[nodiscard]] auto Size(const SeriesSlug& series) const -> std::int64_t override;

    auto Refill(const SeriesSlug& series, const std::vector<Slug>& slugs) -> std::int64_t override;

    /// Draws one slug for @p series inside @p transaction.
    ///
    /// Taking the caller's transaction rather than opening its own is the whole
    /// point. The draw commits with the row that carries the slug, so work that
    /// fails after drawing returns the slug to the pool instead of burning it —
    /// a slug consumed but never used is a leak nothing would notice until the
    /// pool ran dry for no visible reason.
    ///
    /// Concurrent draws take different slugs rather than queueing behind each
    /// other.
    ///
    /// @returns nullopt when the pool is empty; the caller decides what that
    /// means for it. There is no fallback here to invent one, because a slug
    /// good enough to store is a decision only the caller can make.
    [[nodiscard]] auto Draw(const SeriesSlug& series, userver::storages::postgres::Transaction& transaction) const
        -> std::optional<Slug>;

private:
    userver::storages::postgres::ClusterPtr cluster_;
    std::string size_query_;
    std::string refill_query_;
    std::string draw_query_;
};

}  // namespace slugkit::sdk::pool
