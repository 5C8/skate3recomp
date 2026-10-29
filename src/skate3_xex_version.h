#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>

namespace skate3::xex {

inline constexpr uint32_t kTitleId = 0x454108E6;
inline constexpr uint32_t kBaseVersion = 0x00000003;  // 3.0.0.0
inline constexpr uint32_t kTitleUpdate3Version = 0x00000303;  // 3.0.3.0

struct ExecutionInfo {
  uint32_t version;
  uint32_t title_id;
};

// Optional-header offsets are file offsets, not guest addresses. Read only the
// fixed fields needed here and bound every offset before following it.
inline std::optional<ExecutionInfo> ReadExecutionInfo(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return std::nullopt;
  const auto length = file.tellg();
  if (length < 24) return std::nullopt;
  auto read_u32 = [&](uint64_t offset) -> std::optional<uint32_t> {
    if (offset > static_cast<uint64_t>(length) - 4) return std::nullopt;
    uint8_t bytes[4];
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    if (!file) return std::nullopt;
    return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 |
           uint32_t(bytes[2]) << 8 | uint32_t(bytes[3]);
  };
  if (read_u32(0) != 0x58455832u) return std::nullopt;  // XEX2
  const auto count = read_u32(20);
  if (!count || *count > (static_cast<uint64_t>(length) - 24) / 8) return std::nullopt;
  for (uint32_t i = 0; i < *count; ++i) {
    const uint64_t entry = 24 + uint64_t(i) * 8;
    if (read_u32(entry) != 0x00040006u) continue;
    const auto offset = read_u32(entry + 4);
    if (!offset) return std::nullopt;
    const auto version = read_u32(uint64_t(*offset) + 4);
    const auto title = read_u32(uint64_t(*offset) + 12);
    if (!version || !title) return std::nullopt;
    return ExecutionInfo{*version, *title};
  }
  return std::nullopt;
}

inline bool HasTitleUpdate3(const std::filesystem::path& path) {
  const auto info = ReadExecutionInfo(path);
  return info && info->title_id == kTitleId && info->version == kTitleUpdate3Version;
}

}  // namespace skate3::xex
