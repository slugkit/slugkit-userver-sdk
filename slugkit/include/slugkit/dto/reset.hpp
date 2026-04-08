#pragma once

#include <slugkit/types.hpp>

#include <userver/formats/serialize/boost_uuid.hpp>
#include <userver/formats/serialize/to.hpp>

namespace slugkit::sdk::dto {

/// Reset rewinds a series back to sequence 0 — the next ``Mint`` will
/// return the first slug of the pattern again. Destructive on the
/// server side; the API key needs the appropriate scope.
struct ResetRequest {
    OptionalOrgId org_id;
    SeriesSlug series;
};

template <typename Format>
auto Serialize(const ResetRequest& request, userver::formats::serialize::To<Format>) -> Format {
    typename Format::Builder builder;
    if (request.org_id) {
        builder["org_id"] = request.org_id->GetUnderlying();
    }
    builder["series"] = request.series.GetUnderlying();
    return builder.ExtractValue();
}

}  // namespace slugkit::sdk::dto
