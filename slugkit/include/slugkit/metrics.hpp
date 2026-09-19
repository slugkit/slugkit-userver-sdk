#pragma once

/// @file
/// What the client reports about its calls to SlugKit.
///
/// The pool's metrics (slugkit/pool/replenisher.hpp) say whether there are
/// slugs to hand out; these say how the calls that fill it are going, and they
/// answer different questions. A pool can sit comfortably above its low-water
/// mark for an hour while every refill is being retried three times through a
/// cold start — and that is the hour to find out, not the one after it drains.
///
/// ```
/// <prefix>.requests     {slugkit_client, operation, outcome}  rate — one per round trip, retries included
/// <prefix>.retries      {slugkit_client, operation}         rate — round trips that were tried again
/// <prefix>.duration-ms  {slugkit_client, operation}         histogram — one per round trip
/// ```
///
/// A round trip rather than a call is the unit, because a call that succeeded
/// on its fourth attempt is three failures and a success, and counting it as
/// one success is how a degrading dependency hides behind a retry loop.
///
/// `outcome` follows the exception the SDK throws (slugkit/exceptions.hpp),
/// so an operator reading a series and a developer reading a catch block use
/// the same word for the same thing.

#include <array>
#include <chrono>
#include <cstddef>
#include <string_view>

#include <userver/utils/statistics/by_label_storage.hpp>
#include <userver/utils/statistics/histogram.hpp>
#include <userver/utils/statistics/rate_counter.hpp>
#include <userver/utils/statistics/writer.hpp>

namespace slugkit::sdk {

enum class Outcome {
    kOk,
    /// No answer at all: connection, DNS, timeout.
    kTransportError,
    kUnauthorized,
    kForbidden,
    kNotFound,
    kRateLimited,
    /// Any other 4xx.
    kClientError,
    kServerError,
    /// A 2xx whose body was not what the endpoint promises.
    kMalformed,
};

inline constexpr std::size_t kOutcomeCount = 9;

/// The outcome of an HTTP status, by the same mapping the client's exceptions
/// use. A 2xx is @ref Outcome::kOk.
[[nodiscard]] auto OutcomeForStatus(int status) noexcept -> Outcome;

[[nodiscard]] auto MetricLabel(Outcome outcome) noexcept -> std::string_view;

class ClientMetrics final {
public:
    ClientMetrics() = default;
    ClientMetrics(const ClientMetrics&) = delete;
    auto operator=(const ClientMetrics&) -> ClientMetrics& = delete;

    /// Creates the series for @p operation before its first call, so a rate
    /// alert has a history of zeros to compare the first failure against.
    void Declare(std::string_view operation);

    /// One round trip.
    void Account(std::string_view operation, Outcome outcome, std::chrono::milliseconds elapsed);

    /// A round trip that failed and will be tried again.
    void AccountRetry(std::string_view operation);

    friend void DumpMetric(userver::utils::statistics::Writer& writer, const ClientMetrics& metrics);
    friend void ResetMetric(ClientMetrics& metrics);

    /// One operation's counters. Public only so the storage can name it.
    struct Operation final {
        Operation();

        std::array<userver::utils::statistics::RateCounter, kOutcomeCount> requests{};
        userver::utils::statistics::RateCounter retries{};
        userver::utils::statistics::Histogram duration_ms;
    };

    struct OperationLabels final {
        std::string_view operation;
    };

private:
    userver::utils::statistics::MonotonicByLabelStorage<OperationLabels, Operation> operations_;
};

void DumpMetric(userver::utils::statistics::Writer& writer, const ClientMetrics::Operation& operation);
void ResetMetric(ClientMetrics::Operation& operation);

}  // namespace slugkit::sdk
