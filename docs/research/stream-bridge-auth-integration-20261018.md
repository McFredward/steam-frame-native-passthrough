# Authenticated stream bridge integration fixture (2026-10-18)

This is a local software integration check, not live Steam Link, TCP, native
NVENC, decoder, GPU, mask-transfer or headset evidence. Its deterministic
pairing secret and explicitly supplied source/session facts are **fixtures**.
No runtime provider is authorized by this test and no image route is enabled.

`tests/test_stream_bridge_authenticated.cpp` links the actual implementations:

```text
AuthSession(local signing authority)
  -> SenderAdapter(optional PayloadSealer)
  -> authenticated 176-byte envelope
  -> actual appendOwned HEVC SEI in the caller's complete AU
  -> copied original augmented AU
  -> Receiver.observeAccessUnit (one inspectOwned)
  -> PayloadVerifier (one bootstrapFromVideo or openControl)
  -> actual deferred receiver Handshake.begin/receive
```

The reverse HELLO, ACK, COMMIT and probe echo use the actual HMAC implementation
and are verified before reaching the producer's handshake/probe state. Signing
is not treated as a successful send: sender completion and reverse handoff are
acknowledged separately. The initial video HELLO is authenticated exactly once;
the fixture never authenticates it externally and then reconsumes the same
envelope, never fabricates a raw 80-byte replacement AU, and never marks a local
signing capability as an observed peer.

The built-in AU is a small structural Annex-B fixture. An optional command-line
path supplies a complete independently encoded HEVC AU. The focused evidence
run additionally uses a one-frame `ffmpeg/libx265` AU with real parameter sets,
foreign user-data SEI and VCL. Removing the owned SEI must recover **every
original byte**, including original headers and foreign SEI. Decoder preservation
is covered separately by the existing real software-HEVC codec fixture; this
integration test does not replace that evidence.

The checks cover:

- Exact unchanged receiver AU pointer, length and all bytes on every success
  and refusal, with no receiver stripping or metadata-to-AU re-encoding.
- Missing peer, unavailable auth sealer and legacy raw-80 discovery retain
  ordinary video and cannot establish a protected route. Missing/incompatible
  peer notices remain useful and once per session.
- Wrong pair ID, wrong secret, bad MAC, repeated signed AU, another native
  stream/generation, incomplete AU and unproved source boundary never bootstrap
  the receiver or consume a valid future packet's replay position.
- Paired authentication is separate from protocol compatibility. Even a valid
  MAC cannot override unmet required capabilities.
- Both actual signed HELLO/ACK/COMMIT directions are required. Mutual completion
  has no mask, selected-frame or rendering permission.
- Probe echo is possible only after that signed probe is observed inside the
  actual supplied coded AU. Preparation/submission, mutual handshake, or an
  unauthenticated echo cannot substitute for that observation.
- Actual decoder and selected-surface correspondence are separate explicit
  caller inputs. Native key, frame generation, surface handle/content generation
  and stream generation must match exactly; reused surface contents invalidate
  the old tuple. No latest-picture fallback is allowed.
- A changed sender codec/encoder generation revokes the previous source;
  original AU bytes remain intact. Explicit transport loss resets all owners
  immediately without a timer or restart.

Build the focused fixture with C++17 and the repository root include directory,
linking `stream_bridge_auth.cpp`, `stream_bridge_protocol.cpp`,
`hevc_bridge_sei.cpp`, `stream-bridge-sender.cpp` and
`stream_bridge_receiver.cpp`. Run with AddressSanitizer/UndefinedBehaviorSanitizer
for the same paths. Targeted semantic mutants remove the sealer, MAC verifier,
selected native generation check, source epoch check and local COMMIT gate; each
must fail its corresponding production-path assertion.

Remaining runtime requirements are unchanged: a real authenticated control
transport; private credential loading; actual admitted source/AU ownership;
observed Steam Link SEI survival; exact native selected-surface/import/eye/UV
association; and a complete independent coverage route. A passing fixture proves
the composed portable contract, not these missing native facts or picture quality.
