#pragma once

/// @file
/// One series' refill pass, separated from the component that schedules it.

#include <slugkit/dto/mint.hpp>
#include <slugkit/pool/policy.hpp>
#include <slugkit/types.hpp>

#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace slugkit::sdk::components {
class Client;
}

namespace slugkit::sdk::pool {

class Storage;

/// Minting, narrowed to the one call a pool needs.
///
/// The refill pass takes this rather than the SlugKit client so its behaviour
/// on a provider failure can actually be exercised — the interesting branches
/// here are the ones where minting throws, and there is no honest way to
/// provoke those through a real HTTP client.
class Minter {
public:
    virtual ~Minter();

    [[nodiscard]] virtual auto Mint(const dto::MintRequest& request) const -> std::vector<Slug> = 0;
};

/// Adapts the SlugKit client to @ref Minter. Holds a reference; the client must
/// outlive it, which for components it does.
class ClientMinter final : public Minter {
public:
    explicit ClientMinter(const components::Client& client);

    [[nodiscard]] auto Mint(const dto::MintRequest& request) const -> std::vector<Slug> override;

private:
    const components::Client& client_;
};

/// Everything the refill pass needs to know about one series.
struct SeriesSettings {
    SeriesSlug series;
    OptionalOrgId org_id;
    std::int64_t low_water{};
    std::int64_t target{};
    std::int32_t max_per_request{};
    /// When set, minted slugs that do not match are discarded rather than
    /// stored. @see CheckShape
    std::optional<std::regex> pattern;
    /// The pattern as written, for the log line that reports a mismatch.
    std::string pattern_source;
};

/// What one refill pass did. Everything a caller needs to report the pool's
/// health without repeating the arithmetic or parsing the log.
struct RefillOutcome {
    /// Slugs held when the pass began; unset when the store could not be read,
    /// which is the one case where the pass knows nothing at all.
    std::optional<std::int64_t> size_before;
    /// Slugs actually added, after any duplicates the store ignored.
    std::int64_t added{0};
    /// Minted, then discarded for not matching the series' configured shape.
    /// Non-zero means the series' pattern has drifted from what was expected.
    std::int64_t discarded{0};

    bool size_failed{false};
    bool mint_failed{false};
    bool store_failed{false};

    /// True when the pass completed without any step failing — including the
    /// common case of a pool that needed nothing.
    [[nodiscard]] auto Ok() const noexcept -> bool { return !size_failed && !mint_failed && !store_failed; }
};

/// Brings one series' pool back up to its target, if it has fallen below its
/// waterline.
///
/// Never throws: every failure is a reported outcome, because this runs on a
/// timer and there is nobody to propagate to. The log levels are deliberately
/// uneven — a mint that could not reach SlugKit is a warning, since the pool is
/// the buffer and treating a transient outage as an incident would waste it,
/// while a pool found empty is an error, since by then whatever draws from it is
/// already failing.
auto RefillSeries(const Minter& minter, Storage& storage, const SeriesSettings& settings) -> RefillOutcome;

}  // namespace slugkit::sdk::pool
