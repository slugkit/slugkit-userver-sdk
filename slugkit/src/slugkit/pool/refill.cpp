#include <slugkit/pool/refill.hpp>

#include <utility>

#include <userver/logging/log.hpp>

#include <slugkit/components/client.hpp>
#include <slugkit/pool/storage.hpp>

namespace slugkit::sdk::pool {

Minter::~Minter() = default;

ClientMinter::ClientMinter(const components::Client& client) : client_{client} {}

auto ClientMinter::Mint(const dto::MintRequest& request) const -> std::vector<Slug> { return client_.Mint(request); }

auto RefillSeries(const Minter& minter, Storage& storage, const SeriesSettings& settings) -> RefillOutcome {
    const auto& name = settings.series.GetUnderlying();
    RefillOutcome outcome;

    std::int64_t size = 0;
    try {
        size = storage.Size(settings.series);
    } catch (const std::exception& e) {
        LOG_WARNING() << "slugkit-pool: could not read the pool size for series '" << name << "': " << e.what();
        outcome.size_failed = true;
        return outcome;
    }
    outcome.size_before = size;

    if (size == 0) {
        // Whatever draws from this pool is already failing by the time this
        // fires, so it is an incident rather than a housekeeping note.
        LOG_ERROR() << "slugkit-pool: the pool for series '" << name << "' is empty";
    }

    const auto count = ComputeRefillCount(size, settings.low_water, settings.target, settings.max_per_request);
    if (count == 0) {
        return outcome;
    }

    std::vector<Slug> minted;
    try {
        dto::MintRequest request;
        request.org_id = settings.org_id;
        request.series = settings.series;
        request.count = count;
        minted = minter.Mint(request);
    } catch (const std::exception& e) {
        // The pool is the buffer and a transient outage is what it is for. The
        // next tick tries again; the error level above is reserved for the pool
        // having actually run out.
        LOG_WARNING() << "slugkit-pool: mint failed for series '" << name << "' (pool at " << size << "): " << e.what();
        outcome.mint_failed = true;
        return outcome;
    }

    if (settings.pattern.has_value()) {
        auto checked = CheckShape(std::move(minted), *settings.pattern);
        if (!checked.rejected.empty()) {
            // The series no longer produces what this service can use. Storing
            // them anyway would defer the failure to whoever draws one, by which
            // time the cause is several days and a deployment away.
            LOG_ERROR() << "slugkit-pool: " << checked.rejected.size() << " of "
                        << checked.rejected.size() + checked.accepted.size() << " slugs minted for series '" << name
                        << "' do not match the configured pattern '" << settings.pattern_source
                        << "' and were discarded; first offender: " << checked.rejected.front().GetUnderlying();
        }
        outcome.discarded = static_cast<std::int64_t>(checked.rejected.size());
        minted = std::move(checked.accepted);
    }

    if (minted.empty()) {
        return outcome;
    }

    try {
        outcome.added = storage.Refill(settings.series, minted);
    } catch (const std::exception& e) {
        // The slugs are minted and the series has advanced, so they are spent
        // whatever happens next. Say so loudly: repeated failure here burns the
        // series without filling the pool.
        LOG_ERROR() << "slugkit-pool: minted " << minted.size() << " slugs for series '" << name
                    << "' but could not store them: " << e.what();
        outcome.store_failed = true;
        return outcome;
    }

    LOG_INFO() << "slugkit-pool: series '" << name << "' " << size << " -> " << size + outcome.added << " ("
               << outcome.added << " added)";
    return outcome;
}

}  // namespace slugkit::sdk::pool
