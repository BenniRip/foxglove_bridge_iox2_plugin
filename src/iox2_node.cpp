#include "foxglove_bridge_iox2_plugin/iox2_node.hpp"

#include <format>
#include <stdexcept>
#include <utility>

#include <iox2/bb/file_path.hpp>
#include <iox2/enum_translation.hpp>
#include <iox2/node_name.hpp>
#include <iox2/signal_handling_mode.hpp>

namespace foxglove_bridge_iox2_plugin {

iox2::Config loadIox2Config(const std::string& configFile) {
  if (configFile.empty()) {
    return iox2::Config::global_config().to_owned();
  }
  auto configPath = iox2::bb::FilePath::create(configFile.c_str());
  if (!configPath.has_value()) {
    throw std::runtime_error(std::format("invalid iceoryx2 config file path '{}'", configFile));
  }
  auto config = iox2::Config::from_file(configPath.value());
  if (!config.has_value()) {
    throw std::runtime_error(std::format("failed to load iceoryx2 config '{}': {}", configFile,
                                         iox2::bb::into<const char*>(config.error())));
  }
  return std::move(config.value());
}

IpcNode createIox2Node(const iox2::Config& config) {
  auto nodeName = iox2::NodeName::create("foxglove_bridge");
  if (!nodeName.has_value()) {
    throw std::runtime_error("failed to create iceoryx2 node name");
  }
  auto node = iox2::NodeBuilder()
                .name(nodeName.value())
                .signal_handling_mode(iox2::SignalHandlingMode::Disabled)
                .config(config)
                .create<iox2::ServiceType::Ipc>();
  if (!node.has_value()) {
    throw std::runtime_error(
      std::format("failed to create iceoryx2 node: {}", iox2::bb::into<const char*>(node.error())));
  }
  return std::move(node.value());
}

}  // namespace foxglove_bridge_iox2_plugin
