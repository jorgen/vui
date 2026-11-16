# vui

Windows, input and a declarative UI toolkit on the vio event loop (github.com/jorgen/vio). It used
to live in vio as `gui/` and moved out with its history in November 2026. `PLAN.md` is the design
and the roadmap; `DECISIONS.md` records every choice, numbered G1 onwards, with its reason.

## Build

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/vui_tests
```

Dependencies come through cmake-dep (`cmake/3rdPartyPackages.cmake`, `cmake/Build3rdParty.cmake`):
vio pinned by commit, FreeType, HarfBuzz, Vulkan-Headers, doctest, and slangc when
`VUI_FETCH_SLANG=ON`. vio builds BoringSSL, so Go must be on `PATH`, and NASM too on Windows.

To change vio and vui together, build against a vio checkout instead of the pin:
`-DCMDEP_SOURCE_DIR_VIO=/path/to/vio`. When vio changes, move the `CmDepFetchGit(vio ...)` pin to
a commit that is on GitHub, or CI cannot fetch it.

On Windows, run `vcvars64.bat` before configuring.

## Tests

- Windows and macOS: `./build/vui_tests`. On Windows, `ctest` also runs the examples with frame
  limits.
- Linux: `test/run_linux.sh build/vui_tests` starts Xvfb and a headless weston and runs the tests
  and examples on X11 and on Wayland. Set `VIO_BACKEND=io_uring` to run on io_uring instead of
  epoll.
- Linux ASan locally: the image `ghcr.io/jorgen/vio-ci:latest` (from vio's `Dockerfile`) has every
  package the Linux backends need. Run it with `--security-opt seccomp=unconfined` so io_uring
  works, and mount vio as well if you build against a local checkout.
- `.gitattributes` keeps `*.sh` and `*.supp` LF. A CRLF `run_linux.sh` fails with "pipefail:
  invalid option", and a CRLF `lsan.supp` silently matches nothing.
- MSVC's ASan has no LeakSanitizer, so leaks only show up on Linux.

## Architecture

- `src/vui/` holds the public headers: `vui.h` (display, window, events), `context.h` (GL, Vulkan,
  Metal, D3D12, OpenCL and CUDA interop) and `ui/ui.h` (the toolkit). The targets are `vui`
  (`vui::vui`) for the window layer and `vui_ui` (`vui::ui`) for the toolkit.
- Backends: `src/backend_x11.cpp`, `backend_wayland.cpp`, `backend_win32.cpp` and
  `backend_cocoa.mm`. At run time `VUI_BACKEND=x11|wayland` picks a Linux backend.
- `frame()` is awaited. Everything else a window reports (input, `resize_event_t`,
  `close_request_t`) arrives through typed subscriptions, `window.subscribe<key_event_t,
  text_event_t>()`, each with its own queue, so every subscriber sees every event of its types
  (G19). Every await yields `std::nullopt` once the window closes.
- Window-system callbacks only record events, and a `vio::before_block_hook_t` resumes the waiters.
  On macOS and Windows the loop blocks in a host wait (`vio::loop_set_host_wait`) and runs
  `vio::loop_run_nested()` turns inside modal resize loops. On Wayland a frame is only delivered
  after the previous one was committed (G13).
- **Only vio's public API** (G48). Nothing under `vio/loop/` and nothing in `vio::detail`. vio
  exposes what vui needs: `fd_watch.h` for the display connection, `host_loop.h`, `loop_hooks.h`
  and `handle_wait.h`. If vui needs more, add it to vio properly rather than reaching in.
- vui code lives in `namespace vui`, so vio names are always written `vio::`.
- Text widgets (G50) are fixed-width: `text_t::advance` puts text on a grid, so caret and selection
  are column arithmetic. A text input reads `typing_event_t`, never a merge of keys and text,
  which reorders them. A view that sizes itself from `pressable_t::bounds()` must not place
  anything that can grow the box being measured, or it grows every frame; put it in a
  `box_t{.clip = true}`, which does not grow to fit its children.
- No graphics library is linked. GL, EGL and Vulkan are loaded at run time (G6, G31). The UI
  renderer prefers Vulkan and falls back to GL (G32). Its SPIR-V is committed in
  `src/shaders/ui_spirv.h`, and only the `vui_shaders` target (slangc) regenerates it.

## Gotchas

- Windows headers define `small` as a macro, so a variable named `small` builds everywhere except
  MSVC.
- glibc declares `error_t`, so write `vio::error_t` rather than relying on a `using`.
