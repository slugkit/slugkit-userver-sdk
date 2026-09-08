#include <slugkit/pool/memory_storage.hpp>

#include <userver/utest/utest.hpp>

namespace {

using slugkit::sdk::SeriesSlug;
using slugkit::sdk::Slug;
using slugkit::sdk::pool::MemoryPool;

const SeriesSlug kUsers{"users"};
const SeriesSlug kChannels{"channels"};

UTEST(MemoryPool, StartsEmpty) {
    MemoryPool pool;
    EXPECT_EQ(pool.Size(kUsers), 0);
    EXPECT_FALSE(pool.Draw(kUsers).has_value());
}

UTEST(MemoryPool, RefillThenDraw) {
    MemoryPool pool;
    EXPECT_EQ(pool.Refill(kUsers, {Slug{"one"}, Slug{"two"}}), 2);
    EXPECT_EQ(pool.Size(kUsers), 2);

    const auto drawn = pool.Draw(kUsers);
    ASSERT_TRUE(drawn.has_value());
    EXPECT_EQ(drawn->GetUnderlying(), "one");
    EXPECT_EQ(pool.Size(kUsers), 1);
}

UTEST(MemoryPool, DrawsOldestFirst) {
    // Otherwise a pool grows a stagnant tail that only gets issued long after
    // it was minted.
    MemoryPool pool;
    pool.Refill(kUsers, {Slug{"first"}, Slug{"second"}});
    pool.Refill(kUsers, {Slug{"third"}});

    EXPECT_EQ(pool.Draw(kUsers)->GetUnderlying(), "first");
    EXPECT_EQ(pool.Draw(kUsers)->GetUnderlying(), "second");
    EXPECT_EQ(pool.Draw(kUsers)->GetUnderlying(), "third");
}

UTEST(MemoryPool, RefillIgnoresSlugsAlreadyHeld) {
    // A replenisher whose mint succeeded but whose store call timed out retries
    // with the same slugs; that retry has to converge rather than double up.
    MemoryPool pool;
    EXPECT_EQ(pool.Refill(kUsers, {Slug{"one"}, Slug{"two"}}), 2);
    EXPECT_EQ(pool.Refill(kUsers, {Slug{"two"}, Slug{"three"}}), 1);
    EXPECT_EQ(pool.Size(kUsers), 3);
}

UTEST(MemoryPool, RefillOfNothingIsNotAnError) {
    MemoryPool pool;
    EXPECT_EQ(pool.Refill(kUsers, {}), 0);
    EXPECT_EQ(pool.Size(kUsers), 0);
}

UTEST(MemoryPool, ADrawnSlugCanBeRefilledAgain) {
    // The duplicate guard tracks what is *held*, not what was ever seen —
    // otherwise the pool would slowly refuse the whole series.
    MemoryPool pool;
    pool.Refill(kUsers, {Slug{"one"}});
    EXPECT_EQ(pool.Draw(kUsers)->GetUnderlying(), "one");
    EXPECT_EQ(pool.Refill(kUsers, {Slug{"one"}}), 1);
    EXPECT_EQ(pool.Size(kUsers), 1);
}

UTEST(MemoryPool, SeriesAreIndependent) {
    MemoryPool pool;
    pool.Refill(kUsers, {Slug{"shared-name"}});
    pool.Refill(kChannels, {Slug{"shared-name"}});

    EXPECT_EQ(pool.Size(kUsers), 1);
    EXPECT_EQ(pool.Size(kChannels), 1);

    EXPECT_TRUE(pool.Draw(kUsers).has_value());
    EXPECT_EQ(pool.Size(kUsers), 0);
    EXPECT_EQ(pool.Size(kChannels), 1);
}

UTEST(MemoryPool, DrainingIsReportedRatherThanImprovised) {
    MemoryPool pool;
    pool.Refill(kUsers, {Slug{"only"}});
    EXPECT_TRUE(pool.Draw(kUsers).has_value());
    EXPECT_FALSE(pool.Draw(kUsers).has_value());
}

}  // namespace
