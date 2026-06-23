#include "protocols/receiver_sources.hpp"

#include <vector>

namespace mirage::protocols {

std::vector<receiver_source_descriptor> make_receiver_source_descriptors(const config& cfg) {
    return {
        make_airplay_receiver_source(cfg),
        make_cast_receiver_source(cfg),
        make_wfd_receiver_source(cfg),
    };
}

}  // namespace mirage::protocols
