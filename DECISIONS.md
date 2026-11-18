# Decisions

Each entry is a choice made while building vui, and why. `PLAN.md` describes the design.

## G1. The library lives in the vio repository

It needs hooks in the loop core (G2) that change with it, and one CI run then checks both. It is
its own target, `vui`, in its own directory, so it can move to its own repository once the
core hooks settle.

## G2. A host wait, not a host loop

The loop keeps control of the turn. On macOS and Windows a *host wait* replaces only the blocking
step. The other way round, where the window system's loop runs and calls vio, would make the turn
order depend on the platform, and every vio guarantee about it would need restating per platform.

## G3. Nested turns run inside modal loops

The open question in the core `DECISIONS.md` was whether coroutines should run while Win32 or Cocoa
holds the thread in a resize or move loop. They do. Without it, a window being resized shows
stale content, and I/O stalls for as long as the user holds the mouse button. `run_nested()` skips
each stage that is already running further up the stack, so no queue is walked twice at once.

## G4. Callbacks record, a hook resumes

Window-system callbacks only queue events. A before-block hook resumes the waiters. A coroutine
that closes its window then cannot free the window while Xlib, libwayland, AppKit or a window
procedure is still running on it.

## G5. Frames are requested, not pushed

Each await of `frame()` asks for one frame. A program that is idle draws nothing and the loop
sleeps. This matches `wl_surface.frame` and `requestAnimationFrame`.

## G6. Graphics APIs are loaded at run time

EGL, GL, GLX and Vulkan are loaded with `dlopen` (or `LoadLibrary`) on first use. A program that
draws with Vulkan does not need libGL, and one that only opens windows needs neither.

## G7. Window systems are linked on Linux

libX11 and libwayland-client are linked. Both are present on any desktop that can show a window,
and loading them at run time would add a table of some two hundred function pointers for no
gain. The backend is chosen at run time: Wayland when `WAYLAND_DISPLAY` is set, X11 otherwise,
and `VUI_BACKEND=x11|wayland` overrides.

## G8. EGL for OpenGL on Linux

EGL works on X11 and Wayland alike, so one code path covers both. GLX would serve only X11.

## G9. Sizes are in the window system's own units

`size` is pixels on X11 and Windows, and logical units on Wayland and macOS. `framebuffer` is
always pixels, and `scale` is the factor between them, or the user's UI scale where the two are the
same (X11's `Xft.dpi`). Render at `frame.framebuffer`; lay out at `frame.size`. GLFW draws the
same line, and anything else would mean inventing a unit one of the four systems does not have.

## G10. A context is released when its window closes

A GL context and its surface hold the native window. When the window closes, every attachment is
told first and drops its native objects; later calls answer `errc::canceled`. Otherwise closing a
window before its context would leave EGL holding a destroyed `wl_surface` or X window.

## G11. EGL is loaded once and never unloaded

Mesa's drivers keep thread-locals and register exit handlers. Unloading them while those are live
crashes at exit on some versions. Keeping the library also lets LeakSanitizer attribute what the
driver allocates, and with it loaded the suite reports no leaks.

## G12. Wayland swaps with interval zero

The frame callback already paces a Wayland window. An EGL swap interval of one would make
`eglSwapBuffers` wait for a second callback of its own and block the loop thread meanwhile. On X11
the interval follows `gl_config_t::vsync`, since nothing else there knows where vertical blank is.

## G13. Wayland frames need a commit

`frame()` on Wayland resolves from a `wl_surface.frame` callback asked for when the previous frame
was handed out. The compositor answers only once that frame is committed, so a program that awaits
frames must present each one (`swap_buffers`, a Vulkan present, or its own commit). A window the
compositor hides gets no callbacks and so no frames, which is the point.

## G14. A loop timer paces frames on X11, Windows and macOS

Only Wayland tells a client when to draw. Elsewhere a timer on the loop fires at the display's
refresh rate, aligned to the clock, so windows that share a rate tick together. A display link
thread (`CVDisplayLink`, `DwmFlush`, the X Present extension) would be closer to vertical blank but
would add a thread per display and a post per frame. The swap interval still holds the picture to
vertical blank where the driver supports it.

## G15. One host wait per thread

Windows and macOS deliver window messages per thread, not per window, so one host wait serves
every display on a loop. A second display shares it, and a display on another loop on the same
thread is refused with `errc::busy`.

## G16. Windows: a forwarder thread puts completions back

An I/O completion port cannot be waited on together with the message queue. The forwarder thread
takes one batch of completions when the loop is about to wait, posts each straight back to the
port, and posts a message to a hidden window, which ends `MsgWaitForMultipleObjectsEx`. It takes
only one batch per wait, so it never spins while the loop is busy. Electron drives libuv the same
way. The cost is one thread and the reordering of completions within a batch, which nothing in
vio depends on.

## G17. Inside a modal loop, deadlines keep the window system's timer resolution

During a Win32 size or move loop, vio's own deadlines arrive through `WM_TIMER`, which the system
delivers at roughly 16 ms; I/O still wakes a turn at once through the forwarder. A 5 ms sleep
therefore runs at about 60 Hz while the user drags a border, and at full speed again afterwards.
On macOS the deadline timer is a `CFRunLoopTimer` in the common modes, re-armed before every wait,
since vio's timers are not kqueue events and would not otherwise end a tracking loop's sleep.
Core Foundation never fires a timer or source again while its own callout is running, and a
nested turn can itself run inside one, because AppKit spins the run loop during ordinary calls.
The deadline is therefore a fresh one-shot timer each time it changes, never one re-armed in place:
a long-interval timer moved with `CFRunLoopTimerSetNextFireDate` from the before-waiting observer
never fired during tracking on CI, while a new timer does. The observer also fires it at once when
the kqueue has events while the descriptor source is the callout on the stack.

## G18. GL and Vulkan tests run without leak detection

Mesa loads and unloads driver modules while it probes for a device. LeakSanitizer then reports
driver allocations whose frames it can no longer name, and the report changes from run to run. The
Linux script runs every other test with leak detection on, and the GL and Vulkan cases in a second
pass with it off.

## G19. Window events are subscriptions, and subscriptions broadcast

`window_t::subscribe<Ts...>()` yields only the listed event types: `optional<T>` for one,
`optional<variant<Ts...>>` for several, everything for none. A typed await on one shared queue
would have to either discard what it skips, which steals it from other readers, or leave it there
to fill the queue. So each subscription has its own queue, registered before the first await, and
the window copies every event into each subscription that names its type. Nothing is consumed
from anyone else. Subscriptions are the only way to read input: the shared queue behind the old
`input()` was removed rather than kept beside them, because it duplicated the queueing and
delivery code and stored every event for a reader that might never come. An event of a type no
subscription names is therefore not stored at all. Order holds within a
subscription and not across two, which is why one subscription can name several types. The end of
the window is the end of the stream, not an error: an await yields `nullopt`, so
`while (auto e = co_await sub)` stops exactly then. Each subscription bounds its queue at
`input_queue_limit`, merges pointer moves and counts its own drops.

Resizes and close requests went the same way, as `resize_event_t` and `close_request_t` in
`window_event_t`. `resized()` and `close_requested()` had the defect `input()` had: one waiter took
each resize or request, so a renderer and a layout coroutine awaiting the same window each missed
what the other got. Neither needs a backlog -- only the latest size and the fact of a request
matter -- so a resize or close request that arrives straight after another of its kind replaces it,
the rule pointer moves already had. Being in the same variant also orders them against input, which
is what a dialog waiting for Escape or the close button wants. A subscription made after the
window's first size does not see it; `size()` answers that without a stream.

`frame()` stays an await, because awaiting a frame is what asks for one (`frame_wanted`): a
subscription would want frames for as long as it lived, which makes on-demand rendering
continuous, and a queued frame is a stale one. It already hands the same frame to every waiter of a
turn, so it never had the splitting defect. It now yields `std::optional<frame_t>`: its only error
was `canceled` on close, which is what `nullopt` means everywhere else, so every await in the
library ends the same way.

## G20. The UI beat is the loop's deferred queue

Everything the toolkit resumes -- a behaviour woken by a press, the frame loop woken by a change --
waits on the runtime's ready list and is resumed from one posted node on the loop's deferred queue,
so nothing runs on the stack of whatever recorded the event. The deferred queue rather than a
before-block hook, because a hook that runs before the display's pump can miss work the pump makes
ready and the loop then blocks with it pending; deferring a node instead makes the next wait return
at once. Frames are destroyed there too, never inside their own `final_suspend` or inside
`cancel()`. The one resumption that destroys the runtime -- the end of `ui::run`, once every scope
has drained -- goes through a second node of its own, so the beat that triggered it has returned
before the runtime goes.

## G21. Slots are keyed by address and type

A component's identity is its address, but a component and its first member share one, so the slot
map is keyed by the address and the view thunk together. No component can contain a component of
its own type at offset zero, so the pair is unique. Unmounting and mounting the same member again
makes a fresh slot: its behaviours start over, its state stays.

## G22. Cancellation is passed, not attached

`await_transform` cannot give an operation a `cancellation_t`: vio's one-shot operations take
the token as an argument when they are created, before the wrapper sees them, and readers have a
`cancel()` method instead. So a behaviour passes `scope.token()`, binds a reader with
`scope.bind()`, and the wrapper's suspension-point check is the backstop that makes
`while (true) co_await sleep(...)` end without either. The toolkit's own subscriptions are the
exception: the wrapper registers a callback that wakes them with `nullopt`.

Some operations resume their coroutine from inside `cancel()` (sleep does; the QUIC operations
latch and resume in their own beat), so a cancelled behaviour can run on the canceller's stack. It
only ever runs to its next `co_await`, where it is refused and buried, so nothing is freed under
whoever called `cancel()`. A frame parked inside an operation that took no token is never destroyed
-- the operation holds its handle -- and delays its component's release until the operation
completes.

## G23. The first renderer clears scissor rectangles

Every box is a `glScissor` and a `glClear`: no shaders, no buffers, nothing linked, only the
`proc<>` lookups. It draws coloured rectangles and nothing else, which is all phase 6 has. Layout
produces a flat list of `draw_item_t`, which is also what `run_config_t::on_layout` hands tests,
so a real renderer replaces this one behind the same list. Without a GL context (the null backend)
nothing is drawn and everything else runs.

Known gap: a behaviour that throws terminates, as `task_t` does, rather than rethrowing out of
`ui::run`. (The other gap recorded here, an expose with no change going unpainted, is closed by
G36. This renderer is now the GL fallback of G32 and reads the display list of G24.)

## G24. The renderer reads a display list, not the hit-test list

`draw_item_t` is one entry per box because hit testing needs exactly that. A renderer needs
something else: a label is one box and dozens of glyphs, a plot is one box and thousands of
segments. Folding those into `draw_item_t` would make every pointer move walk them. So layout emits
two lists, and the component layer is unchanged by either.

## G25. Shaders are Slang, compiled offline and committed

One source gives SPIR-V, MSL and DXIL, and GLSL or WGSL if a GL or WebGPU backend is ever wanted.
Compiled at build time, never at run time: the Slang runtime is large and a UI has two or three
shaders. The outputs are committed so that building vio does not need `slangc` on `PATH`, a
requirement BoringSSL already makes of Go and NASM and which is not worth repeating for three
shaders.

## G26. Shapes are analytic distance functions, not tessellation

Rounded rects, borders, shadows, circles and arcs are one instanced quad each, with the exact
distance to the shape computed in the fragment shader and turned into coverage with `fwidth`.
Tessellation shaders were the alternative and lose on every count: D3D12, Vulkan and Metal each
model tessellation differently (Metal through compute), the output is triangles that still need
MSAA to look right, and it is a whole pipeline stage for primitives a few hundred pixels across.
These distances are exact, not sampled from a texture, so they have none of the softness of
distance-field text.

## G27. Text is FreeType and HarfBuzz into a coverage atlas

Signed distance field text was considered and rejected: at UI sizes (11-16 px) it is visibly
softer than a glyph rasterized at its size, a single-channel field rounds corners, and it has no
hinting. Its strength, one atlas entry for every size, matters for zoomable or transformed text,
which a UI rarely has. FreeType gives hinting and an outline rasterizer that also renders icons;
HarfBuzz gives shaping, without which only Latin text is right. FreeType is built with
`FT_DISABLE_HARFBUZZ=ON` so the two do not depend on each other. Both are dependencies of
`vui_ui` only, through cmake-dep with the usual `VIO_USE_SYSTEM_*` toggles; neither the vio
core nor the window layer sees them. Blending is in linear space, because without it light text on
dark is too thin and dark text on light too heavy.

## G28. Plot lines are joined segments, and large series are decimated

A polyline drawn as independent capsules looks right only when opaque: translucent lines blend twice
where neighbours overlap and bead at every point. So each segment reads its neighbours from the
point buffer and the vertex shader builds a miter join, bevelled past a limit, with no overlap.
Series are uploaded relative to an origin subtracted in `double`, because `float32` cannot hold a
timestamp axis and zooming into one jitters. Past one point per pixel column, M4 decimation keeps
the first, last, minimum and maximum of each column, which is pixel-identical for a line plot and
turns a million points into a few thousand segments.

## G29. Updates are cached per component on the CPU, rebuilt whole on the GPU

The whole window is rendered every frame, so the saving is not in drawing less. It is in not
producing what has not changed, because `view()`, layout and text shaping are the expensive part
of a frame. Each slot keeps its own commands, in its own coordinates, and a measured size with the
constraints behind it. A change marks `subtree_dirty` upward, stopping at the first ancestor
already marked, and only marked subtrees are revisited. It is a separate flag from `view_dirty`:
marking a parent view-dirty would re-run user code for every change below it.

Commands are grouped in chunks rather than kept one list per slot. Most components produce a few
primitives each, and assembling a frame from thousands of tiny copies would pay per-copy overhead
on all of them. So a subtree below a threshold flattens into its ancestor's chunk, with hysteresis
so one near the limit does not switch every frame. Flattened copies at *every* level were rejected:
memory grows with depth, a leaf change re-flattens every ancestor, and the root's copy is the whole
window anyway. With chunks every command lives in exactly one.

What the threshold does *not* bound, as built: a holder's chunk holds its own commands and those
of every descendant too small to hold one, so a parent of a thousand 100-primitive children holds
one chunk of 100,000 and flattens all of it when any child changes. And a holder that is marked
only because a nested holder below it changed flattens again although none of its own content did.
Both are bounded by what the window shows, both are cheap next to running views, and neither is
fixed: the first wants a holder that grows past a multiple of the threshold to promote its largest
inlined children; the second wants a third flag, set only by changes that land in the holder's own
chunk.

The instance buffer is concatenated afresh every frame rather than patched in place. Patching
would need an allocator over the buffer and a plan for chunks that change length, to save a copy
of tens of microseconds. Offsets are applied in the shader from each run's origin (G34), so that
copy is a plain `memcpy` and moving costs one origin. Boundaries, which hold their own GPU buffers,
are for the subtrees large enough that even the copy matters. Data that is large in itself -- the
atlas, plot series, heatmaps -- lives in its own GPU resources and is never part of any copy.

## G30. Vulkan is the renderer's first backend

It is the one backend that can be tested everywhere the window layer is: lavapipe runs it in Linux
CI under Xvfb and weston, as phase 3 already does for surfaces, and the same code runs on Windows.
D3D12 would be testable only on Windows and Metal only on macOS. It also sidesteps WGL, whose
context creation fails on the Windows machine the toolkit was first run on. D3D12 and Metal follow
once the display list, shaders and text are settled on Vulkan.

## G31. The renderer owns its Vulkan device unless the program lends one

The window layer never creates a device; a toolkit that draws has to. So by default the renderer
creates its own instance and device, picking a discrete GPU over an integrated one over anything
else (lavapipe last), and a queue family that can present to the window. A program that already
renders passes `run_config_t::vulkan_device` -- instance, physical device, device, queue, family
and its `vkGetInstanceProcAddr` -- and the renderer makes only its surface, swapchain, pipeline and
buffers on it, destroying those and nothing else. The program must have enabled the instance's
surface extensions (`vulkan::required_instance_extensions`) and the device's `VK_KHR_swapchain`,
and must not use the queue from another thread while the loop draws: Vulkan queues are externally
synchronised and the renderer submits from the loop thread. Handles cross the public header as
`void *`, so `ui.h` needs no `vulkan.h`.

## G32. The GL stopgap stays as the fallback

`render_api_t::automatic` tries Vulkan, then GL. GL is still G23's scissor-clear renderer, now
reading the display list: square corners, no borders, no shadows. It stays until Vulkan has run on
every platform CI covers, because until then it is the only thing that draws on a machine whose
Vulkan loader or driver is missing. `run_config_t::on_renderer` says which was chosen and why
Vulkan was not, since a silent fallback looks like a styling bug. Removing it is phase 12's call.

## G33. The committed SPIR-V is a stopgap from a GLSL transcription

`ui.slang` is the source of truth (G25), but the first `ui_spirv.h` was not compiled from it.
Downloading slangc was refused in the session that wrote the renderer, and nothing installed on the
machine compiled Slang. RenderDoc's `glslangValidator` 16.2.0 was installed, so the two entry
points were transcribed to GLSL by hand -- same attribute locations, same push-constant block, same
arithmetic -- and compiled with that; the transcription is not in the tree. The header says so in
its first lines. Regenerate it with `cmake --build <dir> --target vui_shaders` once slangc is
on `PATH`, then run `test_ui_render.cpp`: the offscreen test reads back pixels and is what proves
the Slang output matches. The renderer reads each module's entry point name out of its
`OpEntryPoint`, so slangc keeping `vertexMain` where glslang writes `main` needs no change. The
target builds for `spirv_1_0` because the renderer asks for Vulkan 1.0.

**Resolved.** `-DVUI_FETCH_SLANG=ON` fetches slangc 2026.21's release archive for the host
through cmake-dep (one package per host system and processor, since the archives are prebuilt;
off by default, so neither the lockfile nor CI fetches it), and `ui_spirv.h` is now its output.
It found one difference the transcription hid: Slang's `SV_VertexID` is zero-based as in
Direct3D, so it subtracts `BaseVertex`, which needs `DrawParameters` and silently raised the
vertex module to SPIR-V 1.3 -- invalid on a Vulkan 1.0 device. The shader uses
`SV_VulkanVertexID` instead; every draw starts at vertex 0, so the value is the same. slangc
still warns that 1.0 is "too old"; it is advisory and both modules are 1.0. The tests and the
counter example draw the same through it as through the stopgap.

## G34. A run's origin is a push constant, not an index into a buffer

G29 planned a buffer of chunk origins indexed per instance. One `vkCmdDraw` per run with the origin
pushed does the same job without a descriptor set, a second buffer or a field in every primitive,
and the number of runs is the number of chunks, roughly primitives / threshold -- dozens, not
thousands. The cost is a draw call per run. When the glyph atlas brings descriptor sets in phase 10,
or a profile shows draw calls, the buffer is the change to make; the display list already carries
everything it needs.

## G35. Layout is in logical units, on every platform

`frame.framebuffer / frame.scale` is the window's size on Wayland and macOS and its size divided by
the UI scale on Windows and X11 (G9), so one expression gives the same unit everywhere. Before
this, layout used `frame.size`, which is pixels on Windows and X11 -- a 60-unit box
was 60 pixels at any DPI. Pointer positions arrive in the window system's units and are scaled by
logical / size before hit testing. Sizes stay integers; the renderer scales by framebuffer /
logical, per axis, in floating point. Only the null backend (scale 1) is under test, so the
conversion itself has been seen working only in the counter example at 144 DPI.

## G36. An expose is an event in the window layer

The frame loop sleeps in `changed()` and a window's `damaged` flag reached nobody until something
else asked for a frame -- the G23 gap. `expose_event_t` joins `window_event_t`, emitted when the
window system reports damage and when a hidden window is shown again, so a subscriber learns that
its contents were lost. It is an event rather than a flag on `frame()` because frames are only
delivered when asked for (G5): a program waiting for a reason to draw needs to be told. The UI
subscribes to it beside `resize_event_t`; the frame that follows runs no view and reuses every
chunk, which `test_ui_render.cpp` checks.

## G37. A shadow is the Gaussian of the distance, not the exact convolution

The shadow's coverage is `0.5 - 0.5 erf(d / (sqrt(2) sigma))` of the same rounded-box distance the
box itself uses. It is exact along straight edges and an approximation at corners, where the true
convolution of a rounded rectangle is slightly darker. Evan Wallace's closed form integrates the
corners properly but is several times the code; at UI shadow sizes the difference is not visible.
The quad grows by three sigmas so the tail is not clipped.

## G38. Colours are sRGB as authored, blended in linear space

`color_t` values are what a designer types -- sRGB, straight alpha. The vertex shader converts to
linear and premultiplies; blending is `ONE, ONE_MINUS_SRC_ALPHA`; the swapchain is an `_SRGB` format
so the hardware encodes on write. Where a surface offers no sRGB format the shader encodes itself
(`encode_srgb` in the push constants). The clear colour is converted on the CPU to match. This is
what G27 asks of text, set up before text arrives.

## G39. Glyphs and icons are coverage prims into one R8 atlas, rounded at a shared anchor

A coverage prim is a rectangle of the atlas copied texel for texel: no sampler, no filtering, one
texel per pixel. That holds only if the rectangle lands on whole pixels, and layout cannot place it
there itself -- a run's origin and a non-holder's offset are logical units, which at 1.5x are half
pixels. So a glyph carries a logical anchor (its text node's position) and an integer pixel offset
from it, and the vertex shader rounds the anchor. Every glyph of a text shares the anchor and rounds
the same way, so HarfBuzz's spacing is kept exactly; rounding each glyph would shift them by
different amounts. The subpixel offset (one of four) is chosen from the pen position relative to the
anchor, which is what the rounding preserves.

FreeType renders with light hinting -- vertical only -- because HarfBuzz's advances are unhinted and
full hinting would change widths under them. Both are fetched by cmake-dep: FreeType 2.15.0 with
HarfBuzz, zlib, bzip2, PNG and Brotli off; HarfBuzz 15.1.0 compiled from its `harfbuzz.cc`
amalgamation with its own OpenType font functions, so neither sees the other. Both are private to
`vui_ui`.

The atlas is shelf-packed, starts at 512 square and doubles to 4096; full at that size it is emptied
and the frame lays everything out again once, since what was laid out before names rectangles that
are gone. A new scale empties it too, because every glyph's pixel size changes. One atlas per
runtime: FreeType faces are not thread-safe and a window's loop owns its runtime, so nothing is
locked. `font_t` itself -- the bytes and an `hb_face_t` -- is immutable and shared.

Uploads are the dirty rectangle since the previous display list, through a per-frame staging buffer,
before the render pass. The barrier after the copy waits on all commands, not only transfer, and
that is empirical: on an RX 5700 XT with a frame in flight, a copy whose rows were not 4-byte
aligned was not there when the same frame's fragments read it, so new glyphs (the clock's digits in
the counter example) did not appear. Waiting for idle after each frame, aligning the copy, or
widening the barrier's source stage each fixed it; the offscreen test, which waits between frames,
never showed it. The spec reading is that `TRANSFER -> FRAGMENT_SHADER` suffices; no validation layer
was available to say otherwise, so treat this as a driver workaround until one does. It costs
nothing measurable: it runs only on frames that upload.

## G40. A text node is one line, one font, one direction

`text_t` is shaped as a single HarfBuzz run whose direction, script and language HarfBuzz guesses
from the text. A line of Arabic, Hebrew or Devanagari is right; a line mixing directions is not,
because that needs the Unicode bidirectional algorithm to split it into runs first. Missing glyphs
draw as the font's .notdef: there is no fallback list. No wrapping, and a row aligns tops, not
baselines. Each of those is real work with a library or an algorithm behind it (SheenBidi or
fribidi, fontconfig or a fallback table, line breaking per UAX 14), and none is needed to put a
label on a UI. The default font is found by path -- Segoe UI, the macOS system font, DejaVu, Noto or
Liberation Sans -- rather than through fontconfig, which would be a third dependency loaded at run
time for one lookup; `run_config_t::font` overrides it. Colour glyphs (emoji) are not drawn: the
atlas is one channel.

## G41. Layout is one pass with padding and alignment; plots start as segments and areas

Layout stays content-sized and single-pass: a container places each child at its start position,
learns its size from doing so, and only then moves it -- across for alignment, down for a baseline
-- by shifting the entries the child produced. No constraints travel down, so nothing yet grows to
fill a row (flex); that is a second pass and waits for a layout that needs it. Positions became
`float`, because two things need fractions: a plot's bars move by fractions of a unit as they
scroll, and baselines do. A text's baseline is a whole number of pixels below its anchor, which the
shader rounds; moving the anchor by exactly the difference of two baselines in logical units --
fractional at 1.5x -- therefore lands both on the same pixel row, where rounding the move to whole
units missed by one. Hit testing rounds positions; drawing keeps them.

A box with children holds them overlapping, each aligned inside its padding, rather than as a
column: one child is the common case (a label on a button), and a column inside is one more node.
`at_t` exists for what alignment cannot say -- a bar at a time-dependent x.

Plots begin inside the existing pipeline, ahead of phase 11's point buffer (G28). `polyline_t`
emits one segment prim per segment -- the distance to the segment, so ends and joins are round --
one area prim per segment of fill, and one circle per marker. Round joins overlap, so a translucent
line darkens at every point; G28's joined segments remove that, and until then the example's lines
are opaque. A line is rebuilt with its component, which at a few thousand prims is cheap.

Areas meet edge to edge, and that edge is not the rasterizer's to decide. The first version made
each area's quad exactly x0 to x1 and relied on shared edges rasterizing once; the offscreen test
of one shared edge at 2x passed, and the running example at 1.5x showed stripes where the fill was
blended twice. SPIR-V lets the driver contract and reorder vertex arithmetic per vertex, so two
vertices computed from the same float need not land on the same position. The quad is now a pixel
wider on each side, and the fragment shader keeps a pixel only if its centre, mapped back to the
run's logical x, lies in [x0, x1) -- bounds passed through from the attributes with no arithmetic,
compared against a value computed the same way for that pixel in either prim, so exactly one of
two neighbours keeps it. A test of three hundred thin areas at 1.5x on a fractional origin failed
on 174 pixels of one row before that, and passes after.

Centring a text centres its line -- ascent plus descent -- as CSS does, and a "+" or "-" sits on
the font's maths axis, below the middle of that line: in Segoe UI the example's button labels were
5.5 px low at 1.5x. `text_t::tight` measures a text by its ink instead, moving its glyphs by whole
pixels so they stay texel-exact; it is opt-in, because a line of text must keep its line's height
or neighbouring lines of different content would not match. A centred offset is also snapped to the
nearest pixel rather than floored to a whole unit, which at 1.5x is worth up to three quarters of a
pixel. The labels are now within half a pixel of centre.

## G42. Flex is measure-then-place where space is handed out; lists own by shared pointer

Flex needs a child's natural size before the child is placed, which a one-pass layout cannot give.
Rather than a second, separate measuring function per node type -- which would have to agree with
placing forever -- measuring is placing and taking it back: the entries are truncated and the
counters restored, and the result is cached for the frame by the node's address, so nesting does
not multiply it. Only a container that hands out space (a child grows or stretches, or it was given
a size itself) measures; everything else is still placed once. A mounted component is laid out for
the size it is given and keeps its natural size from its last unconstrained layout, so a clean
child is measured for free. The root is given the window, which is what lets a page fill it.

`list_t<T>` holds its items by `shared_ptr` and its mounts share that ownership with the slot: an
erased item's behaviours still hold `this` until they reach their next await, so the item is freed
when the slot is reaped, not at `erase()`. Props are carried type-erased on `mount_t`, compared each
frame and copied into the child only when they differ, which is what rebuilds it. A child scope is
cancelled through a callback on its parent's token, and its owner must join it; that await, and
awaiting a spawned job, are exempt from the refusal a cancelled scope gives every other await,
because both are bounded by the cancellation itself and refusing them would end the owner while
what it owns still runs. `job_t<T>` keeps its result outside its frame, since the scope destroys the
frame a beat after it ends.

## G43. A merge is one waiter watching several queues

`ui::merge(scope, a, b, c)` does not put a waiter into each subscription's queue: several waiters for
one coroutine could each be readied in the same turn and resume it twice, and whichever lost would
have taken an event out of its queue for nobody. Instead each queue gets a watcher pointer. An event
that no direct awaiter takes is queued as before and readies the watcher, at most once -- a waiter
already readied is skipped -- and the merge, when it runs, stops watching every queue and takes from
the first non-empty one, starting after the one it took from last so a busy source cannot starve a
quiet one. Events are never removed from a queue until the merge consumes them, so none is lost or
seen twice. The result is a variant indexed by argument position, so two subscriptions of one event
type stay distinguishable.

`ui::every` and `ui::lines` are behaviours in the scope feeding a source the subscription reads.
They mark themselves quiet: their own wake-ups do not rebuild the component, which a ticker at
16 ms would otherwise do every period whether or not anything consumed the tick. A stream's end is
an event (`line_t::closed`), not the source going away, which would leave a waiting merge waiting.
Tested against a real loopback TCP connection with a line split across writes and a CRLF.

## G44. Plot lines are mitred segments in the one pipeline; heatmaps live in the atlas

G28 planned a point buffer read by the vertex shader, a pan-and-zoom uniform, triangle strips for
fills and an R32F texture for heatmaps. What was built keeps everything in the instanced-prim
pipeline the rest of the toolkit uses, because each of those would have been a second pipeline,
buffer or descriptor for one feature, and the prim was enough:

- **Joins.** A segment carries, at each end, the plane its neighbour shares: through the joint,
  square to the sum of the two directions, which is the mitre line. The CPU writes the same floats
  into both segments; the shader tests the pixel against it in logical units with `precise`
  arithmetic (SPIR-V `NoContraction`), so the two compute the same answer and exactly one draws the
  pixel -- the lesson of the area seams in G41, applied before the stripes showed this time. Inside
  its planes a segment is a band square to its line, which is the mitre; a zero normal is a round
  end, used at the line's ends and past a mitre limit of 4, where a mitre would spike. A test
  renders a half-transparent zigzag and finds no pixel blended twice; with the joins turned round
  it finds some.
- **Decimation** is M4 on the CPU where the line is placed: per pixel column the first, lowest,
  highest and last point. A million points become about four hundred segments, and the extremes
  survive (a one-point spike is tested for). Lines whose x goes backwards are not decimated.
- **Heatmaps** are quantized to 256 levels into a region of the R8 glyph atlas, keyed by the data's
  address and re-quantized only when its `version` or range changes, so an updating spectrogram
  uploads only its own rectangle through the dirty-rect path G39 already had. The colormap is a
  polynomial fit of viridis in the shader. 256 levels is below what a colormap can show; the cost
  is that a heatmap's region is never freed until the atlas fills, and that a heatmap is at most
  4096 cells a side.

Not built: pan and zoom as a uniform. A plot that pans rebuilds its prims, which at a few thousand
per plot after decimation is cheap; a million-point series is decimated per rebuild, which costs
the walk over a million points.

## G45. What a 200,000-chip scene needed: mount lists, culling, and zoom as a run scale

The chip stress test (PLAN.md, "Stress test") was measured first and changed after, and each change
is what a profile of that build pointed at:

- **A clean view's mounts are kept, not searched for.** Finding the mounted children of a component
  walked its whole cached node tree every frame, so a scene whose 400 tiles had not changed still
  walked 200,000 chip nodes: 8 ms a frame of panning with no view run. Each slot now records its
  mounts when its view runs. Panning everything went from 18.6 to 4.0 ms.
- **Hit testing collects only what a press can be aimed at,** and skips subtrees with none, unless
  `on_layout` asks for every box; the chips have no pressables, so their 200,000 boxes cost nothing.
- **Chunks and hit-test items wholly outside the window are skipped** (with a margin for shadows). A
  chunk skipped while it changed is flattened again when it next comes into view, and so is every
  holder below it on the changed path, since the per-frame dirty marks are cleared either way.
- **Zoom is a scale on the run, applied by the GPU.** `mount_t::scale` draws a component that many
  times its layout size without laying it out again; positions, sizes, radii, borders and line
  widths scale in the shader, glyphs keep their pixel size and move with their anchor (they are
  texel-exact), and layout and hit testing see the scaled size. A scaled mount is always a holder,
  since the scale belongs to a run. The scene lays tiles out at zoom steps a factor of the square
  root of two apart and scales between them, so a zoom gesture rebuilds nothing until it crosses a
  step: zooming with everything in view went from 81 to 6.3 ms. This is the "pan and zoom as a
  uniform" G28 planned, at the level of a mount rather than a plot.
- **`run_config_t::on_stats`** reports each frame's cost by phase, which is how all of the above
  was found.

The stress test also found a shutdown race the examples had always had: the runtime read the event
loop through the window, and a behaviour woken by its own timer after the window closed -- before
the frame loop noticed -- asked for the loop through a window that no longer had one. The runtime
now keeps the loop from construction. `vui_ui_counter`'s timed exit failed about two runs in
five before and none in twenty after; a unit test on the null backend did not reproduce it, so
there is none.

## G46. Widgets are controlled components; focus and popups are the runtime's

`vui/ui/widgets.h` has the first widgets: `button_t`, `checkbox_t` and `combobox_t`. Each is an
ordinary component built only from what `ui.h` offers any program, which keeps them honest as a
worked example and kept the toolkit changes to the two things a combobox could not do without.

- **Controlled, not self-owned.** A widget's value comes down as props and a change comes up as an
  event (`toggle_event_t`, `select_event_t`) on a public `event_source_t` member, which the parent
  applies to its own state. PLAN.md's rule that a view reads only its own component already forces
  this: a parent reading `box.checked` would not be rebuilt when it changed. It also makes setting
  a value from code nothing more than assigning the parent's member. What the widget keeps is only
  what no parent cares about: whether the menu is open, the keyboard's row. The cost is a frame of
  latency between asking and being told, so two presses in one loop turn both read the old props;
  a widget remembers what it last asked for until its next view runs, which is after the parent
  answered. The test of two arrow keys in one turn is what found this.
- **Focus is a property of a pressable**, `pressable_t{ui::focusable}`. A press focuses it; Tab and
  Shift+Tab walk the focusable boxes in hit-test order, which is drawing order and so the order a
  view lists them in. The focused pressable is also a `source_t<key_event_t>` and is sent every key,
  Tab included, before focus moves -- a combobox closes on it. Space holds and then presses it, so a
  widget that reacts to presses works from the keyboard without code of its own. `focus_visible()`
  is set by a key and cleared by a click, for a ring only keyboard users see. Focus is dropped when
  its box leaves the hit-test list, so a disabled button cannot be pressed with Space.
- **A popup is a mount flag**, `ui::mount(menu).on_top(dismiss)`. It is placed where its parent puts
  it but takes no room there, always holds its own chunk, and is drawn and hit-tested after
  everything else, moved to fit the window. With a dismiss source, a whole-window hit-test entry
  sits under it, so a press outside reaches that source and nothing under it. Being a mount means
  the popup is its own component: the combobox creates its menu when it opens and drops it when it
  closes, shared through `mount_t::keep`, so the menu's row pressables outlive the frame in which
  the runtime still has them as hovered.
- **The keyboard's row and the pointer's are kept apart** in the menu: accent for the row Enter
  picks, a tint for the one under the pointer. Letting hover move the keyboard's row made a pointer
  that happened to rest where the menu opened take over the arrow keys.
- **A combobox is as wide as its widest option**, by drawing every option and its placeholder
  invisibly under the one shown, so it does not change width with the choice. The menu does the
  same with the placeholder; its padding adds up to the field's so both are the same width.

Not yet: a theme as props or inherited (one dark `theme_t` for now), type-to-search in a combobox,
flipping a menu above its field when there is no room below (it is moved up into the window
instead), focus that follows the pointer into a menu, and text input.

## G47. A live resize draws before WM_SIZE returns; what is left is the driver's

Content trailed the border while a window was dragged larger on Windows. Traced on an RX 5700 XT,
Release, dragging `vui_ui_widgets` by its right edge:

- **Every other resize was drawn a refresh late.** `WM_SIZE` ran one nested turn, but a UI's frame
  loop waits on its own change signal, which wakes through the loop's deferred queue, and a turn
  drains that queue before the hook that hands out frames. So the frame for a new size was often
  only reached on the next turn -- the next `WM_TIMER`, about 16 ms later -- after Windows had
  already shown the window at that size. 50 of 80 frames were drawn inside `WM_SIZE`. Now
  `WM_SIZE` and `WM_PAINT` run turns until a frame has been taken or none is due, at most three:
  73 of 74, the other being the first frame.
- **A resize costs a new swapchain, and that is the driver's time:** `vkCreateSwapchainKHR` took a
  median 8.4 ms, against under 0.3 ms for everything else in a frame. Vulkan on Win32 requires the
  image size to equal the window's, so there is no reusing a larger one. Asking for the minimum
  image count made creation 5.5 ms but acquire then waited for vertical blank, 15 ms in all.
- **Present mode stays FIFO.** IMMEDIATE and MAILBOX (the latter not offered by this driver)
  showed more stretched frames mid-drag, not fewer.

What is left: a frame presented through a Vulkan swapchain reaches the screen on its own schedule,
not in the same composition as the border Windows moves, and while it is late Windows shows the
previous image stretched to the new size. `VK_EXT_swapchain_maintenance1` can ask for that image
to be shown unscaled at the top left instead, which is how a GDI window looks while it is resized;
this driver does not offer it. Drawing in step with the border needs DXGI with DirectComposition,
the D3D12 backend of phase 12. Mid-drag screen captures did not separate the builds before and
after the first fix -- a GDI capture does not sample what DWM shows at a given refresh -- so the
evidence for it is the trace above, not a picture.

## G48. The gui uses vio's public API only

The gui used to reach into `vio::detail` for six things: watching the display server's descriptor,
handing the blocking step to a Win32 or Cocoa message pump, before-block hooks, the deferred queue,
intrusive timers and the loop clock. Two of those had public equivalents (`timer_t`, `loop_now`).
The other four were capabilities vio did not expose, and are public now: `fd_watch_t` and
`host_loop.h` (core D57), `before_block_hook_t` and `deferred_call_t` (core D58). The rule matters
because the gui is moving to a repository of its own, where an internal header is one vio may
change without notice.

Display teardown is shorter for it. The gui's private fd watch was owned by the display, so the
display could not be freed until the watch's close had finished on a later turn, and every backend
reported that through a completion. A `vio::fd_watch_t` keeps its storage until vio is done with
it, and no callback arrives once the handle is dropped, so a backend's `close()` drops its watches
and returns, and the display is freed one posted turn later.

## G49. vui is its own repository

The gui left vio in November 2026 as vui, with its history, through `git subtree split`. G48 is what
made that possible. The namespace is `vui` rather than `vio::gui`, the headers are `<vui/...>`, and
the targets are `vui` and `vui_ui`. vio is pinned by commit through cmake-dep like any other
dependency, and `-DCMDEP_SOURCE_DIR_VIO` builds against a local checkout when the two change
together. The CI image stays vio's: it already carries Xvfb, weston and every package the Linux
backends need. Decisions keep their G numbers; earlier entries that say "the gui" mean vui.

## G50. Text widgets are fixed-width, and the runtime learns to clip

A label, a line edit and a text edit, the last both an input pane and a log. What each needed, and
why it is shaped as it is:

- **Fixed-width text on a grid.** `text_t::advance` places the n-th code point n advances in and
  turns ligatures off, so a column is a multiplication and a caret, a selection or the visible
  slice of a long line is exact arithmetic, never a measurement. The edits use
  `font_t::system_monospace()` and take the advance from `font_t::metrics()`. A code point is one
  cell, so wide CJK characters and combining marks are drawn one to a cell; tabs typed or pasted
  become spaces to the next stop of four, so the rule holds.
- **Clipping is a box flag.** `box_t::clip` brackets the box's children with two entries that draw
  nothing. Flattening a chunk closes a run at each, runs carry a clip rectangle in window units
  that nests by intersection, Vulkan sets a scissor per change and the GL stopgap cuts its clears.
  Hit testing cuts each box to the clips around it, so a scrolled-out row cannot be pressed. A
  clipping box stops growing to fit its children along whichever dimension its size or its parent
  decides. The cut is rectangular, so rounded corners are not clipped exactly.
- **Typed text and keys share one queue.** `ui::merge` takes turns between its subscriptions so a
  busy one cannot starve a quiet one, and that reordered a key and the text typed after it: "x"
  landed before the Enter pressed first. Rather than give up the fairness, a text input
  (`pressable_t{ui::text_input}`) is sent `typing_event_t`, keys and text in arrival order. Space
  is text to it, not a press.
- **Drag and wheel events.** A pressable is also a source of `drag_event_t` (button down on it,
  moves while held, release, with a click count for double and triple clicks) and of
  `wheel_event_t`, which goes to the topmost pressable under the pointer that has a wheel
  subscriber, so nested scrolling areas work.
- **A view sizes itself from `pressable_t::bounds()`**, its box as drawn last frame. A change
  rebuilds the owner, so a text edit learns how many lines and columns it shows one frame after a
  resize. The trap, which the first scrollbar fell into: anything placed from the bounds must not
  be able to grow the box they measure. A thumb at the right edge of the edit's padded box widened
  it by 2 units, which moved the thumb, every frame, without end. It sits in the clipping box now.
- **The line edit is controlled, the text edit is not.** A line is small and a parent that refuses
  an edit -- digits only -- is the point of controlling it. A document copied down through props
  and compared every frame costs as much as the document, so the text edit owns its lines and a
  parent reads them and changes them with `set_text`, `append` and `append_line`. Those take a
  scope because the edit's own behaviour did not wake: `ui::invalidate(scope, component)` rebuilds
  any mounted component, found by the same key a mount uses. `follow` keeps a log at its end while
  the user leaves it there, and `max_lines` drops the oldest lines.
- **The clipboard is reachable from a window**, `window_t::set_clipboard_text` and
  `clipboard_text()`, forwarding to its display: a widget has its window, not the display.

Measured on Windows at 150%: typing, Shift and Ctrl selection, the clipboard, drag selection, the
wheel and the scrollbar in `vui_ui_text`. Not yet: undo, input method composition (only committed
text arrives), word wrap, a horizontal scrollbar, a blinking caret, scrolling while a drag rests
outside the edit, and a proportional-font line edit.

## G51. The task manager: a demo with a system layer under it, and hover events

`examples/task_manager/` is the advanced example: CPU, per-core and memory charts, a heatmap of
every core, a process table sorted by any column and filtered as you type, a panel for the selected
process and a log of processes starting and exiting. What it decided:

- **A system layer of its own.** `system.h` is one probe with raw counters, never rates; each
  platform has a file and only one compiles. Windows takes every process with its threads, times and
  working set from one `NtQuerySystemInformation` call, which costs about 4 ms for 340 processes, and
  swap from the page file list rather than the commit charge, which counted 5 GB of a 5 GB page
  file as used when 540 MB was. Linux reads `/proc`. macOS uses mach, libproc and sysctl; another
  user's process answers only to root, so its row is marked partial rather than dropped, and
  `proc_taskinfo` times are converted from mach absolute time, which is nanoseconds only on Intel.
  `vui_task_manager --probe` checks a pair of snapshots without a window, and CI runs it on every
  platform, which is the only way the macOS file is exercised.
- **Sampling is off the loop.** A thread samples once a second and posts each snapshot through a
  `vio::event_pipe_t`; the behaviour that owns the pipe owns the thread too, declared after it, so the
  thread is joined before the pipe goes.
- **`hover_event_t`**: where the pointer is over a pressable, on every move while it is there and
  once when it leaves. A table needs the row under the pointer and a chart the sample under it, and a
  pressable per row would take the presses a focusable table needs for its keyboard. Only a source
  with a subscriber is sent one. The window keeps only the latest of the pointer moves queued in one
  turn, so a test must let each move through before sending the next.
- **Charts fill what they are given**: a chart's height is a minimum, and mounted in a box that hands
  out more, it draws to its bounds. At 150% a 1440 by 960 window is 960 by 640 units, and the first
  layout, with fixed heights, ran off the bottom.

Measured: layout was 12 ms per frame in a Debug build with 364 texts. Counting placements showed no
layout blow-up -- skipping the space handed out while measuring changed nothing -- and the time is
HarfBuzz shaping every text again whenever its component rebuilds. In Release it is 0.6 to 1.7 ms. A
cache of shaped runs is the lever if it ever matters.

## G52. A press nobody listens to falls through

In the task manager a press on a card's chart did not select the card. The chart has a pressable of
its own, for its bounds and its crosshair, and a press went to the topmost pressable under the
pointer whether or not anything wanted it. Now it goes to the topmost one that takes it: focusable,
or with something subscribed to its presses or drags -- the rule the wheel already followed (G50).
A pressable that only tracks the pointer, or only measures itself, is transparent to presses.

One limit, which the popup test caught at once: a press that lands in a popup stays in it. Falling
through may reach another box of the same popup but never its dismissing backdrop, or a press on a
menu's own unclaimed area would close the menu.

## G53. Context menus, and an explorer that opens a folder at once

The second advanced example, `examples/explorer/`, is a file explorer. It needed two things from
the toolkit and one rule from the user: opening a folder must be fast, and must not wait for the
folder to be read to the end.

- **`context_event_t`**: the right button, sent to the topmost pressable under the pointer that has
  a context subscriber -- the rule presses (G52) and the wheel follow -- and kept inside a popup,
  so nothing under an open menu opens another.
- **`context_menu_t`**: items, separators, headers and disabled items, shown by the parent as a
  popup at the pointer and closed on `chosen` or a press on `dismiss`. It does not take focus from
  what opened it, so the parent hands it the keys it receives while it is open; `key()` moves
  through the choosable items and Enter chooses. Taking focus would need focus to be settable from
  code, which the toolkit does not offer yet.
- **A shell layer** (`shell.h`, a file per platform): listing, the user's folders, drives, type
  names, the applications that open a file, and launching. Windows asks the shell's association
  handlers, which is the list Explorer shows, and invokes the chosen one the way Explorer does;
  Linux reads the `.desktop` entries for the file's MIME type with `mimeapps.list` for the default;
  macOS asks LaunchServices. Listing on Windows is `FindFirstFileExW` with a large fetch and basic
  info: one call per batch gives names, sizes, times and attributes, and nothing per file.
- **A folder streams.** `shell::list` hands over each batch as it is read, the first of 256 so the
  first rows show at once, later ones of 4096 because each costs a hand-over. It runs on a worker
  thread that a new navigation preempts: the queue is dropped and a generation counter makes the
  read in flight stop at its next batch. A folder seen lately is shown from memory at once and read
  again behind it, and the fresh listing replaces it whole, so the rows do not empty and refill.
  Type names, the "Open with" lists and previews come from a second worker, cached per extension
  and asked for only for the rows on screen or the file selected, so a slow registry never holds up
  a listing and the menu usually opens with its applications already known.
- **Sorting merges.** Rows that arrive are filtered, sorted among themselves and merged into the
  order, instead of the whole folder being sorted again for every batch. Names are folded to lower
  case once, on arrival, in ASCII: `std::tolower` consults the locale on every call. Measured on
  `C:\Windows\WinSxS`, 32,496 entries, in Release: sorting was 112 ms per pass and ran once per
  batch; it is now 9.6 ms for the last merge. The folder's first rows show 2.5 ms after Enter and
  the whole of it is read in 21 ms.

The first folder is asked for while the window and its renderer are still starting, so its time to
first rows measures start-up; the status bar says "opened at start-up" for it rather than a number
that blames the folder.

## G54. The explorer deletes, to the Recycle Bin or for good, and always asks

Delete moves the selection to the Recycle Bin (Windows) or Trash (Linux, macOS); Shift+Delete, or
"Delete permanently" in the menu, deletes it outright. Both ask first, in a dialog that says what
will happen -- the permanent one in red, saying it cannot be undone -- answered with its buttons,
Enter or Escape, and a press outside it is No. The work runs on the helper thread, since a big
folder takes a while, and the row goes when it succeeds, with the selection on the row that took
its place.

- Windows uses the shell's delete, `SHFileOperationW`, with undo allowed for the Recycle Bin and
  without for the permanent delete, which also clears read-only files that `remove_all` refuses.
- Linux follows the freedesktop.org Trash specification: the item is renamed into
  `~/.local/share/Trash/files` under a free name, with a `.trashinfo` holding its percent-encoded
  path and the time, which is what lets a file manager restore it. Something on another file system
  than the home trash is refused rather than copied. The first version made the trash with one
  `mkdir` per level and so failed on a home with no `~/.local`; the probe found it.
- macOS uses the File Manager's move to the Trash, deprecated but what Finder's Trash is, because
  `NSFileManager` would need Objective-C.
- The permanent delete elsewhere is `std::filesystem::remove_all`, which deletes a link itself and
  never follows it into what it points at.

`--probe` deletes a scratch folder permanently on every platform, and moves a file to the Trash
only with `VUI_PROBE_TRASH=1`, which CI sets: anywhere else it would leave a file in the user's own
Trash.

## G55. Images: a node, a second atlas, a sixth prim kind

vui could draw text, icons and heatmaps, all from one atlas of single-byte coverage, and nothing in
colour. `image_t` draws RGBA pixels at a size, with rounded corners and an opacity.

- **A second atlas** in the text engine, RGBA and four bytes a texel, packed in shelves like the
  glyph atlas and found again by the `image_data_t`'s address and version. It starts at 512 texels,
  doubles to 4096, and empties when it cannot grow, which the runtime turns into a second layout as
  it already does for glyphs. Each image gets a one-texel border copied from its own edge, so
  bilinear filtering at an edge never reads a neighbour.
- **The renderer has two textures**: the coverage atlas at binding 0, as before, and the image atlas
  at binding 1, `R8G8B8A8_SRGB` so that a sample is linear, read through a bilinear, clamping sampler
  at binding 2. The upload code became one `texture_t` used twice; both uploads share the frame's
  staging buffer at separate offsets, since both copies run after everything has been written.
- **Prim kind 5** maps each pixel's centre into the image's texels, kept half a texel inside so the
  filter stays on its border, samples, premultiplies and applies the rounded-corner mask the shapes
  use. `test_ui_render.cpp` draws a four-colour image offscreen and checks each quadrant, a cut corner
  and the opacity.
- Decoding is not vui's: an image arrives as pixels. What draws a file's picture is the platform
  (G57), which keeps parsers of untrusted files out of the toolkit.

## G56. Focus can be given from code

`ui::request_focus(scope, pressable)` focuses a pressable once a box showing it is drawn, and gives
up after a few frames if none is: a field that has just appeared is not on screen until the next
frame, and focusing it before then would be dropped as focus on something not shown. The explorer's
path bar needs it -- a double click turns the breadcrumbs into a field, which must have the keyboard
at once -- along with `line_edit_t::select_all()`, so the path is selected and typing replaces it.

## G57. Thumbnails come from the system, not from bundled decoders

An explorer feeds a decoder any file on disk. Image and video parsers are where security holes
live, bundling them costs megabytes, and they would still show neither a PDF nor a video. So the
explorer asks the platform, which already has the thumbnail handlers its own file manager uses:

- **Windows**: `IShellItemImageFactory` with thumbnails only, so a file with none falls back to the
  text preview instead of an icon. The bitmap comes premultiplied, or with no alpha at all for most
  photos, and is turned into straight alpha.
- **macOS**: QuickLook's `QLThumbnailImageCreate`, a C call that is deprecated but works, drawn into
  an sRGB bitmap.
- **Linux**: the freedesktop thumbnail cache, named by the MD5 of the file's URI and used when newer
  than the file; else a thumbnailer the desktop installed for the file's type, run for at most five
  seconds into a temporary file. The PNG that comes out is read with `stb_image`, PNG only, pinned
  through cmake-dep: what is decoded is a file the system's thumbnailer made, never the user's file.

Thumbnails are fetched on the helper thread with the text preview and win over it when there is one,
and the last 64 are kept by path and modification time. `--probe` draws a 64x32 BMP and asks for its
thumbnail, required on Windows and macOS, reported on Linux, where a bare container has no
thumbnailer.

## G58. The window frame follows the system's dark mode

On Windows a window's title bar is light unless the program asks DWM for the dark one, which
WezTerm and Explorer do and vui did not, so a dark UI sat under a light frame. `window_config_t`
has `frame_theme`: `system` (the default) reads the user's app mode, `AppsUseLightTheme`, and asks
again when Windows sends `ImmersiveColorSet`; `light` and `dark` force it. The attribute is
`DWMWA_USE_IMMERSIVE_DARK_MODE`, 20, or 19 on Windows 10 before 20H1. macOS follows the system
appearance by itself, and on Linux the frame is the window manager's.

The explorer also gained, at the user's asking: paths copy with forward slashes, which Windows
accepts and a shell or a CMake file wants, with "Copy path with backslashes" for the other; a path
bar that becomes a field on a double click, takes a typed or pasted path (quotes dropped, `~` for
home, either slash) and opens a file's folder with the file selected; and a context menu on the
path bar.

## G59. Archives through the tools the system has

The explorer extracts and compresses with whatever archive tools are installed, found once, best
first: 7-Zip, WinRAR and PeaZip on Windows (from the registry or Program Files) and the bsdtar that
Windows ships; 7-Zip, bsdtar, GNU tar and Info-ZIP on Linux, with File Roller and Ark as windows to
open in; the system's bsdtar on macOS, with Archive Utility, Keka and The Unarchiver. Their
command lines do the work; nothing is linked. The menu offers what those tools add to Explorer and
Finder: on an archive, Extract here and Extract to its own folder with the first tool that reads it,
and Open in each tool that has a window; on anything, Compress to each format some tool writes --
zip, 7z with 7-Zip, rar with WinRAR, tar.gz with tar -- each labelled with the tool.

- Extracting never overwrites (7-Zip `-aos`, WinRAR `-o-`, bsdtar `-k`, GNU tar `--skip-old-files`,
  unzip `-n`; GNU tar's `-k` keeps a file too but then exits with an error, which Linux caught), and a new folder
  or archive takes a free name, "name (2)" and on. 7-Zip's and WinRAR's exit code 1 is a warning --
  skipping a file that exists is one -- and counts as success.
- Compressed tars go to tar, not to 7-Zip or WinRAR: one pass of those unpacks only the compression
  and leaves a .tar where the folder should be, which the probe caught at once.
- The work runs on a thread of its own, so a long extraction does not stop previews, and the folder
  is read again afterwards with what was made selected.
- `--probe` packs a small folder into every format some installed tool writes, unpacks it with a
  tool that reads it, compares the contents, and extracts again over a changed file to see it kept.

Their own context-menu extensions -- 7-Zip's or WinRAR's submenus in Explorer -- are not hosted:
on Windows that means loading third-party shell extension DLLs into the process, which can take it
down with them, and flattening their nested menus. The same actions through the tools' command
lines give the user the same result without either.

## G60. The mouse's back and forward buttons

Every backend already reported them as `button_t::back` and `button_t::forward`; the UI dropped
them. They now become `navigate_event_t`, routed like the wheel: to the topmost pressable under the
pointer that has a subscriber, so a pager inside a window can take them before the window does. An
application that wants them anywhere puts a pressable on its outermost box, which costs nothing
else, since presses fall through a pressable nobody listens to (G52). The explorer does that, and
closes an open menu but leaves a delete question alone when one arrives.

## G61. PDF pages in the preview, from the system's renderer

The thumbnail (G57) gave a PDF a first page only where some application had registered a handler,
and never the others. Each platform carries a PDF renderer, so the explorer draws pages itself:

- **Windows:** Windows.Data.Pdf through C++/WinRT from the SDK (`windowsapp`, still nothing to
  ship). It runs on a short-lived thread in the multithreaded apartment, because the helper is
  single-threaded for the shell and C++/WinRT will not block on an operation there. Its destination
  size is in device-independent units that it multiplies by the display's scale, so the first page
  drawn measures that scale. **It must not stay loaded:** it keeps a Direct3D device that its own
  DLL teardown releases at process exit, where Direct3D fails fast (0xC0000409; only with a window
  up, so the probe alone never showed it). After each page the factory cache is cleared and
  `CoFreeUnusedLibrariesEx(0, 0)` unloads it, at about 65 ms a page including the reload.
- **macOS:** `CGPDFDocument`, drawn on white with the page's own transform, rotation included.
- **Linux:** poppler's `pdfinfo` and `pdftoppm` when they are on the PATH, with the thumbnailers'
  five seconds; otherwise the thumbnail, as before.

The pane shows a page at about twice its size, with arrows and "Page n of m" under it; pages share
the thumbnail cache, so turning back is immediate. The probe writes a two-page PDF (a wide red page,
a tall blue one) and checks the size and colour of both, the second asked for past the end.

## G62. Pointer shapes on boxes, and a Name column that is dragged to size

`box_t::cursor` names the pointer's shape over a box. It is read from the topmost box under the
pointer that takes events -- the same box a press would reach -- so a button over a field gets the
arrow, not the field's beam. While a press is held the shape does not change, because a drag
outruns a thin grip at once and the arrow flickering back mid-drag is what every toolkit that
forgets this looks like. `window_t::cursor()` reports the shape last set, which is how a test
sees it. Line and text edits now show the text beam.

The explorer's Name column fills the room the fixed columns leave until its right edge is dragged;
then it has a width of its own (at least 120, at most what leaves the others theirs) and the space
past Size is empty, as in Explorer and Finder. A double click on the edge lets it fill again. The
width is not remembered between runs.

Every column has an edge now: Date, Type and Size are dragged between 48 and 600 and a double
click puts each back to its own width; their cells clip, so a narrow column cuts its text rather
than spilling into the next.

## G63. Multi-select in the explorer, and a test that drives the window

The table keeps a flag per entry beside the current row (where the keyboard is) and an anchor. A
click selects one row, Ctrl+click (Command on a Mac) toggles one, Shift+click and Shift with the
movement keys select from the anchor, Ctrl+A selects all, and a click below the rows clears it.
Only rows shown count as selected, so a filter that hides a selected row keeps it out of a Delete.
A right click on a selected row keeps the selection, and the menu acts on all of it: open the
files, compress them into one archive named after the folder, copy their paths a line each, delete.
Deleting several moves each on its own, so one that fails does not keep the others, and reports
how many went. The details pane totals the selection; the preview shows only for one.

`vui_explorer --ui-test` builds a scratch folder, opens the real window on it and feeds clicks and
keys into the window's own input queue -- never the system's pointer, which would land on whatever
is on top of the screen. It checks selection by mouse and keyboard, the menu on a selection,
deleting three items, and dragging and resetting the Name column, reading state back through a
hook the explorer sets while it runs. It caught a test of its own reading the cursor before the
click was processed, and runs under X11, Wayland, macOS and Windows.

## G64. A short window shrinks the explorer's panes; laid_out() for what a clip hides

Shrinking the window pushed the status bar off the bottom: the row holding the sidebar, the list
and the details is as tall as its tallest natural child, and the sidebar's natural height is every
place and drive. The sidebar and the details pane are now clipping boxes with a small height of
their own (G50: a clip box with a size does not grow to fit its children), the list's minimum is two
rows, and the sidebar scrolls with the wheel, with a thin thumb when it has more than fits.

Scrolling needs the height of what is hidden, and `bounds()` is cut to the clip by design, so
`pressable_t::laid_out()` reports the box whole, where layout put it. `--ui-test` opens a second,
short window and checks that the list and the sidebar end above the status bar and that the wheel
scrolls the sidebar.

## G65. Folder sizes, counted in the background

Once a folder's listing is complete, each folder in it is counted on a thread of its own, one
after another: everything under it is read with the same `shell::list` the window uses, links not
followed, so nothing is counted twice and a link back up the tree is not walked forever. Folders
that cannot be read are skipped. A big folder's running total is shown faded with an ellipsis as it
grows, every quarter second; finished sizes go to the window in batches at most every 100 ms,
because a folder of a thousand small folders would otherwise rebuild the table a thousand times.
Going elsewhere stops the count (the listing's generation counter); sizes are kept for the session,
so coming back shows them at once, and reading the same folder again (F5) counts its folders again.
Sorting by size orders folders by what they hold once that is known. The details pane gives a
folder's size and file count, "so far" until it is done.

The sizes are logical: what the files hold, not what they occupy on disk, which Explorer's
Properties also reports first. A tree with millions of files takes as long as reading it does; it
runs on its own thread, so nothing waits for it.

## G66. A grid of equal cells

`grid_t` lays its children out in a fixed number of columns, all one width: the widest child, or an
equal share of the width the grid is given if that is more. Rows are as tall as their tallest
child. The task manager's per-core charts needed it. They were rows of rows, each chart set to grow
and a spacer filling each empty slot in the last row. A row hands out only the space left after each
child's natural width, and a spacer has none, so the charts in a short last row came out wider than
the ones above. A grid sizes cells from the grid, not from what else shares the row.

The grid does not hand out height: a grid given more height than its rows need leaves it below them.

## G67. The default file manager, WezTerm, and running as administrator

**Default file manager.** Windows has no setting for one, so this does what Directory Opus and
XYplorer do, for the current user only: a verb of its own under `HKCU\Software\Classes\Directory`
and `\Drive`, made the default verb, and Win+E's item
(`CLSID\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\shell\opennewwindow\command`, `DelegateExecute`
empty). Not the `Folder` class: that also holds This PC, the Recycle Bin and Control Panel, which
this explorer cannot show. A default verb that was there before is kept beside it and put back;
restoring removes only keys left empty, so anything else under them stays. The command holds the
executable's own path, so it should be registered from a copy that stays where it is. The menu on
empty space toggles it, as do `--register` and `--unregister`; `--probe` runs the round trip under
a scratch key with a value of the user's own beside it. "Show in folder" from other programs calls a
shell function that always opens Explorer, and stays as it is.

A drive arrives as `"C:\"`, which the C runtime parses as `C:` and a quote; `--at` reads the quote
as the backslash it swallowed.

**WezTerm.** Windows' own default-terminal setting takes only terminals that implement its handoff
interface, which WezTerm does not, so "Open terminal here" chooses for itself: WezTerm wherever it
is installed (PATH, Program Files, a per-user install, Scoop; the app bundle on macOS; the PATH on
Linux), with `start --cwd`, which opens the shell its configuration names; else what it used before.

**Administrator.** "Open WezTerm here as administrator" and "Run as administrator" for .exe, .com,
.bat, .cmd and .msi use the runas verb, which asks through UAC; a declined prompt is reported as
such. An elevated process starts in System32 whatever directory it is given, so the folder always
travels in the arguments. Windows only.

## G68. Submenus, and Places changed only through one

A `menu_item_t` of kind `submenu` carries rows of its own, opened beside it when the pointer rests
on it, when it is pressed, or with Right; Left goes back out. A choice from it reports the
submenu's row in `index` and its own row in `sub`, which is `menu_choice_t::top` for a row of the
menu itself. One level deep: nothing here needs more, and a second level is where menus stop being
usable. The submenu is a popup of its own, so keeping it inside the window moves only it, not the
menu it came from. Where to put it is read from the frame that showed its row, which is the frame
the pointer rested on it in.

In the explorer, moving, removing and resetting Places sit in a "Manage Places" submenu: removing a
place, or all of them, should take a deliberate step, and a context menu is where a slip of the
pointer lands. "Add to Places" is gone from every menu; a folder is added by dragging it there.
