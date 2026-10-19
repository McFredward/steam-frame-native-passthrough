# Live application format permission instead of alpha sampling

The paired 09:41 hardware run proves startup recovery now completes repeatedly,
and Game Build710 obtains ordinary-RGBA permission. The sender still reports
only8 mask-bearing AUs out of15257 matched inputs, with14661
`source-alpha-witness` refusals. The old gate conflated incomplete nonblocking
witness readback with opaque sampled pixels; those logs cannot distinguish them.
Several exact decoded image leases reached the Frame compositor. A successful
handshake or delivered mask alone does not establish a displayed MR picture.

## Invalid format heuristic

The tiny5x5 per-eye/layer sampling gate existed to distinguish the retired atlas
from an ordinary-RGBA source. It demanded at least one translucent sampled texel
in EVERY submitted eye/layer. An explicitly supported ordinary source can be
fully opaque, have a translucent area between the sample points, or contain an
opaque overlay. None of those properties invalidate its format. Maintaining the
sampling path also queued50 tiny GPU copies per stereo layer and extra staging
readbacks in addition to the actual full coverage receipt.

The legacy source is no longer supported. Format semantics now require the live
application's existing explicit SBN1 ordinary-RGBA request, validated against the
named pipe's kernel client PID and the actual prepared DirectMode owner. No
pixel or RGB value authenticates this contract. Game710 already implements it;
no game-side source, resolution or wire-protocol change is needed.

## Revocation and exact picture proof

SourceIpcPermission grants only source-format permission after native preparation
and mutually confirmed paired video/control proof. EOF, invalid input, cancel or
stop publishes0 before pending OS requests are reaped. A reconnect, including the
same PID with a new nonce, receives a new monotonic declaration epoch. Epoch
exhaustion refuses rather than wrapping. Only the cold provider publishes; native
callbacks read one lock-free packed PID/epoch identity.

The first actual native source draw freezes that declaration for the present.
Changing it between eyes/layers makes the present inadmissible. Native copy and
accepted-Encode/Lock association retain that exact declaration alongside the
existing immutable source/encoder/submission receipt. The current declaration is
checked before GPU readback, after RLE and after certification before publication.
Revocation during compression or MAC work discards the optional mask and retains
the original AU. Source ready/preparation deliberately does not depend on the new
permission; requiring it there would prevent the first permission from being
issued.

All existing peer, native source owner/generation, actual draw context/callsite,
complete eye/layer, native UV/geometry, encoder, GPU readiness, accepted-input,
exact AU, decoded-image and GPU-lease requirements remain. Format permission is
not itself selected-image authority. Native RGB, resolution, foveation and
reprojection are unchanged. Fully opaque coverage remains a legitimate image.

## Focused evidence

Portable tests exercise the production source permission and declaration epoch,
including wrong PID, missing native preparation/peer, live renegotiation, EOF,
same-PID reconnect and exhaustion. Three causal controls omit PID, epoch or
revocation protection and fail. The actual Windows named-pipe fixture uses
GetNamedPipeClientProcessId, asynchronous request/reply, EOF and reconnect to
check the real server callback. The separate real D3D11 R8 fixture retains alpha0,
fractional and alpha1 native-style draws, original target/shader preservation,
stereo viewports and multilayer transmittance.

These are software boundary checks. The default private pinned native conversion
shader is reviewed separately without redistributing its body. Hardware results
for final full-resolution output and simultaneous game/menu MR remain unverified.
