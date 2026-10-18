#pragma once
#include "stream_bridge_auth.h"
#include <filesystem>
namespace stream_bridge {
enum class CredentialStatus { Ok, MissingSelection, MissingCredential, InsecureFile,
    MalformedSelection, MalformedCredential, RoleMismatch, UnsupportedProtocol, IoError };
// Cold worker only. Exactly active.json and that selected ID are read. No scan,
// repairs, environment changes, logging, writes or exception text is emitted.
// Output is transactional; caller owns/erases the returned secret after use.
CredentialStatus loadPrivatePairing(const std::filesystem::path& pairingsDirectory,
    Role expected,Pairing& output) noexcept;
const char* credentialStatusName(CredentialStatus) noexcept;
// Cold explicit preparation only. OS CSPRNG, at most 64 bytes, no weak fallback.
bool bridgeSystemRandom(void*,std::size_t) noexcept;
} // namespace stream_bridge
