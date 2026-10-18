#pragma once
#include <cstddef>

// Runtime pins for the locally generated, owned fragments. These identify
// private installation outputs, not distributable Valve shader binaries.
// Generator/installer parity and real cold-loader tests cover both entries.
namespace stream_bridge_artifacts {
inline constexpr std::size_t CoverageBytes=7188;
inline constexpr const char* CoverageSha256="316ceb1cc3c50a6eef9c6e176d1001d1afd61e0747b628d87728439c3d2bfc6e";
inline constexpr std::size_t LegacyBytes=31384;
inline constexpr const char* LegacySha256="44c4a6c6bf7543b53d14ab6d350d8498143d6c03f70bfb45b9c6812e6b900a49";
}
