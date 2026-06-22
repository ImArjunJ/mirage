#include "core/receiver_source.hpp"

#include <algorithm>
#include <utility>

namespace mirage {

receiver_source_registry::receiver_source_registry(std::vector<receiver_source_descriptor> sources)
    : sources_(std::move(sources)) {}

std::span<const receiver_source_descriptor> receiver_source_registry::all() const {
    return {sources_.data(), sources_.size()};
}

const receiver_source_descriptor* receiver_source_registry::find(protocol id) const {
    auto it = std::ranges::find(sources_, id, &receiver_source_descriptor::id);
    if (it == sources_.end()) {
        return nullptr;
    }
    return &*it;
}

std::vector<receiver_source_descriptor> receiver_source_registry::enabled() const {
    std::vector<receiver_source_descriptor> result;
    for (const auto& source : sources_) {
        if (source.enabled) {
            result.push_back(source);
        }
    }
    return result;
}

}  // namespace mirage
