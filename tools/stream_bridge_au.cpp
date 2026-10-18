// Offline Annex-B adapter exerciser. This executable does not attach to Steam,
// negotiate a peer, select a native frame or enable passthrough.
#include "bridge/hevc_bridge_sei.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::vector<std::uint8_t> read(const char* path, std::size_t maximum) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open input");
    const auto end = file.tellg();
    if (end < 0 || static_cast<unsigned long long>(end) > maximum)
        throw std::runtime_error("Input exceeds the explicit limit");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    file.seekg(0);
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()),
                                    static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Cannot read input");
    return bytes;
}
void writeNew(const char* path, const std::vector<std::uint8_t>& bytes) {
    // The harness supplies a private new output path; never overwrite an input.
    std::ifstream existing(path, std::ios::binary);
    if (existing.good()) throw std::runtime_error("Output already exists");
    std::ofstream file(path, std::ios::binary);
    if (!file || !file.write(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Cannot write output");
}
}

int main(int argc, char** argv) {
    try {
        const bool append = argc == 5 && std::string(argv[1]) == "append";
        const bool inspect = argc == 4 && std::string(argv[1]) == "inspect";
        const bool remove = argc == 4 && std::string(argv[1]) == "remove";
        if (!append && !inspect && !remove) {
            std::cerr << "Offline only: stream_bridge_au append AU payload new-AU | "
                         "inspect AU new-payload | remove AU new-AU\n";
            return 2;
        }
        auto input = read(argv[2], stream_bridge::MaximumAccessUnitBytes);
        std::vector<std::uint8_t> output;
        const stream_bridge::Bytes view{input.data(), input.size()};
        stream_bridge::SeiStatus result;
        if (append) {
            auto payload = read(argv[3], stream_bridge::MaximumOwnedPayloadBytes);
            result = stream_bridge::appendOwned(view, {payload.data(), payload.size()}, output);
        } else if (inspect) result = stream_bridge::inspectOwned(view, output);
        else result = stream_bridge::removeOwned(view, output);
        if (result != stream_bridge::SeiStatus::Ok) {
            std::cerr << "Owned-SEI operation refused: " << static_cast<int>(result) << '\n';
            return 3;
        }
        writeNew(argv[append ? 4 : 3], output);
        std::cout << "OFFLINE_ONLY bytes=" << output.size()
                  << " SteamLinkObserved=0 MutualHandshake=0 PassthroughEnabled=0\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
