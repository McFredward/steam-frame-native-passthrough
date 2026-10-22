#pragma once
#include <cstddef>

// Runtime pins for the locally generated, owned fragments. These identify
// private installation outputs, not distributable Valve shader binaries.
// Generator/installer parity and real cold-loader tests cover the sole active coverage artifact.
namespace stream_bridge_artifacts {
inline constexpr std::size_t CoverageBytes=7420;
inline constexpr const char* CoverageSha256="37602d15087deefbe8acc8bd7f53fea80f8b740bb726a25b2fe160f3ab175db3";
inline constexpr std::size_t CameraOnlyBytes=7072;
inline constexpr const char* CameraOnlySha256="a8f7c2a375abb3f8d5188fdf129c00b2a2a1b13f3f3d8e7620b0a833510ea8ee";
}
