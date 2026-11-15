#pragma once

#include <expected>
#include <string>

#include <iox2/config.hpp>

namespace foxglove_bridge_iox2_plugin {

/// Reads the schema registered with a service from iceoryx2's storage, without opening it.
/// iceoryx2 v0.10.0's open() deletes the registered schema on a mismatch, so compare first.
std::expected<std::string, std::string> readRegisteredSchema(iox2::Config& config,
                                                             const std::string& serviceHash);

}  // namespace foxglove_bridge_iox2_plugin
