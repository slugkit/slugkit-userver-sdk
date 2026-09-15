#include <slugkit/secrets.hpp>

#include <userver/formats/json/serialize.hpp>
#include <userver/utest/utest.hpp>

namespace {

using slugkit::sdk::Secrets;
using userver::formats::json::FromString;

TEST(SlugkitSecrets, FindsTheBlockNamedByTheAlias) {
    const Secrets secrets{FromString(R"({
        "slugkit": {
            "accounts": {"api_key": "sk-accounts", "series": "account-slugs", "base_url": "https://slugkit.example.com"},
            "second":  {"api_key": "sk-second"}
        }
    })")};

    const auto* first = secrets.Find("accounts");
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->api_key, "sk-accounts");
    ASSERT_TRUE(first->series.has_value());
    EXPECT_EQ(*first->series, "account-slugs");
    ASSERT_TRUE(first->base_url.has_value());
    EXPECT_EQ(*first->base_url, "https://slugkit.example.com");

    // A block carrying neither leaves both unset, so the static config's values
    // stand rather than being overwritten with empty strings.
    const auto* second = secrets.Find("second");
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->api_key, "sk-second");
    EXPECT_FALSE(second->series.has_value());
    EXPECT_FALSE(second->base_url.has_value());
}

TEST(SlugkitSecrets, AnUnknownAliasIsAbsentNotAnError) {
    const Secrets secrets{FromString(R"({"slugkit": {"accounts": {"api_key": "sk"}}})")};
    EXPECT_EQ(secrets.Find("skus"), nullptr);
}

TEST(SlugkitSecrets, ADocumentWithoutSlugkitHoldsNothing) {
    // Every component shares one secdist document; one that carries only other
    // services' secrets must parse, not throw.
    const Secrets secrets{FromString(R"({"postgresql_settings": {}})")};
    EXPECT_EQ(secrets.Find("accounts"), nullptr);

    const Secrets malformed{FromString(R"({"slugkit": "not an object"})")};
    EXPECT_EQ(malformed.Find("accounts"), nullptr);
}

}  // namespace
