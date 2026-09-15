#pragma once

/// @file
/// The SlugKit API key out of the static config and into secdist.
///
/// An API key is a credential, and a credential in a static config is a
/// credential in a file: userver renders config through templates onto disk, so
/// a deployment that fills `api-key` from an environment variable still writes it
/// out, and the variable itself is one more place a rotation has to reach.
/// secdist is read once at startup, from a document that never becomes part of
/// the rendered config.
///
/// The expected document, keyed by the client's `secdist-alias`:
///
/// ```json
/// {
///   "slugkit": {
///     "accounts": {
///       "api_key":  "sk_…",
///       "series":   "apart-rushy-hooey-ac1b",
///       "base_url": "https://slugkit.example.com"
///     }
///   }
/// }
/// ```
///
/// Only `api_key` is a secret. `series` and `base_url` travel with it because
/// both belong to the account the key is for: a series lives in one
/// organisation, on one server. Keeping the three together makes pointing a
/// consumer at another account one change to one block, rather than a key
/// change plus a redeploy for the config that names the series. Either of the
/// two may be omitted and taken from the static config instead.
///
/// Keyed by alias, and named after the consumer rather than the provider —
/// `accounts`, `skus` — so two pools minting two series hold two keys that can
/// be scoped and rotated separately.

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <userver/formats/json/value.hpp>

namespace slugkit::sdk {

/// One block of the secdist document.
struct Credentials {
    std::string api_key;
    /// The series this key's consumer mints from. Optional: a client that mints
    /// from several series names them in its own config instead.
    std::optional<std::string> series;
    std::optional<std::string> base_url;
};

class Secrets final {
public:
    Secrets() = default;
    explicit Secrets(const userver::formats::json::Value& doc);

    /// @returns the block under @p alias, or nullptr when the document has none.
    ///
    /// Absence is not an error here. Every component in a process shares one
    /// secdist document, and the client decides what a missing block means,
    /// because only it knows whether it was told to expect one.
    [[nodiscard]] auto Find(std::string_view alias) const -> const Credentials*;

private:
    std::unordered_map<std::string, Credentials> blocks_;
};

}  // namespace slugkit::sdk
