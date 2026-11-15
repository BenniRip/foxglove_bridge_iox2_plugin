#include "foxglove_bridge_iox2_plugin/iox2_service_storage.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <string_view>
#include <system_error>

#include "foxglove_bridge_iox2_plugin/plugin_support/read_file.hpp"

namespace foxglove_bridge_iox2_plugin {
namespace {

std::optional<uint64_t> parseTomlUnsigned(std::string_view section, std::string_view key) {
  const auto prefix = std::format("{} = ", key);
  for (const auto line : std::views::split(section, '\n')) {
    const std::string_view text(line.begin(), line.end());
    if (!text.starts_with(prefix)) {
      continue;
    }
    const auto digits = text.substr(prefix.size());
    uint64_t value = 0;
    if (std::from_chars(digits.data(), digits.data() + digits.size(), value).ec != std::errc{}) {
      return std::nullopt;
    }
    return value;
  }
  return std::nullopt;
}

std::string toDecimal(uint64_t high, uint64_t low) {
  __extension__ using Uint128 = unsigned __int128;
  return std::format("{}", (static_cast<Uint128>(high) << 64) | low);
}

}  // namespace

std::expected<std::string, std::string> readRegisteredSchema(iox2::Config& config,
                                                             const std::string& serviceHash) {
  const std::string prefix = config.global().prefix();
  const std::filesystem::path servicesDir =
    std::filesystem::path(config.global().root_path()) / config.global().service().directory();
  const std::string staticConfigSuffix = config.global().service().static_config_storage_suffix();

  // The unique service ID in the static config names the resource directory.
  const auto staticConfigPath = servicesDir / (prefix + serviceHash + staticConfigSuffix);
  const auto staticConfig = readFile(staticConfigPath);
  if (!staticConfig) {
    return std::unexpected(staticConfig.error());
  }
  const auto sectionStart = staticConfig->find("[unique_service_id]");
  if (sectionStart == std::string::npos) {
    return std::unexpected(
      std::format("no [unique_service_id] in '{}'", staticConfigPath.string()));
  }
  const auto sectionEnd = staticConfig->find("\n[", sectionStart + 1);
  const auto section =
    std::string_view(*staticConfig).substr(sectionStart, sectionEnd - sectionStart);
  const auto payloadValue = parseTomlUnsigned(section, "payload_value");
  const auto uniqueValue = parseTomlUnsigned(section, "unique_value");
  if (!payloadValue || !uniqueValue) {
    return std::unexpected(
      std::format("unexpected [unique_service_id] in '{}'", staticConfigPath.string()));
  }

  const auto resourceDir = servicesDir / toDecimal(*uniqueValue, *payloadValue);
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(resourceDir, error)) {
    if (entry.path().filename().string().starts_with(prefix + "payload")) {
      return readFile(entry.path());
    }
  }
  return std::unexpected(std::format("no payload schema in '{}'", resourceDir.string()));
}

}  // namespace foxglove_bridge_iox2_plugin
