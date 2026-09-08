#include <slugkit/pool/refill.hpp>

#include <stdexcept>

#include <userver/utest/utest.hpp>

#include <slugkit/exceptions.hpp>
#include <slugkit/pool/memory_storage.hpp>
#include <slugkit/pool/storage.hpp>

namespace {

using slugkit::sdk::SeriesSlug;
using slugkit::sdk::Slug;
using slugkit::sdk::dto::MintRequest;
using slugkit::sdk::pool::MemoryPool;
using slugkit::sdk::pool::Minter;
using slugkit::sdk::pool::RefillSeries;
using slugkit::sdk::pool::SeriesSettings;
using slugkit::sdk::pool::Storage;

const SeriesSlug kUsers{"users"};

/// Hands out `prefix-0`, `prefix-1`, … so a test can predict what lands in the
/// pool, and records what it was asked for.
class FakeMinter final : public Minter {
public:
    explicit FakeMinter(std::string prefix = "slug") : prefix_{std::move(prefix)} {}

    auto Mint(const MintRequest& request) const -> std::vector<Slug> override {
        requests.push_back(request.count);
        if (throws) {
            throw slugkit::sdk::TransportError{"the provider is unreachable"};
        }
        std::vector<Slug> minted;
        for (std::int32_t i = 0; i < request.count; ++i) {
            minted.push_back(Slug{prefix_ + "-" + std::to_string(next_++)});
        }
        return minted;
    }

    mutable std::vector<std::int32_t> requests;
    mutable std::int32_t next_{0};
    bool throws{false};

private:
    std::string prefix_;
};

/// A store over MemoryPool that can be told to fail, so the refill pass's
/// failure branches are reachable.
class FlakyStorage final : public Storage {
public:
    auto Size(const SeriesSlug& series) const -> std::int64_t override {
        if (size_throws) {
            throw std::runtime_error{"the database is unreachable"};
        }
        return pool.Size(series);
    }

    auto Refill(const SeriesSlug& series, const std::vector<Slug>& slugs) -> std::int64_t override {
        if (refill_throws) {
            throw std::runtime_error{"the database is unreachable"};
        }
        return pool.Refill(series, slugs);
    }

    mutable MemoryPool pool;
    bool size_throws{false};
    bool refill_throws{false};
};

auto Settings(std::int64_t low_water, std::int64_t target, std::int32_t max_per_request = 100) -> SeriesSettings {
    SeriesSettings settings;
    settings.series = kUsers;
    settings.low_water = low_water;
    settings.target = target;
    settings.max_per_request = max_per_request;
    return settings;
}

UTEST(RefillSeries, FillsAnEmptyPoolUpToTarget) {
    FakeMinter minter;
    FlakyStorage storage;

    EXPECT_EQ(RefillSeries(minter, storage, Settings(5, 10)).added, 10);
    EXPECT_EQ(storage.pool.Size(kUsers), 10);
    ASSERT_EQ(minter.requests.size(), 1u);
    EXPECT_EQ(minter.requests[0], 10);
}

UTEST(RefillSeries, DoesNotMintWhileAboveTheWaterline) {
    FakeMinter minter;
    FlakyStorage storage;
    storage.pool.Refill(kUsers, {Slug{"a"}, Slug{"b"}, Slug{"c"}});

    EXPECT_EQ(RefillSeries(minter, storage, Settings(2, 10)).added, 0);
    EXPECT_TRUE(minter.requests.empty()) << "a pool above its waterline must not spend a request or any quota";
    EXPECT_EQ(storage.pool.Size(kUsers), 3);
}

UTEST(RefillSeries, AsksOnlyForTheDeficit) {
    FakeMinter minter;
    FlakyStorage storage;
    storage.pool.Refill(kUsers, {Slug{"a"}, Slug{"b"}});

    EXPECT_EQ(RefillSeries(minter, storage, Settings(5, 10)).added, 8);
    ASSERT_EQ(minter.requests.size(), 1u);
    EXPECT_EQ(minter.requests[0], 8);
}

UTEST(RefillSeries, SplitsALargeDeficitAcrossTicks) {
    FakeMinter minter;
    FlakyStorage storage;
    const auto settings = Settings(500, 1000, 100);

    EXPECT_EQ(RefillSeries(minter, storage, settings).added, 100);
    EXPECT_EQ(RefillSeries(minter, storage, settings).added, 100);
    EXPECT_EQ(storage.pool.Size(kUsers), 200);
    EXPECT_EQ(minter.requests[0], 100) << "a mint larger than the cap would be refused whole";
}

UTEST(RefillSeries, ReportsWhichStepFailed) {
    // The outcome is what a caller turns into metrics, so each failure has to be
    // distinguishable — "nothing was added" alone cannot tell a dry provider
    // from an unreachable store from a pool that simply needed nothing.
    FakeMinter minter;
    FlakyStorage storage;

    EXPECT_TRUE(RefillSeries(minter, storage, Settings(5, 10)).Ok());

    minter.throws = true;
    const auto mint_failed = RefillSeries(minter, storage, Settings(15, 20));
    EXPECT_TRUE(mint_failed.mint_failed);
    EXPECT_FALSE(mint_failed.size_failed);
    EXPECT_FALSE(mint_failed.Ok());
    EXPECT_EQ(mint_failed.size_before, 10) << "the size was read even though the mint then failed";

    minter.throws = false;
    storage.size_throws = true;
    const auto size_failed = RefillSeries(minter, storage, Settings(15, 20));
    EXPECT_TRUE(size_failed.size_failed);
    EXPECT_FALSE(size_failed.size_before.has_value()) << "a pass that cannot read the store knows nothing";

    storage.size_throws = false;
    storage.refill_throws = true;
    const auto store_failed = RefillSeries(minter, storage, Settings(15, 20));
    EXPECT_TRUE(store_failed.store_failed);
    EXPECT_EQ(store_failed.added, 0);
}

UTEST(RefillSeries, CountsWhatTheShapeGuardDiscarded) {
    FakeMinter minter{"Wrong_Shape"};
    FlakyStorage storage;

    auto settings = Settings(5, 10);
    settings.pattern.emplace("^[a-z-]+[0-9]*$");
    settings.pattern_source = "^[a-z-]+[0-9]*$";

    const auto outcome = RefillSeries(minter, storage, settings);
    EXPECT_EQ(outcome.discarded, 10) << "a drifted series pattern must be countable, not just loggable";
    EXPECT_EQ(outcome.added, 0);
    EXPECT_TRUE(outcome.Ok()) << "a shape mismatch is a series problem, not a failed step";
}

UTEST(RefillSeries, SurvivesAProviderOutage) {
    // The whole point of the pool: a mint failure is absorbed, not propagated.
    FakeMinter minter;
    minter.throws = true;
    FlakyStorage storage;
    storage.pool.Refill(kUsers, {Slug{"a"}});

    EXPECT_EQ(RefillSeries(minter, storage, Settings(5, 10)).added, 0);
    EXPECT_EQ(storage.pool.Size(kUsers), 1) << "the slugs already held must survive a failed refill";
}

UTEST(RefillSeries, SurvivesAStoreThatCannotBeRead) {
    FakeMinter minter;
    FlakyStorage storage;
    storage.size_throws = true;

    EXPECT_EQ(RefillSeries(minter, storage, Settings(5, 10)).added, 0);
    EXPECT_TRUE(minter.requests.empty()) << "minting without knowing the deficit would be guessing";
}

UTEST(RefillSeries, SurvivesAStoreThatCannotBeWritten) {
    FakeMinter minter;
    FlakyStorage storage;
    storage.refill_throws = true;

    EXPECT_EQ(RefillSeries(minter, storage, Settings(5, 10)).added, 0);
    EXPECT_EQ(minter.requests.size(), 1u) << "the slugs are spent even though they could not be stored";
}

UTEST(RefillSeries, DiscardsSlugsThatDoNotMatchTheConfiguredShape) {
    FakeMinter minter{"Wrong_Shape"};
    FlakyStorage storage;

    auto settings = Settings(5, 10);
    settings.pattern.emplace("^[a-z-]+[0-9]*$");
    settings.pattern_source = "^[a-z-]+[0-9]*$";

    EXPECT_EQ(RefillSeries(minter, storage, settings).added, 0);
    EXPECT_EQ(storage.pool.Size(kUsers), 0) << "storing them would defer the failure to whoever draws one";
}

UTEST(RefillSeries, KeepsTheSlugsThatDoMatch) {
    FakeMinter minter{"good-slug"};
    FlakyStorage storage;

    auto settings = Settings(5, 10);
    settings.pattern.emplace("^[a-z-]+[0-9]*$");
    settings.pattern_source = "^[a-z-]+[0-9]*$";

    EXPECT_EQ(RefillSeries(minter, storage, settings).added, 10);
    EXPECT_EQ(storage.pool.Size(kUsers), 10);
}

UTEST(RefillSeries, ConvergesWhenTheSameSlugsAreOfferedTwice) {
    // A mint that succeeded and a store call that timed out leaves the
    // replenisher retrying with slugs it already stored.
    FakeMinter minter;
    FlakyStorage storage;

    EXPECT_EQ(RefillSeries(minter, storage, Settings(5, 10)).added, 10);
    minter.next_ = 0;  // the same slugs come back
    EXPECT_EQ(RefillSeries(minter, storage, Settings(15, 20)).added, 0);
    EXPECT_EQ(storage.pool.Size(kUsers), 10) << "a retry must not double up";
}

}  // namespace
