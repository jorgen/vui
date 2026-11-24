# vui in the browser, and WebGPU through Dawn

A plan, not yet a decision. Two pieces of work that share one renderer:

1. **The browser.** vui built with Emscripten: a window is a `<canvas>`, the loop is driven by the
   browser's own callbacks, and the toolkit draws through WebGPU.
2. **Native WebGPU.** The same WebGPU renderer built against Dawn on Windows, macOS and Linux, where
   Dawn draws through D3D12, Metal and Vulkan. It sits beside vui's own renderers, not in place of
   any: Vulkan and GL stay, and phase 12's hand-written D3D12 and Metal renderers are still wanted.
   Every API is a choice in `render_api_t`.

The renderer targets `webgpu.h`, not Dawn itself. In the browser, `webgpu.h` is Dawn's
emdawnwebgpu port over `navigator.gpu`. On the desktop it is Dawn's `webgpu_dawn` shared library,
or wgpu-native, which implements the same header. So one `src/ui_webgpu.cpp` serves both pieces,
and building it natively first is how the browser renderer gets debugged: native builds have Dawn's
validation messages, RenderDoc and PIX.

Proposed decisions are numbered G81 onwards and go into `DECISIONS.md` as each phase lands.

## Where things stand

- **vio on wasm** (`vio/src/vio/platform/wasm/`) is a small loop of its own: `run_in_loop`,
  `emscripten_set_timeout` timers, about-to-block listeners, `event_pipe_t`, `sleep` and HTTP fetch.
  It is not on the shared `loop_core`. `vio/host_loop.h` and `vio/loop_hooks.h` both `#error` under
  `__EMSCRIPTEN__`, and `vio::run` is a `static_assert` there. vio's own roadmap (EVENT_LOOP_PLAN.md,
  phase 8) already lists "wasm on the shared core".
- **What vui takes from vio** is small: `before_block_hook_t`, `timer_t` (create, rearm, disarm,
  on_fire, keeps_loop_alive), `deferred_call_t`, `loop_now`, `sleep`, `cancellation_t`, `task_t`,
  `fd_watch_*` (X11 and Wayland only), and `host_wait_t`, `loop_run_nested` and `loop_next_timeout`
  (Win32 and Cocoa only). The browser backend needs everything in that list except fd watches and
  the blocking host wait.
- **The renderer seam** (`src/ui_renderer.h`) is already narrow: `render(display_list)` and
  `describe()`, plus `make_vulkan_renderer` / `make_gl_renderer` and `render_offscreen_vulkan` for
  tests. A WebGPU renderer is a third implementation behind it.
- **The shader** (`src/shaders/ui.slang`) needs a few changes for WGSL. It uses a push-constant
  block (WebGPU has none), `Texture2D.Load` (becomes `textureLoad`), a `uint` instance attribute
  (fine), `SV_VulkanVertexID` (WGSL's `vertex_index` already counts from `firstVertex`), and
  `precise` in the area-seam arithmetic of G41 (WGSL has no `precise`).
- **cmake-dep** passes `CMAKE_TOOLCHAIN_FILE` to its dependencies, so FreeType and HarfBuzz should
  cross-compile with `emcmake` as they are. vio under Emscripten builds without BoringSSL, so a web
  build does not need Go or NASM.
- `../emsdk` is installed on this machine.

## Part 1: the loop in the browser (vio)

The browser does not let anything block. A turn has to run inside a callback the browser makes, and
it has to return. vio already has what a host-driven loop needs, because Win32 and Cocoa use it
inside modal loops: `loop_run_nested()` runs one turn that never blocks, and `loop_next_timeout()`
says when the next one is due.

**G81 (proposed). The browser is a backend of vio's shared core, not a second loop.** vio grows a
`backend_browser`: no descriptors (`fd_watch_create` returns `not_supported`), timers from the
core's timer heap, and a wake that schedules a turn. Hooks, timers, deferred calls and cancellation
then behave on the web exactly as they do natively, and vui's display code does not fork. The
current wasm loop's extras (`event_pipe_t`, HTTP fetch, the cooperative multi-loop driver) move
across onto the same core. This is vio's phase 8, done ahead of time because vui needs it.

**G82 (proposed). On the web a turn runs inside the browser callback that caused it.**

- A DOM event callback (key, pointer, resize) records the event the way every backend does, then
  runs a turn synchronously before it returns. The before-block hook resumes the waiters in that
  turn, as on every other platform.
- A `requestAnimationFrame` callback records the frame and runs a turn, so the frame coroutine draws
  within the animation frame that asked for it.
- After every turn the driver keeps one `setTimeout` armed for `loop_next_timeout()`, and replaces
  it when the deadline changes.
- Work finishing outside any callback (a fetch, a WebGPU future) posts to the loop with
  `run_in_loop`; the post itself asks the browser for a turn through `setImmediate`.
- One rule follows from this. A handler has to decide `preventDefault()` before it returns, and it
  cannot wait for a coroutine to answer. The web backend decides by policy (keys and the wheel over
  a focused canvas are taken) rather than by asking the program.

**How `run()` behaves.** Today's examples do `loop.run()` with the loop and windows on `main`'s
stack, or `return vio::run(...)`.

- *Base, for every browser:* `event_loop_t::run()` starts the driver and returns through
  `emscripten_exit_with_live_runtime()`. That skips destructors, so anything the loop uses has to
  live on the heap. `vio::run(f)` becomes available on wasm: it puts the loop *and the callable* on
  the heap, because a coroutine lambda's captures live in the lambda object, which would otherwise
  die with the unwound stack. When the task finishes, it calls `emscripten_force_exit(code)`. Every
  example that already uses `vio::run` then works unchanged. The two that use `loop.run()` directly
  (`hello_window`, and any others like it) move to `vio::run`.
- *Optional, `VIO_WASM_JSPI=ON`:* `run()` blocks for real. The host wait becomes a JS promise
  (resolved by a wake or the timeout) that the wasm stack suspends on through JSPI, so `main` keeps
  its stack. Chrome and Edge have shipped JSPI since 137. Firefox and Safari are not settled as of
  November 2026, so this cannot be the default. Asyncify would cover every browser but costs code
  size and speed, so it is not planned.

`vio/host_loop.h` and `vio/loop_hooks.h` stop refusing `__EMSCRIPTEN__`. `loop_set_host_wait`
returns `not_supported` there, as it already does for backends without one. G48 still holds: the
browser driver lives in vio, and vui reaches it through the public headers.

**Threads.** None at first. Emscripten pthreads need `SharedArrayBuffer`, which needs COOP/COEP
headers on the page, and those headers break embedding in other pages. vio's blocking pool runs its
work inline on wasm or returns `not_supported`. Threads can come later behind an option.

## Part 2: the window layer in the browser (`backend_web.cpp`)

**G83 (proposed). A web window is a canvas.** `window_config_t` gets an optional canvas selector.
Without one, vui creates a canvas at `size` and appends it to the page. A page can hold several, so
`windows_and_network`'s two-window shape still works. Written with `<emscripten/html5.h>` where
that covers it and small `EM_JS` functions where it does not (pointer events, ResizeObserver,
composition events). `platform_t::web`, and `native_window_t` carries the canvas selector.

| window-layer feature | in the browser |
|---|---|
| frames | `requestAnimationFrame`, one per turn for all canvases. Shaped like Wayland's frame callbacks (G13): `frame_wanted` asks for one. A hidden tab gets none, which is already the rule. |
| size and scale | ResizeObserver with `devicePixelContentBoxSize` gives the exact framebuffer. Safari lacks it, so the fallback is round(CSS size × `devicePixelRatio`). Logical size is the CSS size and scale is `devicePixelRatio`, the same model as Wayland and macOS (G35). |
| keys | `KeyboardEvent.code` gives the physical key, `key` gives the text. Tab and the browser's shortcuts are taken only while a vui canvas has focus (needed for `raw_input`, G78). |
| text and IME | A hidden `<textarea>` kept at the caret takes the text. The same textarea brings up the on-screen keyboard on touch devices. vui has no composition (pre-edit) event yet on any platform, so committed text comes first and `compositionupdate` waits for that event to exist. |
| pointer | Pointer events, with `setPointerCapture` from down to up. The wheel is converted from `deltaMode` lines and pages to pixels. Touch arrives as pointer events. |
| cursor | `canvas.style.cursor` |
| title | `document.title` |
| focus | The canvas gets `tabindex`, and its focus and blur events become focus events. |
| fullscreen | `requestFullscreen`, which needs a user gesture. A turn runs inside the input callback, so a coroutine reacting to a key press is still within the gesture's activation window. |
| clipboard | Write: `navigator.clipboard.writeText` (needs a secure context and a gesture). Read: Ctrl/Cmd+V is let through to the textarea, and its `paste` event answers `request_clipboard_text`. That fits the awaited paste vui already has, without the permission prompt `readText()` brings up. No primary selection. |
| drag and drop in | The dropped files are written into MEMFS under `/dropped/` and reported as those paths, so `drop_event_t` keeps its shape. Dragging out: `not_supported`. |
| close | No close request is ever sent. `window.close()` removes the canvas if vui created it. |
| position, minimise, maximise, size limits | `not_supported` / no-ops |

`context.h` on the web: no GL at first (see phase W7 for WebGL2), and no Vulkan, Metal, D3D12,
OpenCL or CUDA. Each of those returns `not_supported`.

## Part 3: one WebGPU renderer (`ui_webgpu.cpp`)

**G84 (proposed). One WebGPU renderer, on `webgpu.h`, for the browser and for Dawn.** It maps onto
the backend seam's five operations ("upload a buffer, upload a texture region, set a scissor, draw
instanced, present"):

- Per frame, one vertex buffer of prims, grown by doubling and filled with `wgpuQueueWriteBuffer`.
  One draw per run, as on Vulkan.
- Push constants become a uniform buffer with one 256-byte-aligned slot per run, bound with a
  dynamic offset. Dawn's "immediates" extension is not standard and is left alone.
- Each atlas (the R8 glyph atlas and the RGBA image atlas) uploads its dirty rectangle with
  `wgpuQueueWriteTexture`. That replaces the per-frame staging buffer and barriers of G39, since
  WebGPU tracks those itself.
- sRGB: canvas surfaces offer only `bgra8unorm`/`rgba8unorm`. Configure with `viewFormats` set to
  the `-srgb` variant and render to that view, so blending stays linear (G38). If a browser refuses
  the view format, the shader encodes sRGB itself, chosen by a specialisation constant.
- Present: on the web the canvas presents when the task ends. On Dawn, `wgpuSurfacePresent`, FIFO.
- Device loss: the device-lost callback makes `render` return false, as Vulkan does today.

**G85 (proposed). Making the renderer is awaited.** `requestAdapter`, `requestDevice` and buffer
mapping are asynchronous, and in the browser they really are: without JSPI nothing can block for
them. Renderer creation becomes a `vio::task_t` on every API (Vulkan's finishes at once), and
a WebGPU future resumes its waiter by posting to the loop. The offscreen read-back used by the
tests is awaited the same way.

**Loaded at run time natively (G6).** vui opens `webgpu_dawn` (or the library named by
`VUI_WEBGPU_LIBRARY`, so wgpu-native can stand in), looks up `wgpuGetProcAddress`, and fills a
function table through it. Under Emscripten the same table is filled from the linked symbols. The
surface comes from `native_window_t`: HWND, Xlib window, `wl_surface`, or the `CAMetalLayer` that
`cocoa_metal_layer` already makes.

**Choosing a renderer.** `render_api_t::webgpu` is new. On the web, `automatic` means WebGPU only.
Natively, `automatic` stays Vulkan → GL, and adds WebGPU between them when vui was built with it.
That covers macOS without MoltenVK, since Dawn uses Metal there. `on_renderer` reports
`"webgpu: <adapter> (<backend>)"`.

**For programs that draw for themselves.** `vui::webgpu::create_surface(instance, window)` in
`context.h`, following Vulkan's rule: the library makes the surface, and the program keeps the
instance and device. That fills in goal 4 for WebGPU, in the browser and natively.

**G86 (proposed). WGSL comes from the same Slang source and is committed, like the SPIR-V (G25,
G33).** `vui_shaders` also writes `src/shaders/ui_wgsl.h` (`slangc -target wgsl`), so a build never
needs slangc. The push-constant block sits behind a macro: `[[vk::push_constant]]` for SPIR-V, a
`ConstantBuffer` at its own binding for WGSL. **Risk:** WGSL has no `precise`. The G41 seam test is
exactly the check for that. It runs on WebGPU too, and if it fails, the area arithmetic is rewritten
so that it does not depend on the order of operations.

**Getting Dawn: pinned to a nightly.** Dawn publishes nightly native builds and an
`emdawnwebgpu_pkg` on its GitHub releases page. cmake-dep fetches both from **one** nightly, pinned
by tag and SHA256 the way slangc is, so `webgpu.h` is the same header on the web and natively.
Moving to a newer nightly is a deliberate pin change, like vio's. Natively it is behind
`VUI_WEBGPU=ON`, off by default, until it has CI on all three platforms.

## Part 4: text, fonts and files in the browser

A page has no system fonts and no file system.

**G87 (proposed). On the web, one font is built in and fallbacks are fetched.** A sans and a mono
face (OFL, a few hundred KB) are compiled into the binary, and `font_t::system_default()` returns
them. Fallback fonts (CJK, colour emoji) are tens of MB, so they are fetched from URLs listed in
`run_config_t::fallback_fonts` the first time a missing code point asks for one, through vio's HTTP
fetch. The cell shows tofu until the font arrives. Then the atlas is emptied and the layout redone,
which G39 already handles. The "one read" loader from b0302ec becomes a fetch on the web. Programs
reach their own assets through `--preload-file` or a fetch. Neither the explorer's shell layer nor
the task manager's system layer is ported.

## Part 5: build

- `cmake --preset web` (emcmake, Ninja, Release) → `build-web/`. `emsdk` is pinned in CI by version.
- In CMake, `if(EMSCRIPTEN)` selects `backend_web.cpp` and `ui_webgpu.cpp` and leaves out X11,
  Wayland, EGL and Vulkan. The X11/Wayland link step (G7) moves behind `if(UNIX AND NOT APPLE AND
  NOT EMSCRIPTEN)`.
- Link flags: `--use-port=<emdawnwebgpu port file>` at compile and link,
  `-sALLOW_MEMORY_GROWTH`, `-sENVIRONMENT=web,node` for the targets that tests also run under Node.
- Each web example ships as `vui_<name>.html/.js/.wasm`, built from a shell page that holds one
  canvas. A `dist-web` preset puts the demos into one static folder with an index, the counterpart
  of `dist-windows`.

**Ports and what does not:** `ui_counter`, `ui_widgets`, `ui_cells`, `ui_chips` and `input_echo`
(once it no longer depends on GL) port. `hello_window` ports after W7 or with its GL swapped out.
`explorer`, `task_manager`, `windows_and_network` (raw TCP), `vulkan_clear`, `d3d12_clear` and
`metal_clear` are not built for the web.

## Testing

- **vio, under Node:** the browser backend's timers, hooks, wake and cancellation, extending
  `test/wasm_smoke.cpp`. The core needs no DOM.
- **The vui toolkit, under Node:** layout, text shaping, display list, chunks and widgets on
  `make_null_backend`: `node build-web/vui_tests.js`. This is most of `vui_tests`.
- **`render_offscreen_webgpu`** next to `render_offscreen_vulkan`, with the pixel tests run against
  both from the same expectations. That checks the WGSL translation, sRGB blending, and the seam and
  baseline tests that were each confirmed to catch their bug.
  - Natively: Dawn on Vulkan (lavapipe in Linux CI), D3D12 (WARP in Windows CI) and Metal (macOS CI).
  - In the browser: headless Chrome through Playwright with `--enable-unsafe-webgpu
    --use-webgpu-adapter=swiftshader`.
- **The web backend, in headless Chrome:** a test page starts a test binary. Playwright sends real
  input through CDP (`Input.dispatchKeyEvent`, mouse, IME composition) and resizes the viewport and
  the device scale factor. The binary checks what arrived, the way the X11 and Wayland tests do
  under Xvfb and weston.
- **CI:** one Linux job, `emsdk` plus Node and Playwright's Chromium. A `VUI_WEBGPU=ON` job on each
  native platform.

## Phases

Track A does not depend on Track B and can start now. Track B's renderer step reuses A's renderer.

| phase | work | verified by |
|---|---|---|
| A1 | `webgpu.h` table loaded at run time, Dawn release through cmake-dep, `ui_webgpu.cpp`, WGSL from Slang, awaited renderer creation, `render_api_t::webgpu` | `render_offscreen_webgpu` passing every Vulkan pixel test on Dawn/D3D12 on Windows; `vui_ui_counter` and `vui_ui_chips --bench` through Dawn |
| A2 | Dawn on Linux (Vulkan) and macOS (Metal); `webgpu::create_surface` | the same tests in Linux and macOS CI |
| B1 | vio: browser backend on the shared core, hooks and timers on wasm, `vio::run` on wasm (vio D69). **Done in vio, not yet pushed or pinned.** A wake is `run_in_loop`/any post, so no separate `loop_wake` was needed | `test/wasm_smoke.cpp` and `test/wasm_run.cpp` under Node |
| B2 | vui builds under Emscripten: CMake gating, FreeType/HarfBuzz cross-compiled, `platform_t::web`. **Builds; the tests are not built for the web yet** | `vui_tests` (null backend) under Node |
| B3 | `backend_web.cpp`: canvas, rAF, size and scale, keys, pointer, focus, cursor, clipboard, IME, drop. **In, except clipboard reads, IME and drop**; checked by hand in headless Edge | Playwright input tests; `input_echo` in Chrome |
| B4 | the WebGPU renderer under emdawnwebgpu; awaited adapter. **In**: the shader encodes sRGB on the UNORM canvas, and the WGSL has its clip-space y turned over (WebGPU's points up, Vulkan's down). emsdk's own emdawnwebgpu port, Dawn `v20261105.214757`, not yet pinned through cmake-dep | offscreen pixel tests in headless Chrome; `ui_counter`, `ui_widgets`, `ui_cells` on screen in Chrome, Firefox and Safari |
| B5 | built-in fonts, fetched fallbacks, emoji | `ui_cells` showing CJK and emoji after the fetch |
| B6 | demo index and GitHub Pages (`pages.yml`), `ui_chips --bench` in the browser. **Pages and the index are in**: counter, widgets, cells and chips | the bench table for Chrome next to the native one |
| B7 | optional: JSPI `run()`, pthreads, WebGL2 `gl_context_t`, WebSocket in vio | per item |

## Open questions

- **G45's 16 MB of prims per frame.** `wgpuQueueWriteBuffer` in the browser copies out of wasm
  memory to the GPU process. If `ui_chips` misses its frame rate there, the planned work of
  uploading only changed chunks comes first.
- **The vertex attribute limit.** WebGPU's default `maxVertexAttributes` is 16, and the prim layout
  uses 6, so this is fine now. It stays something to watch as the prim grows.
