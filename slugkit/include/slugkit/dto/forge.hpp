#pragma once

#include <slugkit/types.hpp>

#include <userver/formats/json/value.hpp>
#include <userver/formats/serialize/to.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace slugkit::sdk::dto {

/// Forge generates slugs from a raw pattern, **without** advancing any
/// persistent series state. Useful for previewing what a pattern looks
/// like or for one-shot generation that doesn't need a series.
///
/// ``selectors`` is left as a free-form JSON value because the selector
/// vocabulary is data-driven on the server side; the SDK doesn't model
/// individual selector kinds. Pass whatever the server documents accept;
/// leave it null to skip the field entirely.
struct ForgeRequest {
    std::string pattern;
    std::int32_t count{1};
    std::optional<std::string> seed;
    std::int64_t sequence{0};
    userver::formats::json::Value selectors;
};

template <typename Format>
auto Serialize(const ForgeRequest& request, userver::formats::serialize::To<Format>) -> Format {
    typename Format::Builder builder;
    builder["pattern"] = request.pattern;
    builder["count"] = request.count;
    if (request.seed) {
        builder["seed"] = *request.seed;
    }
    builder["sequence"] = request.sequence;
    if (!request.selectors.IsNull() && !request.selectors.IsMissing()) {
        builder["selectors"] = request.selectors;
    }
    return builder.ExtractValue();
}

}  // namespace slugkit::sdk::dto
