#include "foxglove_bridge_iox2_plugin/plugin_support/problem_log.hpp"

#include <format>

#include <rclcpp/logging.hpp>

namespace foxglove_bridge_iox2_plugin {

void ProblemLog::report(const rclcpp::Logger& logger, std::string_view source,
                        const Problem& problem,
                        std::chrono::steady_clock::duration repeatInterval) {
  const auto now = std::chrono::steady_clock::now();
  if (problem.message == _lastMessage &&
      (problem.severity == Problem::Severity::Debug || now - _lastTime < repeatInterval)) {
    return;
  }
  _lastMessage = problem.message;
  _lastTime = now;
  const auto text = std::format("Not bridging {}: {}", source, problem.message);
  switch (problem.severity) {
    case Problem::Severity::Debug:
      RCLCPP_DEBUG(logger, "%s", text.c_str());
      break;
    case Problem::Severity::Warn:
      RCLCPP_WARN(logger, "%s", text.c_str());
      break;
    case Problem::Severity::Error:
      RCLCPP_ERROR(logger, "%s", text.c_str());
      break;
  }
}

void ProblemLog::clear() {
  _lastMessage.clear();
}

}  // namespace foxglove_bridge_iox2_plugin
