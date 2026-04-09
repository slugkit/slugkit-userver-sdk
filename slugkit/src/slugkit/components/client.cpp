#include <slugkit/components/client.hpp>

#include <slugkit/exceptions.hpp>

#include <userver/clients/http/client.hpp>
#include <userver/clients/http/component.hpp>
#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/logging/log.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace slugkit::sdk::components {

namespace {

namespace json = userver::formats::json;
namespace http = userver::clients::http;

constexpr auto kDefaultRequestTimeout = std::chrono::milliseconds{10000};
constexpr auto kDefaultUserAgent = "slugkit-userver-sdk/1.0";
constexpr auto kApiBase = "/api/v1/gen";

/// Default retry tunables. SlugKit's serverless deployment can cold-start
/// (sleeping instances), and transient 5xx / network blips happen in
/// practice — so the SDK retries retriable failures with exponential
/// backoff out of the box. Callers can override every knob via static
/// config; setting ``max-attempts: 1`` disables retries entirely.
constexpr auto kDefaultMaxAttempts = std::size_t{4};
constexpr auto kDefaultInitialBackoff = std::chrono::milliseconds{250};
constexpr auto kDefaultMaxBackoff = std::chrono::milliseconds{4000};

struct RetryPolicy {
    std::size_t max_attempts{kDefaultMaxAttempts};
    std::chrono::milliseconds initial_backoff{kDefaultInitialBackoff};
    std::chrono::milliseconds max_backoff{kDefaultMaxBackoff};
};

/// Compute the delay before attempt ``attempt_index`` (1-based: the
/// delay returned is the wait *after* attempt ``attempt_index`` failed
/// and before the next try). Pure exponential backoff capped at
/// ``policy.max_backoff``; no jitter — userver's cooperative scheduler
/// already smears wakeups across coroutines well enough that thundering
/// herds aren't the SDK's problem to solve.
auto BackoffFor(const RetryPolicy& policy, std::size_t attempt_index) -> std::chrono::milliseconds {
    // attempt_index is 1 for the first failure, 2 for the second, etc.
    // delay = initial * 2^(attempt_index - 1), clamped at max_backoff.
    auto shift = attempt_index > 0 ? attempt_index - 1 : 0;
    // Guard against overflow on absurd configs — anything past ~20
    // doublings is meaningless once clamped, so saturate the shift.
    shift = std::min<std::size_t>(shift, 20);
    auto scaled = std::chrono::milliseconds{
        policy.initial_backoff.count() * (std::int64_t{1} << shift),
    };
    return scaled < policy.max_backoff ? scaled : policy.max_backoff;
}

/// Pull a server-emitted error reason out of a JSON body. SlugKit's
/// error responses are usually ``{"error": "..."}`` or
/// ``{"detail": "..."}``; fall back to the raw body if neither key
/// is present.
auto ExtractReason(std::string_view body) -> std::string {
    try {
        auto value = json::FromString(body);
        if (value.HasMember("error")) {
            return value["error"].As<std::string>();
        }
        if (value.HasMember("detail")) {
            return value["detail"].As<std::string>();
        }
    } catch (const std::exception&) {
        // Not JSON — fall through.
    }
    return std::string{body};
}

[[noreturn]] void ThrowForStatus(int status, std::string_view operation, std::string_view body) {
    auto reason = ExtractReason(body);
    auto message = fmt::format("slugkit {}: HTTP {} — {}", operation, status, reason);
    switch (status) {
        case 401:
            throw Unauthorized{std::move(message)};
        case 403:
            throw Forbidden{std::move(message)};
        case 404:
            throw NotFound{std::move(message)};
        case 429:
            throw RateLimited{std::move(message)};
        default:
            break;
    }
    if (status >= 500) {
        throw ServerError{std::move(message)};
    }
    if (status >= 400) {
        throw ClientError{std::move(message)};
    }
    // 2xx-3xx land here only if a caller misroutes — still surface as
    // an error rather than silently succeeding.
    throw Error{std::move(message)};
}

}  // namespace

struct Client::Impl {
    userver::components::HttpClient& http_client;
    std::string base_url;
    std::string api_key;
    std::string user_agent;
    std::chrono::milliseconds request_timeout;
    RetryPolicy retry_policy;

    std::string mint_url;
    std::string forge_url;
    std::string slice_url;
    std::string reset_url;
    std::string pattern_info_url;

    Impl(
        const userver::components::ComponentConfig& config,
        const userver::components::ComponentContext& context
    )
        : http_client(context.FindComponent<userver::components::HttpClient>())
        , base_url(config["base-url"].As<std::string>())
        , api_key(config["api-key"].As<std::string>())
        , user_agent(config["user-agent"].As<std::string>(kDefaultUserAgent))
        , request_timeout(config["request-timeout"].As<std::chrono::milliseconds>(kDefaultRequestTimeout))
        , retry_policy{
              config["retry"]["max-attempts"].As<std::size_t>(kDefaultMaxAttempts),
              config["retry"]["initial-backoff"].As<std::chrono::milliseconds>(kDefaultInitialBackoff),
              config["retry"]["max-backoff"].As<std::chrono::milliseconds>(kDefaultMaxBackoff),
          }
        , mint_url(fmt::format("{}{}/mint", base_url, kApiBase))
        , forge_url(fmt::format("{}{}/forge", base_url, kApiBase))
        , slice_url(fmt::format("{}{}/slice", base_url, kApiBase))
        , reset_url(fmt::format("{}{}/reset", base_url, kApiBase))
        , pattern_info_url(fmt::format("{}{}/pattern-info", base_url, kApiBase)) {
        if (base_url.empty()) {
            throw std::runtime_error("slugkit-client: base-url is required");
        }
        if (api_key.empty()) {
            throw std::runtime_error("slugkit-client: api-key is empty (set SLUGKIT_API_KEY)");
        }
        if (retry_policy.max_attempts == 0) {
            throw std::runtime_error("slugkit-client: retry.max-attempts must be >= 1");
        }
    }

    /// One round-trip. Maps transport failures to ``TransportError`` and
    /// non-2xx statuses to the matching ``Error`` subtype. The retry
    /// loop in ``Post`` decides which of those are worth re-trying.
    [[nodiscard]] auto PostOnce(std::string_view url, const std::string& request_body, std::string_view operation)
        const -> json::Value {
        std::shared_ptr<http::Response> response;
        try {
            response = http_client.GetHttpClient()
                           .CreateRequest()
                           .post(std::string{url}, request_body)
                           .headers({
                               {"content-type", "application/json"},
                               {"accept", "application/json"},
                               {"x-api-key", api_key},
                               {"user-agent", user_agent},
                           })
                           .timeout(request_timeout)
                           .perform();
        } catch (const std::exception& e) {
            throw TransportError{fmt::format("slugkit {}: {}", operation, e.what())};
        }

        const auto status = static_cast<int>(response->status_code());
        const auto body_str = response->body();
        if (status / 100 != 2) {
            ThrowForStatus(status, operation, body_str);
        }
        try {
            return json::FromString(body_str);
        } catch (const std::exception& e) {
            throw Error{fmt::format("slugkit {}: malformed JSON response — {}", operation, e.what())};
        }
    }

    /// Retry-aware POST. Retriable failures — transport errors, HTTP 5xx,
    /// and HTTP 429 — are re-tried up to ``retry_policy.max_attempts``
    /// times with exponential backoff. Anything else (4xx other than 429,
    /// malformed JSON) propagates immediately because retrying won't help.
    [[nodiscard]] auto Post(std::string_view url, json::Value body, std::string_view operation) const
        -> json::Value {
        auto request_body = json::ToString(body);
        for (std::size_t attempt = 1;; ++attempt) {
            try {
                return PostOnce(url, request_body, operation);
            } catch (const TransportError& e) {
                if (attempt >= retry_policy.max_attempts) {
                    LOG_WARNING() << "slugkit " << operation << " transport error after " << attempt
                                  << " attempt(s): " << e.what();
                    throw;
                }
                auto delay = BackoffFor(retry_policy, attempt);
                LOG_WARNING() << "slugkit " << operation << " transport error on attempt " << attempt << "/"
                              << retry_policy.max_attempts << ", retrying in " << delay.count() << "ms: "
                              << e.what();
                userver::engine::SleepFor(delay);
            } catch (const ServerError& e) {
                if (attempt >= retry_policy.max_attempts) {
                    LOG_WARNING() << "slugkit " << operation << " server error after " << attempt
                                  << " attempt(s): " << e.what();
                    throw;
                }
                auto delay = BackoffFor(retry_policy, attempt);
                LOG_WARNING() << "slugkit " << operation << " 5xx on attempt " << attempt << "/"
                              << retry_policy.max_attempts << ", retrying in " << delay.count() << "ms: "
                              << e.what();
                userver::engine::SleepFor(delay);
            } catch (const RateLimited& e) {
                if (attempt >= retry_policy.max_attempts) {
                    LOG_WARNING() << "slugkit " << operation << " rate-limited after " << attempt
                                  << " attempt(s): " << e.what();
                    throw;
                }
                auto delay = BackoffFor(retry_policy, attempt);
                LOG_WARNING() << "slugkit " << operation << " 429 on attempt " << attempt << "/"
                              << retry_policy.max_attempts << ", retrying in " << delay.count() << "ms: "
                              << e.what();
                userver::engine::SleepFor(delay);
            }
        }
    }

    [[nodiscard]] auto ParseSlugList(const json::Value& value, std::string_view operation) const
        -> std::vector<Slug> {
        if (!value.IsArray()) {
            throw Error{fmt::format("slugkit {}: expected JSON array, got {}", operation, json::ToString(value))};
        }
        std::vector<Slug> slugs;
        slugs.reserve(value.GetSize());
        for (const auto& item : value) {
            slugs.push_back(Slug{item.As<std::string>()});
        }
        return slugs;
    }
};

Client::Client(
    const userver::components::ComponentConfig& config,
    const userver::components::ComponentContext& context
)
    : userver::components::ComponentBase{config, context}
    , impl_{config, context} {}

Client::~Client() = default;

auto Client::GetStaticConfigSchema() -> userver::yaml_config::Schema {
    return userver::yaml_config::MergeSchemas<userver::components::ComponentBase>(R"(
type: object
description: SlugKit HTTP API client
additionalProperties: false
properties:
    base-url:
        type: string
        description: Base URL of the SlugKit server (no trailing slash)
    api-key:
        type: string
        description: API key sent in the X-API-Key header. Source via #env in production.
    user-agent:
        type: string
        description: User-Agent header sent with every request
        defaultDescription: slugkit-userver-sdk/1.0
    request-timeout:
        type: string
        description: HTTP request timeout
        defaultDescription: 10s
    retry:
        type: object
        description: |
            Retry policy for transient failures (transport errors, HTTP
            5xx, HTTP 429). Non-retriable errors (4xx other than 429,
            malformed JSON) bypass the loop entirely.
        additionalProperties: false
        properties:
            max-attempts:
                type: integer
                description: |
                    Total attempts including the first one. ``1`` disables
                    retries; the SDK still maps failures to typed errors.
                defaultDescription: '4'
            initial-backoff:
                type: string
                description: |
                    Wait before the first retry. Subsequent retries
                    double this delay until ``max-backoff`` is reached.
                defaultDescription: 250ms
            max-backoff:
                type: string
                description: |
                    Cap on the per-retry wait. Exponential growth stops
                    here; further retries reuse the same delay.
                defaultDescription: 4s
    )");
}

auto Client::Mint(const dto::MintRequest& request) const -> std::vector<Slug> {
    auto body = Serialize(request, userver::formats::serialize::To<json::Value>{});
    auto response = impl_->Post(impl_->mint_url, std::move(body), "mint");
    return impl_->ParseSlugList(response, "mint");
}

auto Client::MintOne(const dto::MintRequest& request) const -> Slug {
    auto slugs = Mint(request);
    if (slugs.empty()) {
        throw Error{"slugkit mint: server returned an empty slug list"};
    }
    return slugs.front();
}

auto Client::Forge(const dto::ForgeRequest& request) const -> std::vector<Slug> {
    auto body = Serialize(request, userver::formats::serialize::To<json::Value>{});
    auto response = impl_->Post(impl_->forge_url, std::move(body), "forge");
    return impl_->ParseSlugList(response, "forge");
}

auto Client::ForgeOne(const dto::ForgeRequest& request) const -> Slug {
    auto slugs = Forge(request);
    if (slugs.empty()) {
        throw Error{"slugkit forge: server returned an empty slug list"};
    }
    return slugs.front();
}

auto Client::Slice(const dto::SliceRequest& request) const -> std::vector<Slug> {
    auto body = Serialize(request, userver::formats::serialize::To<json::Value>{});
    auto response = impl_->Post(impl_->slice_url, std::move(body), "slice");
    return impl_->ParseSlugList(response, "slice");
}

auto Client::SliceOne(const dto::SliceRequest& request) const -> Slug {
    auto slugs = Slice(request);
    if (slugs.empty()) {
        throw Error{"slugkit slice: server returned an empty slug list"};
    }
    return slugs.front();
}

auto Client::Reset(const dto::ResetRequest& request) const -> void {
    auto body = Serialize(request, userver::formats::serialize::To<json::Value>{});
    (void)impl_->Post(impl_->reset_url, std::move(body), "reset");
}

auto Client::GetPatternInfo(const dto::PatternInfoRequest& request) const -> dto::PatternInfoResponse {
    auto body = Serialize(request, userver::formats::serialize::To<json::Value>{});
    auto response = impl_->Post(impl_->pattern_info_url, std::move(body), "pattern-info");
    return response.As<dto::PatternInfoResponse>();
}

}  // namespace slugkit::sdk::components
