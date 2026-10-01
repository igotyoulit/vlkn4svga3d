# vlkn4svga3d

An experimental SVGA3D-to-Vulkan rendering library and QEMU integration prototype. Internal APIs and build targets still use the original `svga3_vlkn` name.

**Work in progress — incomplete and not production-ready.** This is a source snapshot, not a finished virtual GPU or a drop-in replacement for VMware graphics. Shader, format, state, and guest compatibility are not comprehensively verified. Minecraft has reached an in-game world in the PlayBook guest. Recorded September 24 screenshots show terrain, the hotbar, and a textured player hand after integration fixes. A September 25 live check identified the Intel HD Graphics 630 Vulkan driver and measured GPU activity. This is a narrow compatibility milestone, not comprehensive visual validation or a cold-boot reliability guarantee. No general compatibility or performance guarantee is made.

## QEMU lab adapter

The preload adapter supports one allowlisted QEMU build and checks instruction bytes before patching. Use it only for the designated VM; never configure global `LD_PRELOAD`. The tested Proxmox VM runs QEMU as `qemu119` and guest graphics as `svga3d`.

Set `SVGA3_VLKN_VALIDATE=1` to request Vulkan validation layers; initialization fails if they cannot be activated. Portrait mode overrides require `SVGA3_VLKN_GUEST_PROFILE=playbook-portrait`; normal guests use their requested mode. Framebuffer GPA comes from the device register. Application-context centering is disabled by default; the old heuristic is available only with `SVGA3_VLKN_LEGACY_CLIENT_PRESENT=1`. Guest-RAM discovery still uses a lab-only mapping heuristic; official QEMU integration remains follow-up work.

## What is included

- SVGA3D FIFO decoding, surfaces, contexts, and Vulkan rendering code.
- A D3D9-style shader bytecode to SPIR-V translator (partial coverage).
- Guest-memory/GMR handling and framebuffer presentation code.
- A device integration harness and experimental QEMU preload library.
- Mock unit tests, real-Vulkan tests, and a VirtualBox D3D9 reference harness.

## Source layout

| Directory | Contents |
| --- | --- |
| `include/` | Public library headers and bundled Vulkan headers |
| `include/internal/` | Private renderer headers |
| `src/` | Renderer and QEMU implementation files |
| `tests/` | Test programs |
| `shim/include/` | Compatibility headers, including `mock/` |
| `shim/src/` | Compatibility implementations |
| `tools/include/` | Reference harness helper headers |
| `tools/` | Reference harness implementation |

Internal headers are not a stable public API. `.editorconfig` defines whitespace conventions; Makefile recipes use tabs.

## Build on Linux

Requires a C++17 compiler, GNU Make, binutils, pthreads, and libdl. Vulkan headers are bundled. Real rendering additionally requires the Vulkan loader, a working Vulkan driver, and validation layers. Software Vulkan such as Mesa lavapipe is usable for testing.

Example Debian/Ubuntu dependencies:

```sh
sudo apt install build-essential libvulkan1 mesa-vulkan-drivers vulkan-tools vulkan-validationlayers
```

Build from the repository root:

```sh
git clone https://github.com/sukar0972/vlkn4svga3d.git
cd vlkn4svga3d
make -j4 all
```

Outputs include `lib/libsvga3_vlkn.a`, `lib/libqemu_svga3d.so`, and test executables under `bin/`. Public interfaces are in `include/svga3_vlkn.h` and `include/qemu_vmsvga.h`. Link consumers with the static library, `-ldl`, and `-pthread`.

Make generates header dependency files for compiled objects, so header edits rebuild affected objects.

## Tests

Basic reference and mock-engine checks:

```sh
make test
```

These checks do not prove correct GPU rendering. Run the broader suite separately:

```sh
vulkaninfo --summary
make acceptance
```

The acceptance runner stops on the first failure and writes evidence under `artifacts/`. Missing Vulkan drivers or validation layers may prevent these tests from running. Read the actual logs; successful assertions do not establish full protocol coverage.

Individual targets:

| Command | Purpose |
| --- | --- |
| `make test-oracle` | VirtualBox reference implementation with mock D3D9 |
| `make test-vlkn` | Mock engine checks |
| `make test-real-vulkan` | Driver and validation checks |
| `make test-shader-translation` | Shader translation checks |
| `make test-shader` | Shader execution checks |
| `make test-guest-mem` | Guest-memory handling |
| `make test-verified-rendering` | Rendering comparisons |
| `make test-presentation` | Framebuffer presentation |
| `make test-qemu` | Host-side device integration harness |

**`test-qemu` does not boot an actual QEMU guest.** Passing it is not proof of working Linux, Windows, or QNX graphics. Test names and success banners inherited from development should not be read as completeness claims.

## Harness loop

`make harness-loop` runs the tight build/test loop used for iterative translator and driver work:

```sh
make harness-loop                      # one full build + all suites
make harness-loop HARNESS_ICD=/path/to/icd.json
SPIRV_TOOLS_DIR=/path/to/spirv-tools/build/tools make harness-loop
scripts/harness_loop.sh --iterations 5 # repeat; stops on pass->fail regressions
scripts/harness_loop.sh --fail-fast --skip-build
```

The loop requires a real Vulkan ICD (`HARNESS_ICD`, else a hard failure — never a silent skip), enables `spirv-val` when `SPIRV_TOOLS_DIR` provides it, builds everything, then runs the ICD-free translator suite followed by all lavapipe-backed suites. Logs and a `results.tsv` are written per iteration under `.harness-loop/`. Iteration stops on the first pass→fail regression between runs.

What each suite proves:

| Suite | Proves |
| --- | --- |
| `test_translator_novulkan` | D3D9→SPIR-V translation without a Vulkan ICD; optional `spirv-val` (prints SKIP, not PASS, when unavailable); SPIR-V value-flow assertions that shader inputs actually reach the declared outputs (not just decorations) |
| `test_shader_translation` | Translation plus real Vulkan shader-module creation; malformed/unsupported bytecode rejected; `_SAT` handling |
| `test_real_vulkan` | Real device init and bit-exact buffer upload/download; no mock fallback |
| `test_svga3_vlkn` | Engine/unit suite incl. failure-injection of Vulkan dispatch (flush-error propagation) |
| `test_shader_execution` | Translated shaders execute under real Vulkan |
| `test_guest_memory` | Guest memory, GMR/translation, transfer paths |
| `test_malformed_inputs` | Malformed FIFO/input and resource-accounting rejection |
| `test_verified_rendering` | Analytical pixel/rendering scenes (repository-local; does not prove external GLES rendering) |
| `test_presentation` | Presentation/framebuffer paths |
| `test_qemu_integration` | Host-side integration only; no QEMU guest boots |

## QEMU integration limitations

`src/qemu_svga3d_preload.cpp` is an experimental, build-specific hook with guest-specific workarounds. Do not install it globally or preload it into an arbitrary QEMU build. It requires separate review of binary offsets, device layouts, capabilities, and guest-memory handling. This repository does not provide a supported Proxmox deployment procedure. Use disposable, isolated VMs for integration work.

## Snapshot and licensing

Updated from the development source on 2026-09-25. Build products, downloaded system packages, VM images, host configuration, and old runtime logs are excluded. See `VALIDATION.md` for publication-time checks.

Third-party source notices are retained. `DevVGA-SVGA3d-win.cpp` and related VirtualBox files carry GPL-2.0 notices; the license text is in `COPYING`. VMware protocol headers retain their permissive notices. Bundled Khronos headers carry their own license identifiers. No blanket relicensing of third-party code is implied; consult individual files. A separate license grant for original project code has not yet been selected.
