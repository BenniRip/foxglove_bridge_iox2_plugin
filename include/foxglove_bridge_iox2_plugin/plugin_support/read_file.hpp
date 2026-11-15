#pragma once

#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>

namespace foxglove_bridge_iox2_plugin {

inline std::expected<std::string, std::string> readFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::unexpected(std::format("cannot read '{}'", path.string()));
  }
  return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

}  // namespace foxglove_bridge_iox2_plugin
