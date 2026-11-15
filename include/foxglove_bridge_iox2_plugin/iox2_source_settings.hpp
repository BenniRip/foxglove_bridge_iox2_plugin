#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include <iox2/bb/file_path.hpp>
#include <iox2/service_name.hpp>

#include "foxglove_bridge_iox2_plugin/plugin_support/instance_parameters.hpp"

namespace foxglove_bridge_iox2_plugin {

inline constexpr std::string_view PARAM_CONFIG_FILE = "config_file";
inline constexpr std::string_view PARAM_SCHEMA_DIR = "schema_dir";
inline constexpr std::string_view PARAM_SERVICE_NAMES = "service_names";
inline constexpr std::string_view PARAM_TOPICS = "topics";
inline constexpr std::string_view PARAM_SCHEMA_NAMES = "schema_names";
inline constexpr std::string_view PARAM_BFBS_FILES = "bfbs_files";
inline constexpr std::string_view PARAM_POLL_INTERVAL_MS = "poll_interval_ms";
inline constexpr std::string_view PARAM_ALLOW_BLOCKING_PUBLISHERS = "allow_blocking_publishers";
inline constexpr std::string_view PARAM_PROBLEM_REPEAT_INTERVAL_S = "problem_repeat_interval_s";

struct ServiceConfig {
  std::string serviceName;
  iox2::ServiceName iox2ServiceName;
  std::string topic;
  std::string schemaName;
  std::string bfbsPath;
  iox2::bb::FilePath iox2BfbsPath;
  std::string bfbs;
};

struct Settings {
  std::string configFile{};
  std::chrono::milliseconds pollInterval{};
  bool allowBlockingPublishers{};
  std::chrono::seconds problemRepeatInterval{};
  std::vector<ServiceConfig> services{};
};

/// Declares and validates the instance's parameters and reads the schema files. Throws on failure.
Settings loadSettings(InstanceParameters& parameters);

}  // namespace foxglove_bridge_iox2_plugin
