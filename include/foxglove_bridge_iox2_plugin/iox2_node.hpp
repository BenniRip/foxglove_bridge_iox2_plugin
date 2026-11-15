#pragma once

#include <string>

#include <iox2/config.hpp>
#include <iox2/node.hpp>

namespace foxglove_bridge_iox2_plugin {

using IpcNode = iox2::Node<iox2::ServiceType::Ipc>;

/// Loads an iceoryx2 config file (TOML), or iceoryx2's default config if configFile is empty.
iox2::Config loadIox2Config(const std::string& configFile);

/// Creates the bridge's iceoryx2 node. Leaves signal handling to rclcpp.
IpcNode createIox2Node(const iox2::Config& config);

}  // namespace foxglove_bridge_iox2_plugin
