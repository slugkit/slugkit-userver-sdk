#pragma once

#include <userver/formats/json/value.hpp>
#include <userver/formats/parse/to.hpp>
#include <userver/formats/serialize/to.hpp>

#include <cstdint>
#include <string>

namespace slugkit::sdk::dto {

/// Ask the server to analyse a pattern without generating any slugs from
/// it. Returns the pattern's vocabulary footprint — capacity (how many
/// distinct slugs it can produce), the longest possible slug, etc. The
/// SDK exposes this raw because the response shape evolves with the
/// server vocabulary.
struct PatternInfoRequest {
    std::string pattern;
};

template <typename Format>
auto Serialize(const PatternInfoRequest& request, userver::formats::serialize::To<Format>) -> Format {
    typename Format::Builder builder;
    builder["pattern"] = request.pattern;
    return builder.ExtractValue();
}

/// The shape of a ``/api/v1/gen/pattern-info`` response. ``capacity`` is
/// the only field the SDK forces a type on (string-decimal because it
/// can be larger than ``uint64_t``); the rest may grow as the server
/// adds richer analysis. ``raw`` carries the full JSON for callers that
/// need more than the typed fields.
struct PatternInfoResponse {
    std::string pattern;
    std::string capacity;
    std::int32_t max_slug_length{0};
    std::int32_t complexity{0};
    std::uint32_t components{0};
    userver::formats::json::Value raw;
};

template <typename Value>
auto Parse(const Value& value, userver::formats::parse::To<PatternInfoResponse>) -> PatternInfoResponse {
    PatternInfoResponse result;
    result.pattern = value["pattern"].template As<std::string>();
    if (value.HasMember("capacity")) {
        // The server emits this either as a JSON string (so values
        // bigger than 2^53 round-trip safely) or, for small patterns,
        // as a number. Accept both.
        const auto capacity = value["capacity"];
        if (capacity.IsString()) {
            result.capacity = capacity.template As<std::string>();
        } else if (capacity.IsInt64()) {
            result.capacity = std::to_string(capacity.template As<std::int64_t>());
        } else if (capacity.IsUInt64()) {
            result.capacity = std::to_string(capacity.template As<std::uint64_t>());
        }
    }
    if (value.HasMember("max_slug_length")) {
        result.max_slug_length = value["max_slug_length"].template As<std::int32_t>(0);
    }
    if (value.HasMember("complexity")) {
        result.complexity = value["complexity"].template As<std::int32_t>(0);
    }
    if (value.HasMember("components")) {
        result.components = value["components"].template As<std::uint32_t>(0);
    }
    result.raw = value.template As<userver::formats::json::Value>();
    return result;
}

}  // namespace slugkit::sdk::dto
