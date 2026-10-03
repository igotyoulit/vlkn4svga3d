# Validation

## Renderer and scanout changes — 2026-10-03

`make -j4 all`, all 12 harness suites, and `make -j4 test` passed on the final source changes. The harness used `/usr/share/vulkan/icd.d/lvp_icd.json` and SPIRV-Tools v2025.1. The no-ICD translator tests ran `spirv-val` successfully rather than skipping it. Rendering tests check Vulkan validation through resource and device teardown.

Added regressions cover alpha comparisons and discarded-fragment depth/stencil behavior, buffer overwrite ordering and retirement accounting, cached framebuffer invalidation, and screen backing-store offsets, bounds, padded pitch, define/destroy packets, and display notifications.

Earlier live VM119 checks completed three glmark2 runs with scores 545, 533, and 544. Checkable glmark2 scenes reported validation success; scenes reporting `Unknown` are not correctness passes. In the matched Minecraft 1.8.9 scene, five F3 readings averaged 37.6 FPS for the current renderer versus 31.4 for the saved old binary. The old binary also showed rendering corruption, its source identity is unknown, and the comparison was not repeated. These readings do not establish equal rendering work or isolate optimization effects.

Fabulously Optimized 8.0.1 / Minecraft 1.21.4 failed to create a GLX core context on the guest SVGA driver, which reports OpenGL 2.1. It subsequently launched using guest CPU rendering (`llvmpipe`) via an instance-only Prism wrapper. That workaround is separate from these source changes and does not establish accelerated modern-Minecraft support.

Local build/test evidence is in `artifacts/pr-merge-20261003/` and `.harness-loop/iter-01/`; live evidence is in `artifacts/glmark2-ab-20261003/` and `artifacts/minecraft-ab-20261003/`. These directories are excluded from Git.

## Publication checks — 2026-09-25

A separate source snapshot was built without using the development directory's existing object files.

| Check | Result |
| --- | --- |
| `make -j4 all` | Passed; compiler warnings remain |
| `make test` | Passed, including 3,407 mock-engine assertions |
| `make acceptance` | Passed all stages of the existing runner |

The real-Vulkan driver check identified Mesa llvmpipe (LLVM 20.1.2, 256 bits), Vulkan API 1.4.318, software rendering, mock fallback disabled, and validation layers enabled. That check reported zero validation errors and warnings.

These results describe the current tests, not an independent certification of their coverage. The QEMU integration executable is an in-process device harness, not a booted guest. No real VM was booted or modified during publication. The project owner reported entering a Minecraft world on September 24. Recorded screenshots inspected afterward show terrain and a textured hand. A live September 25 check identified the Intel HD Graphics 630 Vulkan driver and nonzero GPU engine activity. Full visual correctness and cold-boot reliability remain unverified. The fresh test runs above use local software Vulkan, not the live guest. Hardware-GPU performance and broad guest compatibility remain unverified.

Build and test transcripts from this snapshot are in `validation/`.
