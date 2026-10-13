# DisplayXR conversion-provider template

A buildable starter kit for a **DisplayXR conversion provider**: a plug-in that turns 2D into
3D — depth from an image or a video, stereo or N views, disparity from a stereo pair, Gaussian
splats from a photo — for every app on a machine running the
[DisplayXR runtime](https://github.com/DisplayXR/displayxr-runtime).

A conversion provider is the runtime's third plug-in type, next to display processors and input
providers. A model vendor is not a display vendor: there is no weaver, no display device and no
eye tracking here. The runtime owns every thread, queue, policy and app-facing API (apps reach
providers through the `XR_DXR_lift` extension and never learn which one served them); a provider
does **pixels and units only**.

This template is one CPU-only provider with a single capability — `DEPTH_MONO`, one image to
depth, on the stream and job lanes — whose "model" is a vertical ramp. It needs no inference
runtime, no GPU and no SDK, and builds with plain CMake on Windows, Linux and macOS.

| Read | For |
|---|---|
| Runtime: `docs/guides/vendor-plugin-onboarding.md` §9 | the walk-through: what to ship, the ABI on one page, the rules, registration, readiness, licences, the installer contract, testing |
| Runtime: `docs/specs/runtime/conversion-provider-discovery.md` | the normative contract |
| Runtime: `docs/adr/ADR-053-conversion-providers.md` | why it is shaped this way |
| Runtime: `docs/specs/extensions/XR_DXR_lift.md` | what apps see |

## What is in it

| Path | What |
|---|---|
| `src/example_conversion.c` | the provider: `xrtConversionPluginNegotiate` and the `xrt_conversion_plugin_iface` vtable. Every place a real provider differs is marked `VENDOR TODO` |
| `include/xrt/xrt_conversion_plugin.h`, `include/xrt/xrt_results.h` | the plug-in ABI, vendored verbatim from the runtime (below) |
| `tools/fakehost.c` | a **fake host**: loads the provider binary the way the runtime does and drives it through the runtime's call sequence, checking the contract — no runtime install needed |
| `scripts/register-dev.bat`, `scripts/register-dev.sh` | register the built provider with a runtime on a dev box (Windows registry row; POSIX manifest) |
| `examples/settings/example-conversion.reg`, `examples/manifest/050-example-conversion-conversion-provider.json` | a full registration row and manifest, with settings |
| `.github/workflows/build.yml` | CI: build, fake-host tests and an exactly-one-export check on Windows, Linux and macOS |

## Build

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Output: `DisplayXR-ExampleConversion.dll` (Windows, `build/Release/`), `.so` (Linux) or `.dylib`
(macOS) in `build/`. The DLL links the C runtime statically and imports only `KERNEL32.dll`,
and it exports exactly one symbol, `xrtConversionPluginNegotiate`. `-DEXAMPLE_WERROR=ON` turns
warnings into errors, as CI does.

## Run the fake host

```
build/Release/example-conversion-fakehost build/Release/DisplayXR-ExampleConversion.dll
build/example-conversion-fakehost build/DisplayXR-ExampleConversion.so --set WARM_MS=500
build/example-conversion-fakehost build/DisplayXR-ExampleConversion.so --set DEPENDENCY=too-old --expect version
```

It negotiates against a stub host iface, probes, walks `describe_cap` (also at a truncated
`struct_size`, checking nothing is written past it), validates each descriptor with the rules of
`displayxr-cli selftest`, reads the state, warms, runs a CPU stream and a job on every depth
capability and checks the output values and the reported units, evicts and destroys. Settings
come from `--set KEY=VALUE`, or from `DXR_CONV_<ID>_<KEY>` in the environment, which wins — the
runtime's order. Exit code 0 = all checks passed. Passing it does not prove the provider works
inside the runtime's conversion host; it proves the calls are made the way that host makes them.

## Make it yours

1. **Rename.** The id `example-conversion` (in `src/example_conversion.c`, both registration
   scripts and the examples) becomes your registry subkey and manifest id; the target
   `DisplayXR-ExampleConversion` becomes `DisplayXR-<Vendor>Conversion`.
2. **Describe what you really do.** In `fill_cap`: the task, lanes, input kinds, outputs, the
   memory and sync kinds you import, the limits (`max_streams`, `max_input_edge`, `min_views` /
   `max_views` for views tasks), the depth kind you actually produce, the licence class. Every
   `VIEWS_*` capability must set `XRT_CONV_CAP_EXPLICIT_VIEWPOINTS`. Keep `XRT_CONV_MEM_CPU`: the
   runtime copies anything you cannot import, so CPU is always a complete path. A library that
   fixes one model per process declares one capability whose variant follows the quality hint.
3. **Check your dependency in `probe`** — cheaply, no model load, no network. Missing →
   `ABSENT`; present but older than your floor → `FAILED` + `XRT_CONV_FAIL_VERSION` (never
   `FAIL_MODEL`); unusable GPU / OS → `INCOMPATIBLE`. Load your inference runtime by absolute
   path or delay-load it: the provider DLL must load without it.
4. **Load the model in `warm`** on the runtime's thread, publishing progress and polling
   `is_cancelled` at least every 250 ms; free it in `evict`.
5. **Run the model in `stream_convert` / `job_execute`** (`run_model`), writing only into the
   runtime-allocated targets, and fill `xrt_conv_depth_info` honestly: the depth kind, the
   per-frame range before any normalisation, intrinsics only with their source. Unknown is zero.
6. **Never** read the environment (`host->get_setting` only), create a window on a thread that
   does not pump messages, change process state (`SetDllDirectory`, `putenv`, `exit`, signal
   handlers), touch the network outside `warm`, or apply policy (viewpoints, convergence, depth
   budget): render exactly the viewpoints and convergence the runtime hands you.

## Settings

The provider reads settings with `host->get_setting(KEY)`, which resolves, in the process that
hosts it: `DXR_CONV_EXAMPLE_CONVERSION_<KEY>` in that process's environment (the conversion host
inherits the **service's** environment, never an app's), else `Settings\<KEY>` under the
registry row (Windows) or the manifest's `settings` object (POSIX).

| Key | Values | Effect in the template |
|---|---|---|
| `RAMP_DIRECTION` | `vertical` (default), `horizontal` | the ramp's axis |
| `WARM_MS` | ms (default 0) | a simulated warm-up, with progress |
| `DEPENDENCY` | `present` (default), `absent`, `too-old` | simulates the dependency check: `ABSENT`, or `FAILED(VERSION)` |
| `ENABLE` | `0` | `probe` declines |

## Register on a dev box and test with the runtime

You need a DisplayXR runtime: installed, or built from source.

**Windows** (elevated prompt for the registration, non-elevated for the rest):

```
scripts\register-dev.bat build\Release\DisplayXR-ExampleConversion.dll
displayxr-cli conv list          :: providers x tasks, selection order, why each row is kept or dropped
displayxr-cli conv status        :: readiness
displayxr-cli selftest           :: the conversion_providers check (exit 13 = malformed descriptor)
scripts\register-dev.bat unregister
```

**Linux / macOS:**

```
scripts/register-dev.sh build/DisplayXR-ExampleConversion.so   # or .dylib; --dir DIR to choose the root
displayxr-cli conv list
scripts/register-dev.sh unregister
```

With no service running, `conv list` / `conv status` fall back to a headless pass that
negotiates, probes and describes your provider but never warms it. With the service running,
the provider runs in its own `displayxr-conversion-host` process; its log is
`DisplayXR_conversion-host_example-conversion.<pid>_<ts>.log` (Windows:
`%LOCALAPPDATA%\DisplayXR\`), and its warnings appear in the service log prefixed
`[conv:example-conversion]`. The template registers at ProbeOrder 50 (the vendor band), so it
is selected for `depth-mono` ahead of the open-model and sim providers; `displayxr-cli conv use
depth-mono <id>` pins a choice per user.

`DEPTH_MONO` serves single-image depth (still images, photo-hinted depth streams, spec-4
`XR_LIFT_TASK_DEPTH_MONO_DXR` streams, and jobs). Video depth is a separate task: to serve it,
add a `DEPTH_VIDEO` capability with `XRT_CONV_CAP_TEMPORAL` that resets its temporal state on
`XRT_CONV_FRAME_RESET`.

### Updating a registered provider

While the service hosts your provider, its conversion host maps your binary, so a rebuild cannot
overwrite it. On a dev box: stop using lift, let the host exit (it does after 10 minutes without
demand, or restart the service), rebuild, and test again; or build into a fresh folder each time
and register that path.

A shipping **installer** follows the runtime's contract (onboarding guide §9.8, discovery spec
§3.8) — **close the host, never the service**:

1. create and own the mutex `Global\DisplayXR-ConvMaintenance-<id>`;
2. Restart Manager on your provider's files (it lists the conversion host, not the service);
   the host exits 20 and your tasks report `WARMING(updating)` while every app session stays up;
3. replace the files and write the registry row with `Version` last;
4. end the Restart Manager session and start nothing;
5. release the mutex; the runtime respawns the host on demand.

## The ABI you build against

`include/xrt/` is vendored verbatim from the runtime at `v2.41.1`
(`src/xrt/include/xrt/xrt_conversion_plugin.h` and `xrt_results.h`), conversion-provider ABI major
`XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT` = **1**. The header is self-contained and carries its
own `sizeof` / `offsetof` tripwires, so a mismatched copy fails to compile. Within a major the
ABI only grows at the end of top-level structs (each carries `struct_size`) and of the vtable;
the runtime's loader rejects a provider of another major before calling anything. To update,
copy the two files from a newer runtime release; you need to rebuild only to use what that
release appended.

Optional: link the runtime's `DisplayXRClient` import library to log through the runtime's
`u_log_*` surface. The template does not, so it builds with no runtime present.

## Licence

[BSL-1.0](LICENSE), like the DisplayXR runtime. Your provider's own licence class
(`PERMISSIVE`, `COMMERCIAL`, `RESEARCH_ONLY`) is a separate matter: it describes your model, its
dependencies and the inference runtime you load, and a `RESEARCH_ONLY` provider is selected only
after the user opts in.
