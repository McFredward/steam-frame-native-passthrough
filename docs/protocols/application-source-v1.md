# Application source API v1

This is the current **Windows application → local sender companion** contract.
It declares an application's output format. It does not carry pixels, grant
camera access or prove that a selected native video frame contains coverage.
See [`stream-bridge-sender-provider-sourceipc.h/.cpp`](../../pc/stream-bridge-sender-provider-sourceipc.h)
and [`SourceContract`](../../pc/stream-bridge-source-contract.h).

## Image and lifecycle

The application supplies its complete ordinary per-eye image: contribution RGB
`E` and independent coverage alpha `A`, with the same eye, extent, source epoch
and native submission. Composition is `E + (1 − A) × camera`. Preserve positive
RGB at zero alpha for additive emission; do not multiply contribution by alpha
again. The current profile is Gamma SDR UNorm and MultiPass. No atlas, second
scene camera, CPU pixel export or changed resolution is required by this API.

Open the sender's local named pipe:
`\\.\pipe\SFNP.StreamBridge.V1.<ownerPID>`. Discover the actual SteamVR host
process from its canonical runtime, verify the pipe server's kernel PID against
that process, and refuse an unverified endpoint. The provider independently
checks the pipe client's kernel PID against the declared game PID. The pipe
rejects remote clients. Its per-user access control is not a substitute for
these identity checks.

Keep this one connection alive while the declaration is wanted. EOF, an invalid
record, explicit disablement or process loss revokes permission. Healthy
connections have no feature-expiry timer. Closing/reopening the pipe requires a
fresh nonce and validation; do not run overlapping source observers. Apply a
changed grant at a complete current stereo boundary and revalidate at the real
render seam. Without permission, retain ordinary opaque output. Native head or
source replacement must not borrow an old eye or mask.

## SBN1 record

Requests and replies are exactly **128 bytes**, unsigned little-endian fields,
without native struct padding. Reserved bytes must be zero.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `SBN1` |
| 4 | 2 | Kind: request 1, reply 2 |
| 6 | 2 | Version 1 |
| 8 | 2 | Total length 128 |
| 10 | 2 | Reply flags; request is zero |
| 12 | 4 | Output profile 1 |
| 16 | 4 | Actual game process ID |
| 20 | 4 | Server process ID; request is zero |
| 24 | 16 | Fresh nonzero client nonce, echoed by reply |
| 40 | 16 | Provider binding; request is zero |
| 56 | 8 | Provider generation; request is zero |
| 64 | 64 | Reserved, zero |

Flag 1, `canAcceptOrdinaryRgba`, grants only the supported output format. A
positive reply also requires its nonzero binding and generation. Flag 2,
`coverageOperational`, is reserved by the application parser; the current
provider deliberately never emits it. A format grant cannot claim that a
particular frame has already passed encoder, decoder and compositor proof.
Do not wait for a first mask before allowing the game to produce its first
RGBA frame: that would create a circular dependency.

The provider latches the validated live connection's format permission across
encoder/pool reconfiguration. Every actual video frame still needs independent
source/texture/encoder identity and an authenticated
[coverage certificate](stream-coverage-v1.md). A cached reply, headset name,
saved preference or successful handshake is insufficient.

## Example and validation

[GloomhavenVR's integration](https://github.com/McFredward/GloomhavenVR/blob/dev/docs/STEAM-LINK-COVERAGE-SOURCE.md)
shows the client, stereo cohorts, material coverage and render callback checks.
Its retry policy is application behavior, not a wire-protocol timer. It is an
example rather than an additional runtime dependency for other games.

Test both eyes, fractional alpha, additive materials, loading, native head and
render-target replacement, menu transitions, toggles, reconnect and teardown.
Test server/client PID mismatch, malformed/partial replies and EOF independently.
Portable record tests do not establish Windows named-pipe behavior or a correct
headset image. Proton does not make Linux SteamVR a supported Windows provider;
a Linux host requires its own native backend and source-identity contract.
