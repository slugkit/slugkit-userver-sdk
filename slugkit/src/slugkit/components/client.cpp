#include <slugkit/components/client.hpp>

#include <slugkit/exceptions.hpp>

#include <userver/clients/http/client.hpp>
#include <userver/clients/http/component.hpp>
#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/logging/log.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <fmt/format.h>

#include <chrono>
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
    }

    [[nodiscard]] auto Post(std::string_view url, json::Value body, std::string_view operation) const
        -> json::Value {
        auto request_body = json::ToString(body);
        std::shared_ptr<http::Response> response;
        try {
            response = http_client.GetHttpClient()
                           .CreateRequest()
                           .post(std::string{url}, std::move(request_body))
                           .headers({
                               {"content-type", "application/json"},
                               {"accept", "application/json"},
                               {"x-api-key", api_key},
                               {"user-agent", user_agent},
                           })
                           .timeout(request_timeout)
                           .perform();
        } catch (const std::exception& e) {
            LOG_WARNING() << "slugkit " << operation << " transport error: " << e.what();
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
