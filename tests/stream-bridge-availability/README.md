# Exact-picture availability evidence

This focused fixture investigates publication ordering. It does not certify a
headset image, GPU completion, native decoder speed or the contents of alpha.

`channel_timing.cpp` uses the actual authenticated Unix channel, SCM_RIGHTS and
shared capture lease. Certificate and decoded-picture packets are queued
together. One channel `tick()` exposes only the first packet; calling `receive()`
twice does not advance the second packet. A worker must explicitly tick/drain
again rather than sleeping between those two related packets.

`renderer_timing.cpp` executes the actual Renderer and mask pool through the
existing public Vulkan fixture. It prewarms finite GPU storage and delivers the
certificate **before** decoded CAPTURE in both controls. With LocalPicture
published before native recording, all 16 first presentations select coverage.
With LocalPicture delayed by one worker phase, all 16 first presentations remain
ordinary even though the message arrives before submit. Re-recording the exact
same picture then works. This is an intentional **requirement RED**, not a green
test accepting opaque active frames.

Run `run.py` with the existing explicit private vertex, fragment and coverage
inputs, as for the renderer fixture. It retains logs/pins and removes its owned
temporary executables. No private shader bytes are copied into this repository.

The production handoff repair must prevent that late publication ordering from
reaching native code. Successful exact CAPTURE DQBUF can publish internally and
then wait on its decoder thread, **after releasing the provider mutex**, for
the exact downstream picture/mask acknowledgement. Waiting for the native
compositor's later image import would create a dependency cycle. Ordinary
unpaired paths bypass this gate; authenticated None frames need no GPU mask.
An emergency deadline is a reported refusal, never permission to use a stale
or most-recent mask. The existing native-decode `--handoff` subset separately
proves safe callback placement, original-call/errno preservation and concurrent
requeue/reset invalidation; its state-lock mutation must fail that proof.
