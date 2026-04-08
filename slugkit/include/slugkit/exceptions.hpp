#pragma once

#include <stdexcept>
#include <string>

namespace slugkit::sdk {

/// Base type for every error the SDK can raise. Catch this if you want
/// blanket SlugKit failure handling; catch a more specific subtype if
/// you care about the cause (e.g. ``Unauthorized`` for a bad API key).
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Network/transport failure — request never reached the SlugKit
/// service, or the response could not be read.
class TransportError : public Error {
public:
    using Error::Error;
};

/// HTTP 401 — the API key was missing, malformed, or revoked.
class Unauthorized : public Error {
public:
    using Error::Error;
};

/// HTTP 403 — the API key is valid but lacks the required scope (e.g.
/// no ``mint`` scope, or the org claim doesn't match the target org).
class Forbidden : public Error {
public:
    using Error::Error;
};

/// HTTP 404 — the addressed resource (typically a series) does not
/// exist on the server side.
class NotFound : public Error {
public:
    using Error::Error;
};

/// HTTP 429 — rate limit or quota exhausted. Retry with backoff.
class RateLimited : public Error {
public:
    using Error::Error;
};

/// HTTP 4xx other than the named cases above — usually a request shape
/// problem the caller can fix.
class ClientError : public Error {
public:
    using Error::Error;
};

/// HTTP 5xx — server-side failure. Generally retryable.
class ServerError : public Error {
public:
    using Error::Error;
};

}  // namespace slugkit::sdk
