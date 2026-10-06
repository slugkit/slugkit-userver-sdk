#include <slugkit/metrics.hpp>

#include <userver/utils/statistics/labels.hpp>

namespace slugkit::sdk {

namespace {

/// Milliseconds. SlugKit answers a warm mint in tens of milliseconds and a
/// cold start in seconds; the buckets are spaced so both show up as shapes
/// rather than as one overflowing bucket.
constexpr std::array<double, 10> kDurationBucketsMs{10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000};

constexpr std::array kOutcomes{
    Outcome::kOk,          Outcome::kTransportError, Outcome::kUnauthorized,
    Outcome::kForbidden,   Outcome::kNotFound,       Outcome::kRateLimited,
    Outcome::kClientError, Outcome::kServerError,    Outcome::kMalformed,
};
static_assert(kOutcomes.size() == kOutcomeCount);

auto Index(Outcome outcome) noexcept -> std::size_t { return static_cast<std::size_t>(outcome); }

}  // namespace

auto OutcomeForStatus(int status) noexcept -> Outcome {
    namespace http = userver::server::http;
    if (IsSuccess(status)) return Outcome::kOk;
    switch (status) {
        case http::HttpStatus::kUnauthorized:
            return Outcome::kUnauthorized;
        case http::HttpStatus::kForbidden:
            return Outcome::kForbidden;
        case http::HttpStatus::kNotFound:
            return Outcome::kNotFound;
        case http::HttpStatus::kTooManyRequests:
            return Outcome::kRateLimited;
        default:
            break;
    }
    if (status >= http::HttpStatus::kInternalServerError) return Outcome::kServerError;
    return Outcome::kClientError;
}

auto MetricLabel(Outcome outcome) noexcept -> std::string_view {
    switch (outcome) {
        case Outcome::kOk:
            return "ok";
        case Outcome::kTransportError:
            return "transport_error";
        case Outcome::kUnauthorized:
            return "unauthorized";
        case Outcome::kForbidden:
            return "forbidden";
        case Outcome::kNotFound:
            return "not_found";
        case Outcome::kRateLimited:
            return "rate_limited";
        case Outcome::kClientError:
            return "client_error";
        case Outcome::kServerError:
            return "server_error";
        case Outcome::kMalformed:
            return "malformed";
    }
    return "client_error";
}

ClientMetrics::Operation::Operation() : duration_ms{kDurationBucketsMs} {}

void ClientMetrics::Declare(std::string_view operation) { (void)operations_[{operation}]; }

void ClientMetrics::Account(std::string_view operation, Outcome outcome, std::chrono::milliseconds elapsed) {
    auto& counters = operations_[{operation}];
    ++counters.requests[Index(outcome)];
    counters.duration_ms.Account(static_cast<double>(elapsed.count()));
}

void ClientMetrics::AccountRetry(std::string_view operation) { ++operations_[{operation}].retries; }

void DumpMetric(userver::utils::statistics::Writer& writer, const ClientMetrics::Operation& operation) {
    for (const auto outcome : kOutcomes) {
        writer["requests"].ValueWithLabels(
            operation.requests[Index(outcome)], userver::utils::statistics::LabelView{"outcome", MetricLabel(outcome)}
        );
    }
    writer["retries"] = operation.retries;
    writer["duration-ms"] = operation.duration_ms;
}

void ResetMetric(ClientMetrics::Operation& operation) {
    for (auto& counter : operation.requests) ResetMetric(counter);
    ResetMetric(operation.retries);
    ResetMetric(operation.duration_ms);
}

void DumpMetric(userver::utils::statistics::Writer& writer, const ClientMetrics& metrics) {
    writer = metrics.operations_;
}

void ResetMetric(ClientMetrics& metrics) { ResetMetric(metrics.operations_); }

}  // namespace slugkit::sdk
