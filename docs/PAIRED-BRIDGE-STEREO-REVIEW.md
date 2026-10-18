# Paired preview stereo regression

The first combined preview headset run reported duplicate, distorted pictures
and a black area where passthrough was expected. Turning native passthrough off
restored the normal picture. This is hardware failure evidence, even though the
installer and isolated software checks passed.

## Evidence and limits

The read-only Frame snapshot on 2026-10-18 confirms both installed native
components match the published preview and are loaded in the intended
compositor/Steam Link processes. Both services are running. Steam Link reports
2.18.2 and a decoder change from 1152x4608 to 3264x6400 at 20:42:59. That extent
is consistent with the previous atlas route, but is not by itself proof of an
application source mode. The PC game logs available at collection were older
Build707 logs; they must not be treated as this run's sender evidence.

A previous Stream Link process crashed while exiting at 20:33:07. The current
stream uses a later process. The crash stack is retained privately; an allocator
frame alone does not establish its cause or explain the current stereo error.
Collection changed no Frame files, services, shaders or running stream state.

## Source-proven failures

1. The production receiver retained the real native set0/binding9 count1 layout
   as pending, but never supplied the approved original-fragment constant-zero
   interface proof. Pipeline admission consequently rejected the actual native
   interface before creating either the legacy decoder or the new coverage
   companion. The original compositor then had no atlas decoder. The previous
   implementation explicitly supplied this proof. The coverage companion also
   needed the same original-only count1 specialization.
2. A draw-time companion alone is not a stable legacy baseline: failed
   observation, an epoch change or a lock miss can otherwise expose the original
   shader to atlas content. Supported legacy decoding must remain the baseline
   independently of optional exact coverage readiness.
3. The sender compared COM object identity between an independently opened
   submitted texture and the resource of the native bound SRV. Separate opens
   can have different objects while referencing the same allocation. Admission
   now requires the explicit shared handle on both resources, the current
   submission generation/PID and the actual texture shape. Unrelated COM
   identity checks remain unchanged. Microsoft documents
   [separate resource objects](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11device1-opensharedresource1)
   and the [legacy shared handle/lifetime contract](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiresource-getsharedhandle).
4. The source-format grant was coupled to transient encoder/mask readiness.
   Releasing the old uniform stream lease can reconfigure the encoder, revoke
   that readiness and make the game request the old format again. Source-format
   permission now belongs to the admitted local pipe connection after its first
   compatible/prepared grant. Reconfiguration does not revoke this format choice;
   disconnect/cancel does. No image or transparency authority is retained.
   Successful typed HEVC reconfiguration also readmits the current native source
   with a fresh generation and resource/map identities. Original callbacks may
   unregister resources; their changes are never overwritten by a saved snapshot.

These form a consistent failure chain: no admitted receiver image route, no
complete new-route negotiation, and no reliable decoding of the retained legacy
source. The source failures and reported symptom agree; a corrected headset
picture still requires a new hardware result.

## Why the earlier checks missed this

Installer tests checked ownership, transactions, startup and process recovery.
They did not prove a stereo image. Earlier graphics fixtures did not exercise
the production renderer's pending native count1 layout admission. Earlier sender
fixtures reused a resource wrapper and therefore missed allocation sharing
through separate COM objects. Directly testing a shader or the handshake cannot
substitute for this complete interface/ownership chain.

The added regressions use the real production renderer with private pinned native
shader inputs and the documented count1 layout. Removing its required proof must
make the regression fail. Both supported descriptor variants are independently
validated on Vulkan for unchanged RGB and correct alpha. The sender fixture
separates simulated metadata checks from actual Windows D3D11 shared-resource
opens; unavailable sharing returns77 and is never reported as GPU proof.

## Updating and recovery

The assistant offers Update for a different checked bundle. Per-operation
checkpoints preserve the currently installed version during recovery, while the
installation receipt retains the initial uninstall baseline. Pairings are kept;
unknown changed files are preserved. Frame files are prepared live, followed by
its coordinated restart/health check. PC SteamVR stays closed after successful
installation/update; a rollback alone restores its original on/off state.

Detailed capture records, source pins, causal controls and actual package receipts
remain in the maintainer's private `steamlink-stream715` evidence directory.
Do not infer a successful combined headset result from these software checks.
