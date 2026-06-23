#pragma once

#include <vector>

#include "core/core.hpp"
#include "core/receiver_source.hpp"

namespace mirage::protocols {

std::vector<receiver_source_descriptor> make_receiver_source_descriptors(const config& cfg);

receiver_source_descriptor make_airplay_receiver_source(const config& cfg);
receiver_source_descriptor make_cast_receiver_source(const config& cfg);
receiver_source_descriptor make_wfd_receiver_source(const config& cfg);

}  // namespace mirage::protocols
