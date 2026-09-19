#include <slugkit/metrics.hpp>

#include <userver/utest/utest.hpp>
#include <userver/utils/statistics/storage.hpp>
#include <userver/utils/statistics/testing.hpp>

namespace {

using slugkit::sdk::ClientMetrics;
using slugkit::sdk::Outcome;
using slugkit::sdk::OutcomeForStatus;
using userver::utils::statistics::Rate;
using userver::utils::statistics::Snapshot;
using userver::utils::statistics::Storage;

auto Read(const ClientMetrics& metrics) -> Snapshot {
    Storage storage;
    auto holder = storage.RegisterWriter("slugkit.client", [&metrics](auto& writer) { writer = metrics; });
    return Snapshot{storage, "slugkit.client"};
}

auto Requests(const Snapshot& snapshot, const char* operation, const char* outcome) -> Rate {
    return snapshot.SingleMetric("requests", {{"operation", operation}, {"outcome", outcome}}).AsRate();
}

}  // namespace

TEST(SlugkitMetrics, StatusesMapTheWayTheExceptionsDo) {
    EXPECT_EQ(OutcomeForStatus(200), Outcome::kOk);
    EXPECT_EQ(OutcomeForStatus(201), Outcome::kOk);
    EXPECT_EQ(OutcomeForStatus(401), Outcome::kUnauthorized);
    EXPECT_EQ(OutcomeForStatus(403), Outcome::kForbidden);
    EXPECT_EQ(OutcomeForStatus(404), Outcome::kNotFound);
    EXPECT_EQ(OutcomeForStatus(429), Outcome::kRateLimited);
    EXPECT_EQ(OutcomeForStatus(422), Outcome::kClientError);
    EXPECT_EQ(OutcomeForStatus(503), Outcome::kServerError);
}

UTEST(SlugkitMetrics, CountsRoundTripsNotCalls) {
    // A mint that succeeded on its third attempt is two failures and a
    // success; counting it as one success hides a degrading dependency.
    ClientMetrics metrics;
    metrics.Account("mint", Outcome::kServerError, std::chrono::milliseconds{40});
    metrics.AccountRetry("mint");
    metrics.Account("mint", Outcome::kTransportError, std::chrono::milliseconds{10000});
    metrics.AccountRetry("mint");
    metrics.Account("mint", Outcome::kOk, std::chrono::milliseconds{30});

    const auto snapshot = Read(metrics);
    EXPECT_EQ(Requests(snapshot, "mint", "ok"), Rate{1});
    EXPECT_EQ(Requests(snapshot, "mint", "server_error"), Rate{1});
    EXPECT_EQ(Requests(snapshot, "mint", "transport_error"), Rate{1});
    EXPECT_EQ(snapshot.SingleMetric("retries", {{"operation", "mint"}}).AsRate(), Rate{2});

    const auto duration = snapshot.SingleMetric("duration-ms", {{"operation", "mint"}}).AsHistogram();
    EXPECT_EQ(duration.GetTotalCount(), 3u);
}

UTEST(SlugkitMetrics, ADeclaredOperationReportsZerosBeforeItsFirstCall) {
    ClientMetrics metrics;
    metrics.Declare("forge");

    const auto snapshot = Read(metrics);
    EXPECT_EQ(Requests(snapshot, "forge", "ok"), Rate{0});
    EXPECT_EQ(Requests(snapshot, "forge", "rate_limited"), Rate{0});
    EXPECT_EQ(snapshot.SingleMetric("retries", {{"operation", "forge"}}).AsRate(), Rate{0});
}

UTEST(SlugkitMetrics, OperationsDoNotShareSeries) {
    ClientMetrics metrics;
    metrics.Account("mint", Outcome::kOk, std::chrono::milliseconds{20});
    metrics.Account("slice", Outcome::kNotFound, std::chrono::milliseconds{20});

    const auto snapshot = Read(metrics);
    EXPECT_EQ(Requests(snapshot, "mint", "ok"), Rate{1});
    EXPECT_EQ(Requests(snapshot, "mint", "not_found"), Rate{0});
    EXPECT_EQ(Requests(snapshot, "slice", "not_found"), Rate{1});
}
