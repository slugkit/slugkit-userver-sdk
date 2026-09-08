# slugkit-userver-sdk

A small [userver](https://userver.tech) component that wraps the SlugKit
HTTP API. Drop it into a service, point it at your SlugKit server, and
mint slugs from any handler — no rolling-your-own HTTP client.

## What it covers

| Method | Endpoint | What it does |
|---|---|---|
| `Mint` / `MintOne` | `POST /api/v1/gen/mint` | Draw fresh slugs from a series, advancing its sequence. |
| `Forge` / `ForgeOne` | `POST /api/v1/gen/forge` | Generate slugs from a raw pattern; no series state touched. |
| `Slice` / `SliceOne` | `POST /api/v1/gen/slice` | Read slugs at a given series offset without advancing it. |
| `Reset` | `POST /api/v1/gen/reset` | Rewind a series back to sequence 0. |
| `GetPatternInfo` | `POST /api/v1/gen/pattern-info` | Analyse a pattern (capacity, complexity, vocabulary footprint). |

Auth is via the `X-API-Key` header. Source the key from the environment
in your service's static config so it never lands in version control.

## Slug pool — surviving a SlugKit outage

Minting on a request path makes SlugKit a hard dependency of that request. If
the slug is a permanent public identifier — an account handle, a tenant slug —
that dependency sits on registration, and one outage means nobody can sign up.

The pool breaks that. A background task mints ahead of demand into a store you
own; the request path only ever reads locally. An outage drains the pool instead
of stopping registration, and an empty pool fails closed rather than improvising
an identifier that can never be rewritten.

```yaml
components_manager:
    components:
        slugkit-client:
            base-url: https://slugkit.example.com
            api-key#env: SLUGKIT_API_KEY

        slugkit-pool-postgres:
            postgres: postgres-main
            table: myschema.slug_pool

        slugkit-pool:
            storage: slugkit-pool-postgres
            series:
                - series: account-handles
                  pattern: '^[a-z0-9-]{3,63}$'
                - series: tenant-slugs
                  low-water: 50
                  target: 200
                  max-per-request: 150
```

> **Series names must come from `config_vars`, not `#env`.** userver enables
> environment lookups only for a component's *top-level* config keys. Inside the
> `series` list it refuses outright — `YamlConfig was not constructed with
> Mode::kEnvAllowed` — and moving the `#env` into `config_vars` does not help,
> because that file is parsed in secure mode. A `$var` reference does resolve at
> any depth, so route the series through a config var:
>
> ```yaml
> # config_vars.yaml
> account-series: my-account-handles
>
> # static_config.yaml
> slugkit-pool:
>     series:
>         - series: $account-series
> ```
>
> Beware that a config var rendered from a template (`envsubst` and similar)
> yields an empty string for an unset variable rather than an error, and the
> replenisher then refuses to start on an empty series name. Check the variable
> is set where you render it, so the failure names the variable.

Watermarks are per series because demand is: two series in one service can
easily differ by an order of magnitude in how fast they drain, and one global
figure would either hold a wasteful reserve of the slow one or an inadequate
reserve of the fast one.

The defaults — `low-water: 500`, `target: 1000`, `max-per-request: 500` — are
chosen together. The band between the waterline and target is 500 wide and the
per-request cap matches it, so an ordinary refill is a single mint rather than
several ticks. Raising `target` without raising the cap quietly gives that up.

### Drawing

```cpp
auto transaction = cluster->Begin(pg::ClusterHostType::kMaster, pg::Transaction::RW);

const auto slug = pool_.Draw(kAccountSeries, transaction);
if (!slug.has_value()) {
    transaction.Rollback();
    return Refuse();  // fail closed; see below
}

InsertAccount(transaction, *slug, ...);
transaction.Commit();
```

The draw takes **your** transaction rather than opening its own, and that is the
point: it commits with the row that carries the slug, so work that fails after
drawing returns the slug to the pool instead of burning it. A slug consumed but
never used is a leak nothing notices until the pool runs dry for no visible
reason.

Drawing deletes the row. The claim is recorded by whatever now carries the slug;
a second table saying the same thing could only ever disagree with the first.

An empty pool returns `nullopt` and the SDK does not decide what that means. If
the slug is a permanent public handle, fail closed — a fallback string lands a
value in a column you have promised never to rewrite. If it is a cosmetic label,
a local fallback is likely the better trade. Only the caller knows which it is.

### The table

The SDK issues no DDL: your schema is yours, and a library that quietly runs
DDL against it is one you cannot reason about during a deploy. Create it with
your own migration tooling.

```sql
create table myschema.slug_pool (
    series    text        not null,
    slug      text        not null,
    minted_at timestamptz not null default now(),
    primary key (series, slug)
);

-- Drawn oldest-first, so a pool cannot grow a stagnant tail that only gets
-- issued long after it was minted.
create index slug_pool_draw_idx on myschema.slug_pool (series, minted_at, slug);
```

### Bringing your own queries

`table` is a convenience that writes the three queries for you, and it works
only if the table is shaped exactly as above. When it is not — different column
names, a pool that is one partition of a larger table, an extra discriminator to
filter on, a quoted or mixed-case identifier — override the queries individually
and keep the schema you already have:

```yaml
slugkit-pool-postgres:
    postgres: postgres-main
    queries:
        size: select count(*) from "SlugReserve" where kind = $1 and claimed_at is null
        refill: >
            insert into "SlugReserve" (kind, value)
            select $1, unnest($2::text[]) on conflict do nothing
        draw: >
            update "SlugReserve" set claimed_at = now()
             where id = (select id from "SlugReserve"
                          where kind = $1 and claimed_at is null
                          order by created_at for update skip locked limit 1)
            returning value
```

Each is optional and falls back to the one generated from `table`; override all
three and `table` is never looked at. What the SDK requires is the contract, not
the shape:

| Query | Parameters | Must return | Must guarantee |
|---|---|---|---|
| `size` | `$1` series `text` | one row, one integer column | counts only slugs still available to draw |
| `refill` | `$1` series `text`, `$2` slugs `text[]` | nothing; the affected-row count is taken as the number added | ignores slugs already held, so a retry after an uncertain outcome converges instead of failing |
| `draw` | `$1` series `text` | zero or one row whose first column is the slug | claims exactly the row it returns, without blocking on a row another draw is already claiming |

Note that `draw` need not *delete*, as the generated one does — the example
above claims by timestamp instead. It runs inside the caller's transaction
either way, so the claim is undone if the caller rolls back, which is what stops
a failed provisioning from burning a slug. `skip locked` on the row selection is
what keeps concurrent draws from queueing behind one another.

### Storage is swappable

`slugkit-pool` finds its store by component name, so swapping one is a config
change. Two ship with the SDK:

| Component | Notes |
|---|---|
| `slugkit-pool-postgres` | Durable and shared between replicas. Draws inside your transaction. Needs `userver::postgresql`, so it is a separate CMake target (`SLUGKIT_BUILD_POOL_POSTGRES=ON`). |
| `slugkit-pool-memory` | Neither durable nor shared — a restart empties it and two replicas hold two independent pools. Fine for a single instance or a test. |

A store of your own implements `slugkit::sdk::pool::Storage`, which is only
`Size` and `Refill` — the part the replenisher is generic over. Drawing is
deliberately **not** on that interface: it is where the atomicity guarantee
lives, and that guarantee is not the same shape in every store. A SQL store
draws inside the caller's transaction; a store without transactions cannot
offer that and should not pretend to. Each store exposes its own `Draw`, typed
to what it actually needs.

### Failure, and how loud it is

| What happened | Level | Why |
|---|---|---|
| Mint could not reach SlugKit | warning | The pool is the buffer. Treating a transient outage as an incident wastes it. |
| Pool found empty | error | Whatever draws from it is already failing. |
| Minted but could not be stored | error | The series has advanced, so the slugs are spent; repeated failure burns the series without filling the pool. |
| Minted slugs failed `pattern` | error | The series no longer produces what this service can use — a configuration problem, caught on the way in rather than deferred to whoever draws one. |

Nothing here throws. The replenisher runs on a timer and has nobody to propagate
to, so every failure is a logged outcome and the next tick tries again.

## Installing

Add the repository as a submodule and pull it into your CMake build:

```cmake
add_subdirectory(third-party/slugkit-userver-sdk/slugkit slugkit-client)
target_link_libraries(my-service PUBLIC slugkit_client)
```

The library is built as a `userver::core`-linked OBJECT target — link it
the same way you would `userver-apn` or any other userver component
library.

## Configuring

Add a `slugkit-client` block to your service's `static_config.yaml`:

```yaml
components_manager:
  components:
    slugkit-client:
      base-url: https://slugkit.example.com
      api-key#env: SLUGKIT_API_KEY
      user-agent: my-service/1.0
      request-timeout: 10s
```

`base-url` and `api-key` are required; `user-agent` and `request-timeout`
default to `slugkit-userver-sdk/1.0` and `10s` respectively. Use the
standard userver `#env` suffix to source the API key from an environment
variable instead of inlining it.

Then register the component in your `main.cpp`:

```cpp
#include <slugkit/components/client.hpp>

userver::components::ComponentList components = userver::components::MinimalComponentList();
components.Append<slugkit::sdk::components::Client>();
```

## Using

Inject the client into a handler the usual way:

```cpp
#include <slugkit/components/client.hpp>
#include <slugkit/dto/mint.hpp>

class MyHandler final : public userver::server::handlers::HttpHandlerBase {
public:
    MyHandler(
        const userver::components::ComponentConfig& config,
        const userver::components::ComponentContext& context
    )
        : HttpHandlerBase{config, context}
        , slugkit_{context.FindComponent<slugkit::sdk::components::Client>()} {}

    auto HandleRequestThrow(
        const userver::server::http::HttpRequest&,
        userver::server::request::RequestContext&
    ) const -> std::string override {
        slugkit::sdk::dto::MintRequest request;
        request.series = slugkit::sdk::SeriesSlug{"my-series"};
        request.count = 1;
        auto slug = slugkit_.MintOne(request);
        return std::string{slug.GetUnderlying()};
    }

private:
    const slugkit::sdk::components::Client& slugkit_;
};
```

## Errors

Every failure surfaces as a subtype of `slugkit::sdk::Error`. Catch the
base type for blanket handling, or a specific subtype when you care:

| Exception | When |
|---|---|
| `TransportError` | Request never reached the server, or response could not be read. |
| `Unauthorized` | HTTP 401 — API key missing, malformed, or revoked. |
| `Forbidden` | HTTP 403 — key valid but missing the required scope. |
| `NotFound` | HTTP 404 — series or other addressed resource doesn't exist. |
| `RateLimited` | HTTP 429 — quota or rate-limit exhausted. |
| `ClientError` | Other 4xx — request shape problem the caller can fix. |
| `ServerError` | 5xx — server-side failure; generally retryable. |

The exception's `what()` carries the server-side error reason (parsed
from the `error` or `detail` field of the JSON body when present), the
HTTP status, and which operation failed.

## Licence

Apache 2.0. See [`LICENSE`](LICENSE).
