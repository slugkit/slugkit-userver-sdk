#include <slugkit/secrets.hpp>

namespace slugkit::sdk {

Secrets::Secrets(const userver::formats::json::Value& doc) {
    const auto& root = doc["slugkit"];
    if (!root.IsObject()) {
        return;
    }

    for (auto it = root.begin(); it != root.end(); ++it) {
        const auto& block = *it;
        if (!block.IsObject()) {
            continue;
        }

        Credentials credentials;
        credentials.api_key = block["api_key"].As<std::string>("");
        if (block.HasMember("series")) {
            credentials.series = block["series"].As<std::string>("");
        }
        if (block.HasMember("base_url")) {
            credentials.base_url = block["base_url"].As<std::string>("");
        }
        blocks_.emplace(it.GetName(), std::move(credentials));
    }
}

auto Secrets::Find(std::string_view alias) const -> const Credentials* {
    const auto found = blocks_.find(std::string{alias});
    return found == blocks_.end() ? nullptr : &found->second;
}

}  // namespace slugkit::sdk
