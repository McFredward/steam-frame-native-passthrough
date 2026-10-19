#pragma once
#include <cstddef>

// Runtime pins for the locally generated, owned fragments. These identify
// private installation outputs, not distributable Valve shader binaries.
// Generator/installer parity and real cold-loader tests cover the sole active coverage artifact.
namespace stream_bridge_artifacts {
inline constexpr std::size_t CoverageBytes=7188;
inline constexpr const char* CoverageSha256="316ceb1cc3c50a6eef9c6e176d1001d1afd61e0747b628d87728439c3d2bfc6e";
}
