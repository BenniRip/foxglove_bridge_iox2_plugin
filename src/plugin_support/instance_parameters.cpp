#include "foxglove_bridge_iox2_plugin/plugin_support/instance_parameters.hpp"

#include <format>
#include <utility>

namespace foxglove_bridge_iox2_plugin {

InstanceParameters::InstanceParameters(rclcpp::Node& node, std::string instanceName)
    : _node(node)
    , _instanceName(std::move(instanceName)) {}

std::string InstanceParameters::name(std::string_view key) const {
  return std::format("{}.{}", _instanceName, key);
}

int64_t InstanceParameters::declare(std::string_view key, int64_t defaultValue,
                                    std::string_view description, int64_t min, int64_t max) {
  auto rangeDescriptor = descriptor(key, description);
  rangeDescriptor.integer_range.push_back(
    rcl_interfaces::msg::IntegerRange().set__from_value(min).set__to_value(max).set__step(1));
  return _node.declare_parameter(name(key), defaultValue, rangeDescriptor);
}

rcl_interfaces::msg::ParameterDescriptor InstanceParameters::descriptor(
  std::string_view key, std::string_view description) const {
  rcl_interfaces::msg::ParameterDescriptor result;
  result.name = name(key);
  result.description = description;
  result.read_only = true;
  return result;
}

}  // namespace foxglove_bridge_iox2_plugin
