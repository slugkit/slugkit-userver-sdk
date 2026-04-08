#pragma once

#include <slugkit/types.hpp>

#include <userver/formats/serialize/boost_uuid.hpp>
#include <userver/formats/serialize/to.hpp>

#include <cstdint>

namespace slugkit::sdk::dto {

/// Slice fetches deterministic slugs from a series **without** advancing
/// its sequence — handy for replaying a window or pre-computing a UI
/// preview. ``sequence`` selects the starting offset; ``count`` is how
/// many slugs to return from there.
struct SliceRequest {
    OptionalOrgId org_id;
    SeriesSlug series;
    std::int64_t sequence{0};
    std::int32_t count{1};
};

template <typename Format>
auto Serialize(const SliceRequest& request, userver::formats::serialize::To<Format>) -> Format {
    typename Format::Builder builder;
    if (request.org_id) {
        builder["org_id"] = request.org_id->GetUnderlying();
    }
    builder["series"] = request.series.GetUnderlying();
    builder["sequence"] = request.sequence;
    builder["count"] = request.count;
    return builder.ExtractValue();
}

}  // namespace slugkit::sdk::dto
