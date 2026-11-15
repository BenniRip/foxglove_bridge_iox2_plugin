#include "foxglove_bridge_iox2_plugin/iox2_source_settings.hpp"

#include <filesystem>
#include <format>
#include <ranges>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "foxglove_bridge_iox2_plugin/plugin_support/read_file.hpp"

namespace foxglove_bridge_iox2_plugin {
namespace {

constexpr int64_t DEFAULT_POLL_INTERVAL_MS = 10;
constexpr int64_t DEFAULT_PROBLEM_REPEAT_INTERVAL_S = 30;

}  // namespace

Settings loadSettings(InstanceParameters& parameters) {
  using Strings = std::vector<std::string>;

  const auto configFile = parameters.declare(PARAM_CONFIG_FILE, std::string(),
                                             "iceoryx2 config file (TOML) shared with the "
                                             "publishers. Empty uses iceoryx2's default "
                                             "configuration.");
  const std::filesystem::path schemaDir =
    parameters.declare(PARAM_SCHEMA_DIR, std::string(),
                       "Directory containing the .bfbs schema files listed in bfbs_files.");
  const auto serviceNames =
    parameters.declare(PARAM_SERVICE_NAMES, Strings(),
                       "iceoryx2 publish-subscribe services with iox2::Flatbuffer payloads.");
  const auto topics =
    parameters.declare(PARAM_TOPICS, Strings(), "Foxglove topic for each entry of service_names.");
  const auto schemaNames = parameters.declare(
    PARAM_SCHEMA_NAMES, Strings(),
    "Fully qualified FlatBuffer root table name for each entry of service_names, e.g. "
    "'foxglove.PointCloud'.");
  const auto bfbsFiles = parameters.declare(
    PARAM_BFBS_FILES, Strings(),
    "Binary FlatBuffer schema (.bfbs) for each entry of service_names, relative to schema_dir. "
    "Must be byte-identical to the schema the publisher registered with iceoryx2.");
  const auto pollIntervalMs = parameters.declare(
    PARAM_POLL_INTERVAL_MS, DEFAULT_POLL_INTERVAL_MS,
    "Interval in milliseconds at which subscribed services are drained.", 1, 1000);
  const auto allowBlockingPublishers = parameters.declare(
    PARAM_ALLOW_BLOCKING_PUBLISHERS, false,
    "Also bridge services with safe overflow disabled. Their publishers may block when the "
    "bridge falls behind, so this is off by default.");
  const auto problemRepeatIntervalS = parameters.declare(
    PARAM_PROBLEM_REPEAT_INTERVAL_S, DEFAULT_PROBLEM_REPEAT_INTERVAL_S,
    "Interval in seconds at which a warning or error about a service that cannot be bridged is "
    "repeated while it persists, so it also reaches clients that connect later.",
    1, 3600);

  if (topics.size() != serviceNames.size() || schemaNames.size() != serviceNames.size() ||
      bfbsFiles.size() != serviceNames.size()) {
    throw std::invalid_argument(
      std::format("{}, {}, {} and {} must have the same length",
                  parameters.name(PARAM_SERVICE_NAMES), parameters.name(PARAM_TOPICS),
                  parameters.name(PARAM_SCHEMA_NAMES), parameters.name(PARAM_BFBS_FILES)));
  }

  Settings settings{.configFile = configFile,
                    .pollInterval = std::chrono::milliseconds(pollIntervalMs),
                    .allowBlockingPublishers = allowBlockingPublishers,
                    .problemRepeatInterval = std::chrono::seconds(problemRepeatIntervalS)};
  std::unordered_set<std::string> seenTopics;
  for (const auto& [serviceName, topic, schemaName, bfbsFile] :
       std::views::zip(serviceNames, topics, schemaNames, bfbsFiles)) {
    auto iox2ServiceName = iox2::ServiceName::create(serviceName.c_str());
    if (!iox2ServiceName.has_value()) {
      throw std::invalid_argument(std::format("invalid iceoryx2 service name '{}'", serviceName));
    }
    if (topic.empty() || !seenTopics.insert(topic).second) {
      throw std::invalid_argument(std::format("topic '{}' is empty or used more than once", topic));
    }
    const auto bfbsPath = (schemaDir / bfbsFile).string();
    auto iox2BfbsPath = iox2::bb::FilePath::create(bfbsPath.c_str());
    if (!iox2BfbsPath.has_value()) {
      throw std::invalid_argument(std::format("invalid schema file path '{}'", bfbsPath));
    }
    auto bfbs = readFile(bfbsPath);
    if (!bfbs) {
      throw std::runtime_error(bfbs.error());
    }
    settings.services.push_back({.serviceName = serviceName,
                                 .iox2ServiceName = std::move(iox2ServiceName.value()),
                                 .topic = topic,
                                 .schemaName = schemaName,
                                 .bfbsPath = bfbsPath,
                                 .iox2BfbsPath = std::move(iox2BfbsPath.value()),
                                 .bfbs = std::move(*bfbs)});
  }
  return settings;
}

}  // namespace foxglove_bridge_iox2_plugin
