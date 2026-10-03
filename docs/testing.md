# Testing

Run these commands from the repository root.

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

Required packages for `make acceptance` (Debian/Ubuntu names; other distros equivalent):
- a Vulkan ICD — e.g. `mesa-vulkan-drivers` (provides lavapipe for headless runs) plus `libvulkan1`
- `vulkan-validationlayers` — needed by the suites that enable `VK_LAYER_KHRONOS_validation`
- `spirv-tools` — optional; enables the `spirv-val` step in shader-translation suites (they report SKIP, not PASS, when it is absent)
- `vulkan-tools` — optional; provides `vulkaninfo` for the pre-flight check above

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

## Piglit guest OpenGL tests

[Piglit](https://piglit.freedesktop.org/) adds external OpenGL correctness tests through the guest's Mesa SVGA driver and the QEMU renderer. Install it **inside the Debian guest** with an existing Xorg session:

```sh
sudo apt install piglit mesa-utils
```

From this repository on the development machine, run:

```sh
make test-piglit PIGLIT_ARGS="--ssh user@guest"
```

The default compares the same focused selection from Piglit's `quick` profile using SVGA3D, then guest CPU rendering via llvmpipe. It covers legacy texture operations, blending, scissor/depth/stencil, framebuffer objects, and selected GLSL 1.10/1.20 execution tests. Tests run serially on display `:0`, with a 60-second per-test timeout and a 30-minute limit per renderer. The runner checks `glxinfo` renderer identity before each run, forces GLX, and clears software-rendering and version overrides. The software choice applies only to the test subprocesses.

The current lab uses `svga3d@10.0.0.144`, display `:0`, in Proxmox VM119 on `10.0.0.200`. QEMU loads the host's `/usr/local/lib/libqemu_svga3d.so`. Piglit belongs in the guest: running it on the Proxmox host would bypass the guest SVGA path. Ensure the intended library is already loaded before testing; the runner does not deploy or restart QEMU. Keep the VM idle for consistent results.

The initial VM runs reproduced Intel GPU hangs while `copyteximage 2d`, `texsubimage`, and framebuffer format tests were executing, followed by Vulkan device loss and stalled tests. The smoke runner schedules copyteximage/texsubimage cases last, preserving them in the test list so earlier cases can finish. `--last REGEX` overrides this ordering. This ordering changes no expected outcomes and does not suppress failing cases. If device loss occurs, stop the accelerated run, retain the logs, and recover before further accelerated testing. Unexecuted cases are not rendering failures or passes.

Other runs:

```sh
# Only the guest SVGA path:
make test-piglit PIGLIT_ARGS="--ssh user@guest --renderer svga"
# Broader upstream profile, including unsupported API features (many expected skips):
make test-piglit PIGLIT_ARGS="--ssh user@guest --suite quick --run-timeout 14400"
# Investigate a particular feature; --include replaces the default selection:
make test-piglit PIGLIT_ARGS="--ssh user@guest --include arb_depth_texture"
# Run directly inside the guest after copying this repository:
make test-piglit
```

Use `--piglit /path/to/piglit` for a built upstream checkout, `--display` for a different X session, and `--exclude` to make a deliberately narrowed run. Exact selection and commands are saved. Debian's package is a dated snapshot; compare runs using the same Piglit version and selection.

Each run creates a new `artifacts/piglit-*` directory with renderer/system identity, package versions, command lines, logs, raw compressed JSON, `summary.json`, and an HTML report under `html/index.html`. The JSON summary counts top-level cases and subtests separately; upstream console totals may differ. SSH runs also retain a temporary guest result directory, printed at startup. Copying evidence back happens even if the runner fails. Inspect raw test output for expected/observed pixels and API errors.

Exit status is nonzero for test failures, crashes, timeouts, warnings, incomplete/missing results, mismatched comparison test lists, or runs without any passing cases. A nonzero Piglit runner exit retains partial evidence and still attempts the software reference. The summary marks interrupted runs as incomplete and planned cases that never started as `notrun`. Skips remain separate from passes. `summary.json` distinguishes SVGA problems, skips, and unverified cases that pass on llvmpipe; skips can reflect differing advertised capabilities. Such differences identify investigation targets, not their cause. This is independent of `make acceptance` and `make harness-loop`; CI checks the result-reporting logic, while guest tests require the live VM. Piglit does not certify VM stability, desktop scanout, or full API conformance.

## Harness loop

`make harness-loop` runs the tight build/test loop used for iterative translator and driver work:

```sh
make harness-loop                      # one full build + all suites
make harness-loop HARNESS_ICD=/path/to/icd.json
SPIRV_TOOLS_DIR=/path/to/spirv-tools/build/tools make harness-loop
scripts/harness_loop.sh --iterations 5 # repeat; stops on pass->fail regressions
scripts/harness_loop.sh --fail-fast --skip-build
```

The loop requires a real Vulkan ICD (`HARNESS_ICD`, else a hard failure — never a silent skip), enables `spirv-val` when `SPIRV_TOOLS_DIR` provides it, builds everything, then runs the ICD-free translator and preload FIFO suites followed by all lavapipe-backed suites. Logs and a `results.tsv` are written per iteration under `.harness-loop/`. Iteration stops on the first pass→fail regression between runs.

What each suite proves:

| Suite | Proves |
| --- | --- |
| `test_translator_novulkan` | D3D9→SPIR-V translation without a Vulkan ICD; optional `spirv-val` (prints SKIP, not PASS, when unavailable); SPIR-V value-flow assertions that shader inputs actually reach the declared outputs (not just decorations) |
| `test_preload_fifo` | Actual preload FIFO walker on a synthetic QEMU state, without patching a process: batches exceeding 8192 commands, ring wrap, producer notification races, incomplete packets and final fences |
| `test_buffer_ordering` | Strict Vulkan validation through teardown; queued buffers/constants, compressed FIFO/GMR transfers, sampler retirement, mip/image ordering, depth sampling and shader pixel regressions |
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

