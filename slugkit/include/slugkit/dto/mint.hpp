#pragma once

#include <slugkit/types.hpp>

#include <userver/formats/serialize/boost_uuid.hpp>
#include <userver/formats/serialize/to.hpp>

#include <cstdint>

namespace slugkit::sdk::dto {

/// Mint draws from a persistent series — every call advances a server-side
/// sequence so the next slug is fresh. The series carries its own pattern,
/// org binding, and quota; the SDK only needs the slug to address it.
///
/// ``org_id`` is only meaningful for multi-org API keys. Single-org keys
/// can leave it empty and the server will infer it from the key claims.
struct MintRequest {
    OptionalOrgId org_id;
    SeriesSlug series;
    std::int32_t count{1};
};

template <typename Format>
auto Serialize(const MintRequest& request, userver::formats::serialize::To<Format>) -> Format {
    typename Format::Builder builder;
    if (request.org_id) {
        builder["org_id"] = request.org_id->GetUnderlying();
    }
    builder["series"] = request.series.GetUnderlying();
    builder["count"] = request.count;
    return builder.ExtractValue();
}

}  // namespace slugkit::sdk::dto
