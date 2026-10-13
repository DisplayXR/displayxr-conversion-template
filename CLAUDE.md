# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repository.

## What this repo is

The **DisplayXR conversion-provider template** — a vendor-neutral, buildable starter kit a model
vendor (or a display vendor shipping a 2D→3D model) clones to write a DisplayXR **conversion
provider**: the runtime's third plug-in type (ADR-053), next to display processors and input
providers. It implements the public conversion ABI (`xrtConversionPluginNegotiate` +
`xrt_conversion_plugin_iface`) with **one CPU-only `DEPTH_MONO` capability that returns a
ramp**, plus a **fake host** that drives it through the runtime's call sequence. It is the
forkable counterpart to the runtime's in-tree `sim_conversion` provider (every task, but not
forkable) and is unrelated to `displayxr-vendor-template` (display processors: weavers).

**Hard invariant: this must stay buildable by anyone.** No dependency on any proprietary SDK,
inference runtime, private package or secret, and no runtime checkout or install: the ABI is
vendored in `include/xrt/`, and the build is plain CMake + a C11 compiler on Windows, Linux and
macOS. If a change would make the template un-buildable on a clean machine, it is wrong here.

**Public repository.** No internal data in code, comments, commit messages or docs: no dataset
names or paths, no figures from internal measurements, no vendor-module internals, no product or
title names.

## Layout

- `src/example_conversion.c` — the provider: negotiate + the vtable. Probe (settings, a simulated
  dependency check), one capability, readiness with `notify_changed`, warm / evict, the stream
  lane (CPU in, R32F out) and the job lane (a RAW_IMAGE blob into a runtime sink). Model:
  `src/xrt/drivers/sim_conversion/sim_conv_plugin.c` in the runtime.
- `tools/fakehost.c` — loads the provider by absolute path, resolves the one symbol, and runs the
  contract checks (descriptor rules = `displayxr-cli selftest`'s `conversion_providers`).
  Registered as CTest cases in `CMakeLists.txt`.
- `include/xrt/xrt_conversion_plugin.h`, `include/xrt/xrt_results.h` — vendored **verbatim** from
  the runtime (`src/xrt/include/xrt/`, tag noted in `README.md`). Never edit them here; update by
  copying from a runtime release.
- `scripts/register-dev.{bat,sh}` — dev registration (`HKLM\Software\DisplayXR\ConversionProviders\
  example-conversion`, `Version` written last; POSIX `NNN-<id>-conversion-provider.json`).
- `examples/` — a full registry row (`.reg`) and manifest, with settings.
- `.github/workflows/build.yml` — Windows + Linux + macOS: build with warnings as errors, CTest,
  exactly-one-export check, artifacts.

## The contract (source of truth: the runtime repository)

- `docs/specs/runtime/conversion-provider-discovery.md` — normative: ABI rules, threading,
  hosting-agnostic rules, registry / manifest, readiness, the installer contract (§3.8).
- `docs/guides/vendor-plugin-onboarding.md` §9 — the walk-through this template follows.
- `docs/adr/ADR-053-conversion-providers.md` — rationale.

Rules when touching provider code:
- `iface.struct_size = sizeof(struct xrt_conversion_plugin_iface)`; return
  `XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT` from negotiate.
- Read and write a top-level struct only within its `struct_size` (`XRT_CONV_COVERS`; the `SET`
  macro). Embedded structs are frozen.
- One call in flight per provider; only `get_provider_status`, `describe_cap`,
  `get_generation`, `get_state`, `stream_get_output_desc` run concurrently and must answer from
  memory (the mutex guards what they read).
- Configuration only via `host->get_setting`; never `getenv`. No windows on non-pumping threads,
  no process-state changes, network only in `warm`. `destroy` may never be called.
- Honest units: report the depth kind actually produced and the per-frame range; never invent
  intrinsics, a scale or a baseline.
- Version gate: a dependency present but too old is `FAILED` + `XRT_CONV_FAIL_VERSION`, never
  `FAIL_MODEL`; a missing one is `ABSENT`.

## Building

```
cmake -S . -B build -DEXAMPLE_WERROR=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Conventions

- License every new file `BSL-1.0` with a `The DisplayXR Project` copyright header.
- Keep every stub honestly marked `VENDOR TODO`. Never present the ramp as a model.
- Match the runtime's style: tabs, 110 columns, `snake_case`, the `ex_` prefix in the provider.
- Commits: conventional-commit subjects.
