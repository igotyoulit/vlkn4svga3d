<img src="docs/logo.svg" width="720" alt="vlkn4svga3d">

# vlkn4svga3d

Experimental SVGA3D to Vulkan library and QEMU integration prototype. Internal names still use `svga3_vlkn`.

## Status

Work in progress. This is not a finished virtual GPU. This is not a drop-in VMware replacement. Minecraft has reached an in-game world in the PlayBook guest. That is one guest milestone. It does not imply broad compatibility.

## Lab adapter

The preload adapter targets one allowlisted QEMU build. It checks instruction bytes before patching. A build-id mismatch exits with code 78 and logs the observed build-id. The adapter only arms inside `qemu-system*` processes. Never set global `LD_PRELOAD`. The tested lab VM runs QEMU as `qemu119` with guest graphics `svga3d`. The framebuffer address comes from the device register.

Environment variables:

- `SVGA3_VLKN_VALIDATE=1` forces validation layers on. `=0` forces them off. Otherwise debug builds validate and release (`NDEBUG`) builds do not. Layers load only if `VK_LAYER_KHRONOS_validation` is installed.
- `SVGA3_VLKN_GUEST_PROFILE=playbook-portrait` enables portrait mode overrides.
- `SVGA3_VLKN_LEGACY_CLIENT_PRESENT=1` enables the old application-context centering heuristic.

## Build

Requires a C++17 compiler, GNU Make, binutils, pthreads, and libdl. Vulkan headers are bundled. Real rendering also needs the Vulkan loader, a working driver, and validation layers. Mesa lavapipe works for tests.

Debian/Ubuntu example:

```sh
sudo apt install build-essential libvulkan1 mesa-vulkan-drivers vulkan-tools vulkan-validationlayers
```

```sh
git clone https://github.com/sukar0972/vlkn4svga3d.git
cd vlkn4svga3d
make -j4 all
```

Outputs: `lib/libsvga3_vlkn.a`, `lib/libqemu_svga3d.so`, and test binaries under `bin/`. Public headers are `include/svga3_vlkn.h` and `include/qemu_vmsvga.h`. Link consumers with `-ldl -pthread`.

## Tests

```sh
make test            # mock and reference checks only
make acceptance      # broader suite; needs a Vulkan ICD and validation layers
make harness-loop    # build and test loop; needs HARNESS_ICD pointing at a real ICD
```

`make test` does not prove correct GPU rendering. `make acceptance` stops on the first failure and writes evidence under `artifacts/`. `test-qemu` and `test_qemu_integration` do not boot a guest.

## Tests on Ubuntu

Publication checks ran on Ubuntu with g++ 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1), software Vulkan, on 2026-09-25. Logs are under `validation/`. See `VALIDATION.md`.

- `make -j4 all` passed. Compiler warnings remain.
- `make test` passed, including 3407 mock-engine assertions. The Oracle runner in the same log passed 1508 tests.
- `make acceptance` passed on llvmpipe (LLVM 20.1.2, 256 bits), Vulkan API 1.4.318, with validation layers on and zero validation errors in the real-Vulkan check.
- The acceptance log covers: real Vulkan device sanity, D3D9 bytecode to SPIR-V shader translation, guest memory (GMR2), verified rendering (8 deterministic scenes), presentation, and an isolated QEMU device harness (PCI, FIFO wrap, cold-boot reset in-process). The QEMU harness does not boot a guest.

## Licensing

VirtualBox-derived files carry GPL-2.0 notices. The license text is in `COPYING`. VMware protocol headers keep their permissive notices. Bundled Khronos headers keep their own licenses. No blanket license has been chosen for original project code yet. See `VALIDATION.md` for publication-time checks.
