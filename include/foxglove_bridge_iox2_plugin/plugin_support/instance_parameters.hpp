#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <rcl_interfaces/msg/parameter_descriptor.hpp>
#include <rclcpp/node.hpp>

namespace foxglove_bridge_iox2_plugin {

/// Declares read-only parameters named "<instance name>.<key>", so instances don't clash.
class InstanceParameters {
public:
  InstanceParameters(rclcpp::Node& node, std::string instanceName);

  [[nodiscard]] std::string name(std::string_view key) const;

  /// Declares a parameter and returns its value.
  template <typename T>
  T declare(std::string_view key, const T& defaultValue, std::string_view description) {
    return _node.declare_parameter(name(key), defaultValue, descriptor(key, description));
  }

  /// Limited to [min, max].
  int64_t declare(std::string_view key, int64_t defaultValue, std::string_view description,
                  int64_t min, int64_t max);

private:
  [[nodiscard]] rcl_interfaces::msg::ParameterDescriptor descriptor(
    std::string_view key, std::string_view description) const;

  rclcpp::Node& _node;
  std::string _instanceName;
};

}  // namespace foxglove_bridge_iox2_plugin
