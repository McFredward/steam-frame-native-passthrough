#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace stream_bridge_camera {
struct ClockSample {std::uint64_t bootNanoseconds=0,monotonicNanoseconds=0;};
using ClockReader=bool(*)(ClockSample&,void*) noexcept;
// A cold-worker-owned subprocess session. Only ready() may be called from a
// recording/submission thread; it performs a single atomic acquire, never I/O.
// No OpenVR library or initialization is performed in the native Valve process.
class Client {
public:
    // Optional pure clock port is for deterministic suspend/stall fixtures.
    // Shipping callers always use the actual Linux clocks.
    explicit Client(ClockReader=nullptr,void* clockContext=nullptr);~Client();
    Client(const Client&)=delete;Client& operator=(const Client&)=delete;
    // Cold serialized owner only. Revocation is published before any command.
    void request(bool enabled) noexcept;
    void tickCold(const std::filesystem::path& root,bool authenticatedPeer) noexcept;
    // Closing the owned channel makes the independent helper restore its key.
    // This never signals/restarts Steam, SteamVR, or another user's process.
    void stopCold() noexcept;
    bool ready()const noexcept;
    bool runningCold()const noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
