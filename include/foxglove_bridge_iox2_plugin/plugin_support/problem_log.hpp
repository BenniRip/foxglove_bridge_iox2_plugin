#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include <rclcpp/logger.hpp>

namespace foxglove_bridge_iox2_plugin {

/// Why a source cannot be bridged right now.
struct Problem {
  enum class Severity { Debug, Warn, Error };

  Severity severity;
  std::string message;
};

/// Logs a source's problems: new ones at once, unchanged warnings and errors every repeat
/// interval, unchanged debug messages never.
class ProblemLog {
public:
  void report(const rclcpp::Logger& logger, std::string_view source, const Problem& problem,
              std::chrono::steady_clock::duration repeatInterval);

  /// So a problem that comes back is logged at once.
  void clear();

private:
  std::string _lastMessage;
  std::chrono::steady_clock::time_point _lastTime;
};

}  // namespace foxglove_bridge_iox2_plugin
