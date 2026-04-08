#pragma once

#include <userver/utils/strong_typedef.hpp>

#include <boost/uuid/uuid.hpp>

#include <optional>
#include <string>

namespace slugkit::sdk {

/// A generated slug — opaque to the SDK; the server is the source of truth.
using Slug = userver::utils::StrongTypedef<class SlugTag, std::string>;

/// The slug of a series defined on the SlugKit account. Used to address
/// a series across the API surface (mint, slice, reset, pattern-info).
using SeriesSlug = userver::utils::StrongTypedef<class SeriesSlugTag, std::string>;

/// Organisation ID. Multi-org API keys identify the target org explicitly
/// per request; single-org keys leave it implicit.
using OrgId = userver::utils::StrongTypedef<class OrgIdTag, boost::uuids::uuid>;

using OptionalOrgId = std::optional<OrgId>;
using OptionalSeriesSlug = std::optional<SeriesSlug>;

}  // namespace slugkit::sdk
