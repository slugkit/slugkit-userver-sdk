#include <slugkit/pool/memory_storage.hpp>

#include <utility>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace slugkit::sdk::pool {

auto MemoryPool::Size(const SeriesSlug& series) const -> std::int64_t {
    const auto pools = pools_.Lock();
    const auto it = pools->find(series.GetUnderlying());
    if (it == pools->end()) {
        return 0;
    }
    return static_cast<std::int64_t>(it->second.queue.size());
}

auto MemoryPool::Refill(const SeriesSlug& series, const std::vector<Slug>& slugs) -> std::int64_t {
    if (slugs.empty()) {
        return 0;
    }
    auto pools = pools_.Lock();
    auto& pool = (*pools)[series.GetUnderlying()];
    std::int64_t added = 0;
    for (const auto& slug : slugs) {
        if (pool.present.insert(slug.GetUnderlying()).second) {
            pool.queue.push_back(slug);
            ++added;
        }
    }
    return added;
}

auto MemoryPool::Draw(const SeriesSlug& series) -> std::optional<Slug> {
    auto pools = pools_.Lock();
    const auto it = pools->find(series.GetUnderlying());
    if (it == pools->end() || it->second.queue.empty()) {
        return std::nullopt;
    }
    auto slug = std::move(it->second.queue.front());
    it->second.queue.pop_front();
    it->second.present.erase(slug.GetUnderlying());
    return slug;
}

MemoryStorage::MemoryStorage(
    const userver::components::ComponentConfig& config,
    const userver::components::ComponentContext& context
)
    : ComponentBase{config, context} {}

MemoryStorage::~MemoryStorage() = default;

auto MemoryStorage::GetStaticConfigSchema() -> userver::yaml_config::Schema {
    return userver::yaml_config::MergeSchemas<userver::components::ComponentBase>(R"(
type: object
description: An in-memory slug pool. Not durable, not shared between replicas.
additionalProperties: false
properties: {}
)");
}

auto MemoryStorage::Size(const SeriesSlug& series) const -> std::int64_t { return pool_.Size(series); }

auto MemoryStorage::Refill(const SeriesSlug& series, const std::vector<Slug>& slugs) -> std::int64_t {
    return pool_.Refill(series, slugs);
}

auto MemoryStorage::Draw(const SeriesSlug& series) -> std::optional<Slug> { return pool_.Draw(series); }

}  // namespace slugkit::sdk::pool
