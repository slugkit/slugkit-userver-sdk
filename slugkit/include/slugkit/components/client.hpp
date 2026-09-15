#pragma once

#include <slugkit/dto/forge.hpp>
#include <slugkit/dto/mint.hpp>
#include <slugkit/dto/pattern_info.hpp>
#include <slugkit/dto/reset.hpp>
#include <slugkit/dto/slice.hpp>
#include <slugkit/types.hpp>

#include <userver/components/component_base.hpp>
#include <userver/utils/fast_pimpl.hpp>

#include <optional>
#include <vector>

namespace slugkit::sdk::components {

/// userver component wrapping the SlugKit HTTP API. Construct it via the
/// usual ``components_manager.components.slugkit-client`` block; inject
/// it into your handlers with ``context.FindComponent<Client>()``.
///
/// Static config:
///
/// ```yaml
/// slugkit-client:
///     base-url: https://slugkit.example.com    # required, here or in secdist
///     secdist-alias: accounts                    # key, series and base URL from secdist
///     api-key#env: SLUGKIT_API_KEY               # or just the key, from the config
///     user-agent: my-service/1.0                 # optional
///     request-timeout: 10s                       # optional, default 10s
///     retry:                                     # optional
///         max-attempts: 4                        # default 4 (set to 1 to disable)
///         initial-backoff: 250ms                 # default 250ms
///         max-backoff: 4s                        # default 4s
/// ```
///
/// The key: with ``secdist-alias`` the client reads ``slugkit.<alias>`` from the
/// secdist document — ``api_key``, plus ``series`` and ``base_url`` when the
/// block carries them (``base_url`` wins over the static config; ``series`` is
/// offered through @ref Series) — see ``slugkit/secrets.hpp``. A named block
/// that is missing, or no key from either source, fails at startup.
///
/// All public methods may throw a subtype of ``slugkit::sdk::Error`` on
/// failure. Network failures land as ``TransportError``; HTTP 4xx/5xx
/// land as the matching status-derived subtype (``Unauthorized``,
/// ``Forbidden``, ``NotFound``, ``RateLimited``, ``ClientError``,
/// ``ServerError``). The error message carries the server-side reason
/// when available.
///
/// Retries: transient failures — ``TransportError``, ``ServerError``
/// (HTTP 5xx), and ``RateLimited`` (HTTP 429) — are retried with
/// exponential backoff up to ``retry.max-attempts``. Non-retriable
/// errors (other 4xx, malformed JSON) propagate immediately.
class Client : public userver::components::ComponentBase {
public:
    using BaseType = userver::components::ComponentBase;
    constexpr static auto kName = "slugkit-client";

    Client(
        const userver::components::ComponentConfig& config,
        const userver::components::ComponentContext& context
    );

    ~Client() override;

    static auto GetStaticConfigSchema() -> userver::yaml_config::Schema;

    /// The series the client's secdist block names, if any.
    ///
    /// Where a consumer mints from exactly one series, that series belongs with
    /// the key: both are the account's, and keeping them together means pointing
    /// the consumer at another account is one change to one block. Callers that
    /// mint from several series name them in their own config instead.
    [[nodiscard]] auto Series() const -> const std::optional<SeriesSlug>&;

    /// Mint ``count`` fresh slugs from a series, advancing its sequence.
    [[nodiscard]] auto Mint(const dto::MintRequest& request) const -> std::vector<Slug>;

    /// Convenience: ``Mint`` with ``count = 1`` and a single-result return.
    /// Throws if the server returned an empty list.
    [[nodiscard]] auto MintOne(const dto::MintRequest& request) const -> Slug;

    /// Generate slugs from a raw pattern. Stateless — no series sequence
    /// is touched.
    [[nodiscard]] auto Forge(const dto::ForgeRequest& request) const -> std::vector<Slug>;

    /// Convenience: ``Forge`` with a single-result return.
    [[nodiscard]] auto ForgeOne(const dto::ForgeRequest& request) const -> Slug;

    /// Read deterministic slugs from a series at the given sequence offset
    /// without advancing it. Useful for previews / replays.
    [[nodiscard]] auto Slice(const dto::SliceRequest& request) const -> std::vector<Slug>;

    /// Convenience: ``Slice`` with a single-result return.
    [[nodiscard]] auto SliceOne(const dto::SliceRequest& request) const -> Slug;

    /// Rewind a series back to sequence 0. Destructive on the server.
    auto Reset(const dto::ResetRequest& request) const -> void;

    /// Ask the server to analyse a pattern and return its capacity /
    /// complexity / vocabulary footprint without generating any slugs.
    [[nodiscard]] auto GetPatternInfo(const dto::PatternInfoRequest& request) const -> dto::PatternInfoResponse;

private:
    constexpr static auto kImplSize = 336UL;
    constexpr static auto kImplAlign = 8UL;
    struct Impl;
    userver::utils::FastPimpl<Impl, kImplSize, kImplAlign> impl_;
};

}  // namespace slugkit::sdk::components
