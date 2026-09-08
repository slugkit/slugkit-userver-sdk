#include <slugkit/pool/policy.hpp>

#include <gtest/gtest.h>

namespace {

using slugkit::sdk::Slug;
using slugkit::sdk::pool::CheckShape;
using slugkit::sdk::pool::ComputeRefillCount;

TEST(ComputeRefillCount, HoldsWhileAboveTheWaterline) {
    EXPECT_EQ(ComputeRefillCount(1000, 200, 1000, 100), 0);
    EXPECT_EQ(ComputeRefillCount(201, 200, 1000, 100), 0);
}

TEST(ComputeRefillCount, DoesNotRefillExactlyAtTheWaterline) {
    // The waterline is the point it is still fine to sit at; refilling here
    // would mint on every tick for a pool that is behaving.
    EXPECT_EQ(ComputeRefillCount(200, 200, 1000, 100), 0);
}

TEST(ComputeRefillCount, RefillsToTargetNotToTheWaterline) {
    // 199 is one slug under; the deficit is measured against target, so the
    // next refill is a whole target-minus-waterline of demand away.
    EXPECT_EQ(ComputeRefillCount(199, 200, 1000, 10000), 801);
}

TEST(ComputeRefillCount, IsCappedByMaxPerRequest) {
    EXPECT_EQ(ComputeRefillCount(0, 200, 1000, 100), 100);
    EXPECT_EQ(ComputeRefillCount(950, 1000, 1000, 100), 50);
}

TEST(ComputeRefillCount, HandlesAnEmptyPool) {
    EXPECT_EQ(ComputeRefillCount(0, 200, 1000, 10000), 1000);
}

TEST(ComputeRefillCount, AsksForNothingWhenAlreadyOverTarget) {
    // Reachable when target is lowered on a redeploy while the pool is full.
    EXPECT_EQ(ComputeRefillCount(1500, 2000, 1000, 100), 0);
}

TEST(ComputeRefillCount, TheShippedDefaultsRefillInOneRequest) {
    // low-water 500, target 1000, cap 500. The band between the waterline and
    // target is exactly the cap, so an ordinary refill is one mint.
    EXPECT_EQ(ComputeRefillCount(499, 500, 1000, 500), 500);
    EXPECT_EQ(ComputeRefillCount(300, 500, 1000, 500), 500);

    // A pool that has drained much further, or was never filled, still needs a
    // second tick — the cap is what keeps any single request sane.
    EXPECT_EQ(ComputeRefillCount(0, 500, 1000, 500), 500);
}

TEST(CheckShapeTest, AcceptsEverythingMatching) {
    const std::regex pattern{"^[a-z-]+$"};
    auto result = CheckShape({Slug{"brave-ocelot"}, Slug{"calm-otter"}}, pattern);
    EXPECT_EQ(result.accepted.size(), 2u);
    EXPECT_TRUE(result.rejected.empty());
}

TEST(CheckShapeTest, SplitsOutTheOffenders) {
    const std::regex pattern{"^[a-z-]+$"};
    auto result = CheckShape({Slug{"brave-ocelot"}, Slug{"Brave_Ocelot_42"}, Slug{"calm-otter"}}, pattern);

    ASSERT_EQ(result.accepted.size(), 2u);
    EXPECT_EQ(result.accepted[0].GetUnderlying(), "brave-ocelot");
    EXPECT_EQ(result.accepted[1].GetUnderlying(), "calm-otter");

    ASSERT_EQ(result.rejected.size(), 1u);
    EXPECT_EQ(result.rejected[0].GetUnderlying(), "Brave_Ocelot_42");
}

TEST(CheckShapeTest, MatchesWholeSlugsNotSubstrings) {
    // regex_match, not regex_search: a slug that merely contains something
    // acceptable is not acceptable.
    const std::regex pattern{"^[a-z-]+$"};
    auto result = CheckShape({Slug{"brave-ocelot-42"}}, pattern);
    EXPECT_TRUE(result.accepted.empty());
    EXPECT_EQ(result.rejected.size(), 1u);
}

TEST(CheckShapeTest, HandlesNothingToCheck) {
    const std::regex pattern{"^[a-z-]+$"};
    auto result = CheckShape({}, pattern);
    EXPECT_TRUE(result.accepted.empty());
    EXPECT_TRUE(result.rejected.empty());
}

}  // namespace
