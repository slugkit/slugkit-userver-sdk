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
