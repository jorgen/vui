/*
Copyright (c) 2026 Jørgen Lind

  Permission is hereby granted, free of charge, to any person obtaining a copy of
  this software and associated documentation files (the "Software"), to deal in
  the Software without restriction, including without limitation the rights to
  use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
  of the Software, and to permit persons to whom the Software is furnished to do
  so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

#include "vui/ui/ui.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <string>
#include <unordered_map>

#include <vio/event_loop.h>

#include "ui_renderer.h"
#include "ui_text.h"
#include "vui/context.h"

namespace vui::ui
{
namespace detail
{

// One laid-out piece of a component's own output: a primitive in the component's coordinates, or a
// mounted child placed at an offset. `box` is set on the primitive that draws a box, which is what
// hit testing reports; a shadow's primitive has none. A clipping box brackets its children with two
// markers that draw nothing: `clip` 1 opens a clip to the prim's rectangle, -1 closes it (G50).
struct entry_t
{
  prim_t prim;
  slot_t *child = nullptr;
  const box_t *box = nullptr;
  float x = 0;
  float y = 0;
  int ordinal = 0;
  std::int8_t clip = 0;
};

// A clip rectangle, left, top, right and bottom.
using clip_rect_t = std::array<float, 4>;
constexpr clip_rect_t no_clip{-run_t::clip_none, -run_t::clip_none, run_t::clip_none, run_t::clip_none};

clip_rect_t intersect(const clip_rect_t &a, const clip_rect_t &b) noexcept
{
  return {std::max(a[0], b[0]), std::max(a[1], b[1]), std::min(a[2], b[2]), std::min(a[3], b[3])};
}

bool empty(const clip_rect_t &r) noexcept
{
  return r[2] <= r[0] || r[3] <= r[1];
}

// A chunk is the flattened output of a subtree too small to keep apart (G29): its own primitives
// and its inlined descendants', in its own coordinates, cut into segments wherever a descendant
// large enough to hold a chunk of its own sits in painter's order.
struct segment_t
{
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
  slot_t *child = nullptr;
  float x = 0;
  float y = 0;
  // In the holder's coordinates.
  clip_rect_t clip = no_clip;
};

struct chunk_t
{
  std::vector<prim_t> prims;
  std::vector<segment_t> segments;
  bool valid = false;
};

// What the framework keeps for one mounted component.
struct slot_t
{
  // A list item's ownership, released last: after the scope, whose behaviours hold the component.
  std::shared_ptr<void> keep;
  void *component = nullptr;
  node_t (*view)(const void *) = nullptr;
  behaviour_t (*behave)(void *, scope_t &) = nullptr;
  std::unique_ptr<scope_t> scope;
  node_t output;
  // Its mount's scale (mount_t::scale): a scaled slot always holds its own chunk.
  float scale = 1;
  // A popup (mount_t::popup): it holds its own chunk too, which is drawn after everything else.
  bool popup = false;
  const pressable_t *dismiss = nullptr;
  // The mounts in `output`, found when the view ran.
  std::vector<const mount_t *> mounts;
  // Boxes in this subtree a press can be aimed at; a subtree with none is skipped by hit testing.
  std::size_t targets = 0;
  // Its view must run again.
  bool dirty = true;
  // It or something below it changed since the last frame, so its layout, entries and chunk are
  // stale. Set on the way back up from a view that ran; never re-runs this slot's own view.
  bool subtree_dirty = false;
  bool reached = false;
  // Laid out at least once; until then it counts as changed.
  bool laid_out = false;

  extent_t size;
  // What it was last laid out to fill, -1 where nothing was given; and its size with nothing given,
  // which a flex parent measures it by (G42).
  extent_t want{-1, -1};
  extent_t natural;
  bool natural_valid = false;
  std::vector<entry_t> entries;
  std::size_t subtree_prims = 0;
  bool holder = false;
  chunk_t chunk;
};

// Where a press is aimed. A box with a source is that source wherever it moves; a box with only an
// on_press handler is its place in its component's output, which survives a rebuild of the same
// shape.
struct target_t
{
  const pressable_t *source = nullptr;
  slot_t *slot = nullptr;
  int ordinal = -1;

  [[nodiscard]] bool empty() const noexcept
  {
    return source == nullptr && slot == nullptr;
  }
  friend bool operator==(const target_t &, const target_t &) = default;
};

namespace
{
prim_t box_prim(const box_t &box, float x, float y, extent_t size)
{
  prim_t prim;
  prim.x = x;
  prim.y = y;
  prim.width = float(size.width);
  prim.height = float(size.height);
  for (auto &radius : prim.radii)
    radius = box.radius;
  prim.fill = box.color;
  prim.border_color = box.border_color;
  prim.border_width = box.border_width;
  return prim;
}

bool has_shadow(const box_t &box) noexcept
{
  return box.shadow.color.a > 0 && (box.shadow.blur > 0 || box.shadow.x != 0 || box.shadow.y != 0);
}

prim_t shadow_prim(const box_t &box, float x, float y, extent_t size)
{
  prim_t prim = box_prim(box, x, y, size);
  prim.x += box.shadow.x;
  prim.y += box.shadow.y;
  prim.fill = box.shadow.color;
  prim.border_color = {0, 0, 0, 0};
  prim.border_width = 0;
  prim.blur = box.shadow.blur;
  return prim;
}

// Move a prim by (dx, dy). A segment's and an area's far ends are positions too. Every prim moves
// by the same additions, so two areas that share an x still share it bit for bit after any move.
void move_prim(prim_t &prim, float dx, float dy) noexcept
{
  prim.x += dx;
  prim.y += dy;
  if (prim.kind == prim_kind_t::segment)
  {
    prim.radii[0] += dx;
    prim.radii[1] += dy;
  }
  else if (prim.kind == prim_kind_t::area)
  {
    prim.radii[0] += dx;
    prim.radii[1] += dy;
    prim.radii[2] += dy;
  }
}

// M4 decimation for a line whose x never decreases: per pixel column the first, lowest, highest
// and last point, in their original order. Empty when x does decrease somewhere, which M4 cannot
// keep the look of.
std::vector<vec2_t> decimate_m4(const std::vector<vec2_t> &points, float scale)
{
  std::vector<vec2_t> out;
  const std::size_t n = points.size();
  std::size_t i = 0;
  while (i < n)
  {
    const float column = std::floor(points[i].x * scale);
    std::size_t first = i;
    std::size_t low = i;
    std::size_t high = i;
    std::size_t last = i;
    for (++i; i < n; ++i)
    {
      if (points[i].x < points[i - 1].x)
        return {};
      if (std::floor(points[i].x * scale) != column)
        break;
      if (points[i].y < points[low].y)
        low = i;
      if (points[i].y > points[high].y)
        high = i;
      last = i;
    }
    const std::size_t picks[4] = {first, std::min(low, high), std::max(low, high), last};
    for (std::size_t k = 0; k < 4; ++k)
    {
      if (k == 0 || picks[k] != picks[k - 1])
        out.push_back(points[picks[k]]);
    }
  }
  return out;
}

// The nodes inside a container, or null.
const std::vector<node_t> *children_of(const node_t &node) noexcept
{
  if (const auto *row = std::get_if<row_t>(&node))
    return &row->children;
  if (const auto *column = std::get_if<column_t>(&node))
    return &column->children;
  if (const auto *grid = std::get_if<grid_t>(&node))
    return &grid->children;
  if (const auto *box = std::get_if<box_t>(&node))
    return &box->children;
  if (const auto *at = std::get_if<at_t>(&node))
    return &at->children;
  return nullptr;
}

// Where a child of `size` starts in `room`, by alignment. Baseline is resolved by the row itself. A
// centred child is snapped to the nearest pixel rather than floored to a whole unit: at 1.5x a unit
// is a pixel and a half, and flooring left a centred label visibly off (G41).
float aligned(align_t align, int room, int size, float scale) noexcept
{
  switch (align)
  {
  case align_t::center:
    return std::round(float(room - size) * 0.5f * scale) / scale;
  case align_t::end:
    return float(room - size);
  default:
    return 0;
  }
}
} // namespace

class runtime_t
{
public:
  runtime_t(window_t &window, run_config_t config)
    : _window(window)
    , _loop(window.impl()->display.loop)
    , _config(std::move(config))
    , _beat(*_loop, [this] { beat(); })
    , _finish(*_loop,
              [this]
              {
                if (auto handle = std::exchange(_drained_waiter, {}))
                  handle.resume();
              })
  {
  }
  runtime_t(const runtime_t &) = delete;
  runtime_t &operator=(const runtime_t &) = delete;

  // Kept from construction: a behaviour woken by its own timer after the window closed, before
  // the frame loop has noticed, still asks for the loop, and the closed window no longer has it.
  [[nodiscard]] vio::event_loop_t &loop() const noexcept
  {
    return *_loop;
  }

  [[nodiscard]] window_t &window() const noexcept
  {
    return _window;
  }
  [[nodiscard]] extent_t viewport() const noexcept
  {
    return _logical;
  }
  frame_stats_t &stats() noexcept
  {
    return _stats;
  }

  [[nodiscard]] const run_config_t &config() const noexcept
  {
    return _config;
  }

  // ---- resumption: everything the toolkit resumes, it resumes here, from the loop's deferred
  // queue, so nothing it wakes runs on the stack of whatever recorded the event.
  void ready(waiter_t &waiter) noexcept
  {
    _ready.push_back(waiter);
    schedule();
  }
  void bury(std::coroutine_handle<> frame) noexcept
  {
    _dead.push_back(frame);
    schedule();
  }
  void schedule() noexcept
  {
    _beat.schedule();
  }

  void beat()
  {
    for (;;)
    {
      if (auto *waiter = _ready.pop_front())
      {
        waiter->handle.resume();
        continue;
      }
      if (!_dead.empty())
      {
        for (auto frame : std::exchange(_dead, {}))
          frame.destroy();
        reap();
        continue;
      }
      break;
    }
  }

  // ---- invalidation
  void invalidate(slot_t &slot) noexcept
  {
    slot.dirty = true;
    wake_frame();
  }
  void wake_frame() noexcept
  {
    _changed = true;
    if (_change_waiter.handle && !_change_waiter.list)
      ready(_change_waiter);
  }

  struct change_awaitable_t
  {
    runtime_t &runtime;
    [[nodiscard]] bool await_ready() const noexcept
    {
      return runtime._changed;
    }
    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
      runtime._change_waiter.handle = handle;
    }
    void await_resume() const noexcept
    {
      runtime._change_waiter.handle = {};
    }
  };
  [[nodiscard]] change_awaitable_t changed() noexcept
  {
    return {*this};
  }

  // ---- the frame step: views that are dirty, layout and entries of the subtrees they are in, the
  // chunks those subtrees fall in; then the display list and the hit-test list, assembled whole.
  void step(const frame_t &frame)
  {
    using clock = std::chrono::steady_clock;
    const auto started = clock::now();
    _stats = {};
    _stats.interval = _last_frame.time_since_epoch().count() ? started - _last_frame : clock::duration{};
    _last_frame = started;
    _changed = false;
    // The renderer has drawn the previous list, so what it uploaded is no longer new.
    _text.clear_dirty();
    if (set_units(frame))
    {
      // Text is rasterized at its size in pixels, so a new scale lays everything out again.
      _text.set_scale(_scale);
      for (auto &[component, slot] : _slots)
        slot->laid_out = false;
    }
    if (!_root)
      _root = make_slot(_root_mount);
    for (auto &[component, slot] : _slots)
      slot->reached = false;
    const auto owners = tracked_owners();
    build(*_root);
    _items.clear();
    _popup_items.clear();
    _stats.views = clock::now() - started;

    std::vector<slot_t *> leaving;
    for (auto it = _slots.begin(); it != _slots.end();)
    {
      if (it->second->reached)
      {
        ++it;
        continue;
      }
      leaving.push_back(it->second.get());
      _dying.push_back(std::move(it->second));
      it = _slots.erase(it);
    }
    for (auto *slot : leaving)
      unmount(*slot, owners);

    const auto laying = clock::now();
    _measured.clear();
    lay_out(*_root, _logical);
    if (_text.take_reset())
    {
      // The atlas filled and was emptied part way through: whatever was laid out before that names
      // rectangles that are gone. Once more, from an empty atlas (G39).
      for (auto &[component, slot] : _slots)
        mark(*slot);
      _measured.clear();
      lay_out(*_root, _logical);
    }
    const auto emitting = clock::now();
    _stats.layout = emitting - laying;
    _prims.clear();
    _runs.clear();
    _root->holder = true;
    _popups.clear();
    emit(*_root, 0, 0, 1);
    // Popups last, each moved inside the window; one may open another, which lands behind it in
    // the list and so is drawn over it.
    for (std::size_t i = 0; i < _popups.size(); ++i)
    {
      auto &popup = _popups[i];
      const float w = float(popup.slot->size.width) * popup.s;
      const float h = float(popup.slot->size.height) * popup.s;
      popup.x = std::max(0.0f, std::min(popup.x, float(_logical.width) - w));
      popup.y = std::max(0.0f, std::min(popup.y, float(_logical.height) - h));
      emit(*popup.slot, popup.x, popup.y, popup.s);
    }
    for (auto *slot : std::exchange(_touched, {}))
      slot->subtree_dirty = false;

    _items.clear();
    _popup_items.clear();
    collect_items(*_root, 0, 0, 1);
    for (auto &popup : _popups)
    {
      // Under the popup and over everything else: the whole window, reporting to its dismiss.
      if (popup.slot->dismiss)
      {
        popup.backdrop.events = *popup.slot->dismiss;
        _items.push_back(draw_item_t{rect_t{0, 0, _logical.width, _logical.height}, {}, &popup.backdrop, popup.slot, -1, rect_t{0, 0, _logical.width, _logical.height}});
      }
      const auto first = _items.size();
      collect_items(*popup.slot, popup.x, popup.y, popup.s);
      _popup_items.emplace_back(first, _items.size());
    }
    update_bounds();
    take_focus_request();
    _stats.display_list = clock::now() - emitting;
    _stats.prims = _prims.size();
    _stats.runs = _runs.size();
    _stats.items = _items.size();
    _stats.components = _slots.size();
    if (_has_pointer)
      hover(_pointer);
    // An answer to a drag given since the last event: told now rather than at the next move.
    if (_drop_target)
      report_drop_answer();
    if (_config.on_layout)
      _config.on_layout(_items);
    if (_config.on_display_list)
      _config.on_display_list(display_list());
  }

  [[nodiscard]] display_list_t display_list() const noexcept
  {
    return {_prims, _runs, _config.background, _logical, _framebuffer, _scale, _text.atlas(), _text.images()};
  }

  // ---- input. Pointer positions arrive in the window system's units (G9) and are kept in layout
  // units.
  void pointer_moved(point_t position, modifier_t modifiers = modifier_t::none)
  {
    _pointer = to_logical(position);
    _has_pointer = true;
    hover(_pointer);
    if (_hovered)
      hovering(_hovered, true);
    if (_drag)
    {
      dragging(modifiers);
      return;
    }
    if (_held.source)
      drag(_held.source, drag_phase_t::move, _pointer, modifiers, _clicks);
    update_cursor();
  }

  // ---- drag and drop (G70). A drag begun by ui::start_drag stays the runtime's own while the
  // pointer is inside the window, so nothing of the window system is involved in moving something
  // from one pane to another; past the window's edge it is handed to the window system, where the
  // backend can, and comes back -- if it does -- as the window system's drop events.
  bool start_drag(std::vector<std::string> paths, drop_action_t allowed)
  {
    if (!_held.source || _drag)
      return false;
    _drag = drag_session_t{_held.source, std::make_shared<const drag_data_t>(drag_data_t{std::move(paths), {}}), allowed};
    _dragged = true;
    _drop_data = _drag->data;
    _drop_allowed = allowed;
    _drop_from_here = true;
    dragging(modifier_t::none);
    return true;
  }

  // The pointer moved while the runtime carries a drag.
  void dragging(modifier_t modifiers)
  {
    if (_drag->handed_off)
      return;
    _drop_modifiers = modifiers;
    const bool outside = _pointer.x < 0 || _pointer.y < 0 || _pointer.x >= _logical.width || _pointer.y >= _logical.height;
    if (outside && !_drag->refused_off)
    {
      drop_leave();
      if (_window.start_drag(_drag->data->paths, _drag->allowed))
      {
        _drag->handed_off = true;
        return;
      }
      // This window system cannot take it: it stays inside, and leaving goes nowhere.
      _drag->refused_off = true;
    }
    drop_over(_pointer);
  }

  // Whether a pressable is still on screen: one that was is gone when its component unmounted.
  [[nodiscard]] bool shown(const pressable_t *source) const noexcept
  {
    for (const auto &item : _items)
    {
      if (item.box->events.source == source)
        return true;
    }
    return false;
  }

  [[nodiscard]] const pressable_t *drop_target_at(point_t position) const noexcept
  {
    for (auto it = _items.rbegin(); it != _items.rend(); ++it)
    {
      const auto *source = it->box->events.source;
      if (!source || !it->rect.contains(position))
        continue;
      if (static_cast<const detail::source_t<drop_event_t> &>(*source).subscribed())
        return source;
    }
    return nullptr;
  }

  void drop_emit(const pressable_t *target, drop_phase_t phase, point_t position)
  {
    auto &drops = static_cast<detail::source_t<drop_event_t> &>(*const_cast<pressable_t *>(target));
    drops.emit(drop_event_t{target, phase, position, _drop_modifiers, _drop_allowed, _drop_data, _drop_from_here}, *this);
  }

  // The drag at `position`: leave what it was over, enter what it is over now, or move over it.
  void drop_over(point_t position)
  {
    if (_drop_target && !shown(_drop_target))
      _drop_target = nullptr;
    const auto *target = drop_target_at(position);
    if (target != _drop_target)
    {
      if (_drop_target)
        drop_emit(_drop_target, drop_phase_t::leave, position);
      _drop_target = target;
      if (target)
      {
        const_cast<pressable_t *>(target)->_drop_answer = drop_action_t::none;
        drop_emit(target, drop_phase_t::enter, position);
      }
    }
    else if (target)
      drop_emit(target, drop_phase_t::over, position);
    report_drop_answer();
  }

  void drop_leave()
  {
    if (_drop_target && shown(_drop_target))
      drop_emit(_drop_target, drop_phase_t::leave, _pointer);
    _drop_target = nullptr;
    report_drop_answer();
  }

  // What the pressable under the drag answers, as far as the drag allows it.
  [[nodiscard]] drop_action_t drop_answer() const noexcept
  {
    if (!_drop_target || !shown(_drop_target))
      return drop_action_t::none;
    return _drop_target->_drop_answer & _drop_allowed;
  }

  // The answer to the window system, and while the drag is the runtime's own, to the pointer.
  void report_drop_answer()
  {
    const auto answer = drop_answer();
    if (answer != _reported_answer)
    {
      _reported_answer = answer;
      _window.accept_drop(answer);
    }
    if (_drag && !_drag->handed_off)
    {
      const auto wanted = answer == drop_action_t::none ? cursor_t::not_allowed : cursor_t::arrow;
      if (wanted != _cursor)
      {
        _cursor = wanted;
        _window.set_cursor(wanted);
      }
    }
  }

  // Let go over the pressable under it: dropped there if it accepted, else nothing.
  drop_action_t drop_here(point_t position)
  {
    drop_over(position);
    const auto answer = drop_answer();
    if (_drop_target && answer != drop_action_t::none)
      drop_emit(_drop_target, drop_phase_t::drop, position);
    else if (_drop_target)
      drop_emit(_drop_target, drop_phase_t::leave, position);
    _drop_target = nullptr;
    _drop_data.reset();
    _drop_from_here = false;
    report_drop_answer();
    return answer;
  }

  // The drag is over: its pressable hears how.
  void finish_drag(drop_action_t result, point_t position, modifier_t modifiers)
  {
    const auto session = std::move(*_drag);
    _drag.reset();
    if (shown(session.source))
    {
      auto &drags = static_cast<detail::source_t<drag_event_t> &>(*const_cast<pressable_t *>(session.source));
      drags.emit(drag_event_t{session.source, drag_phase_t::end, position, modifiers, _clicks, result}, *this);
    }
    _cursor = cursor_t::hidden;
    update_cursor();
  }

  // The window system's drop events: a drag from elsewhere, or this window's own come back.
  void window_drop(const vui::drop_event_t &event)
  {
    const auto position = to_logical(event.position);
    _drop_modifiers = event.modifiers;
    switch (event.phase)
    {
    case drop_phase_t::enter:
      _drop_data = event.data;
      _drop_allowed = event.allowed;
      _drop_from_here = _drag && _drag->handed_off;
      _drop_target = nullptr;
      drop_over(position);
      break;
    case drop_phase_t::over:
      if (event.data)
        _drop_data = event.data;
      drop_over(position);
      break;
    case drop_phase_t::leave:
      drop_leave();
      _drop_data.reset();
      break;
    case drop_phase_t::drop:
      if (event.data)
        _drop_data = event.data;
      _drop_allowed = event.allowed;
      (void)drop_here(position);
      break;
    }
  }

  // A drag handed to the window system has ended there.
  void window_drag_ended(const drag_end_event_t &event)
  {
    if (_drag && _drag->handed_off)
      finish_drag(event.action, _pointer, modifier_t::none);
  }

  // The pointer's shape: the cursor of the topmost box under it that takes events, or the arrow.
  // Held still while a press is, so dragging off a thin grip does not flicker back to the arrow.
  void update_cursor()
  {
    if (!_held.empty())
      return;
    const auto *item = hit(_pointer);
    const auto wanted = item && item->box->cursor ? *item->box->cursor : cursor_t::arrow;
    if (wanted == _cursor)
      return;
    _cursor = wanted;
    _window.set_cursor(wanted);
  }

  // The wheel goes to the topmost pressable under the pointer that has a wheel subscriber.
  void wheel(const scroll_event_t &event)
  {
    const auto position = to_logical(event.position);
    for (auto it = _items.rbegin(); it != _items.rend(); ++it)
    {
      const auto *source = it->box->events.source;
      if (!source || !it->rect.contains(position))
        continue;
      auto &wheels = static_cast<detail::source_t<wheel_event_t> &>(*const_cast<pressable_t *>(source));
      if (!wheels.subscribed())
        continue;
      wheels.emit(wheel_event_t{source, event.dx, event.dy, position, event.modifiers}, *this);
      return;
    }
  }

  // Back and forward go to the topmost pressable under the pointer with a subscriber, as the wheel does.
  void navigate(point_t position, bool forward, modifier_t modifiers)
  {
    for (auto it = _items.rbegin(); it != _items.rend(); ++it)
    {
      const auto *source = it->box->events.source;
      if (!source || !it->rect.contains(position))
        continue;
      auto &navigates = static_cast<detail::source_t<navigate_event_t> &>(*const_cast<pressable_t *>(source));
      if (!navigates.subscribed())
        continue;
      navigates.emit(navigate_event_t{source, forward, position, modifiers}, *this);
      return;
    }
  }

  // The right button goes to the topmost pressable under the pointer with a context subscriber. A
  // popup keeps it, as it keeps presses: nothing under a menu opens another.
  void context(point_t position, modifier_t modifiers)
  {
    _pointer = position;
    _has_pointer = true;
    hover(position);
    std::size_t floor = 0;
    bool landed = false;
    for (std::size_t i = _items.size(); i-- > floor;)
    {
      const auto &item = _items[i];
      const auto *source = item.box->events.source;
      if (!source || !item.rect.contains(position))
        continue;
      auto &contexts = static_cast<detail::source_t<context_event_t> &>(*const_cast<pressable_t *>(source));
      if (contexts.subscribed())
      {
        contexts.emit(context_event_t{source, position, modifiers}, *this);
        return;
      }
      if (!landed)
      {
        landed = true;
        for (const auto &[first, last] : _popup_items)
        {
          if (i >= first && i < last)
            floor = first;
        }
      }
    }
  }

  // Typed text goes to the focused pressable if it is a text input, in one queue with its keys.
  void text(const text_event_t &event)
  {
    if (_focused && !owner_of(_focused))
      set_focus(nullptr, false);
    if (_focused && _focused->text_input())
      typing(typing_event_t{event});
  }

  void typing(const typing_event_t &event)
  {
    auto &typed = static_cast<detail::source_t<typing_event_t> &>(*const_cast<pressable_t *>(_focused));
    typed.emit(event, *this);
  }
  void pointer_left()
  {
    _has_pointer = false;
    set_hovered(nullptr);
  }
  void button(const pointer_button_event_t &event)
  {
    if (event.button == button_t::right && event.pressed)
    {
      context(to_logical(event.position), event.modifiers);
      return;
    }
    if ((event.button == button_t::back || event.button == button_t::forward) && event.pressed)
    {
      navigate(to_logical(event.position), event.button == button_t::forward, event.modifiers);
      return;
    }
    if (event.button != button_t::left)
      return;
    const auto position = to_logical(event.position);
    _pointer = position;
    _has_pointer = true;
    hover(position);
    if (event.pressed)
    {
      _held = target_at(position);
      if (_held.source)
        set_flag(_held.source, &pressable_t::_held, true);
      if (_held.source && _held.source->focusable())
        set_focus(_held.source, false);
      if (_held.source)
      {
        // A press soon after the last, on the same pressable and close to it, counts on.
        const auto now = std::chrono::steady_clock::now();
        const bool again = _held.source == _click_source && now - _click_time < std::chrono::milliseconds(500) && std::abs(position.x - _click_at.x) <= 4 && std::abs(position.y - _click_at.y) <= 4;
        _clicks = again ? _clicks % 3 + 1 : 1;
        _click_source = _held.source;
        _click_time = now;
        _click_at = position;
        drag(_held.source, drag_phase_t::begin, position, event.modifiers, _clicks);
      }
      return;
    }
    // A drag the runtime carries ends where it is let go; one the window system has ends when the
    // window system says, and its pressable hears of it then.
    if (_drag && !_drag->handed_off)
    {
      _drop_modifiers = event.modifiers;
      const auto result = drop_here(position);
      finish_drag(result, position, event.modifiers);
    }
    auto held = std::exchange(_held, {});
    const bool dragged = std::exchange(_dragged, false);
    if (held.source)
    {
      set_flag(held.source, &pressable_t::_held, false);
      if (!dragged)
        drag(held.source, drag_phase_t::end, position, event.modifiers, _clicks);
    }
    update_cursor();
    if (dragged || held.empty() || !(target_at(position) == held))
      return;
    const auto *item = find(held);
    if (held.source)
      press(held.source, position);
    if (item && item->box->on_press)
    {
      // The handler mutates its component, so that component is rebuilt. Copied first: the
      // handler may invalidate, and nothing here rebuilds before it returns, but the copy keeps
      // that true however the handler is written.
      auto handler = item->box->on_press;
      auto *slot = item->slot;
      handler();
      invalidate(*slot);
    }
  }

  // ---- the keyboard (G46). Every key goes to the focused pressable; Tab then moves focus, and
  // Space presses it -- held while down, pressed on release.
  void key(const key_event_t &event)
  {
    if (_focused && !owner_of(_focused))
      set_focus(nullptr, false);
    if (_focused)
    {
      if (event.pressed)
        set_flag(_focused, &pressable_t::_focus_visible, true);
      auto &keys = static_cast<detail::source_t<key_event_t> &>(*const_cast<pressable_t *>(_focused));
      keys.emit(event, *this);
      if (_focused && _focused->text_input())
        typing(typing_event_t{event});
    }
    if (event.pressed && event.key == vui::key_t::tab)
    {
      move_focus(has(event.modifiers, modifier_t::shift));
      return;
    }
    if (!_focused || _focused->text_input() || event.key != vui::key_t::space || event.repeat)
      return;
    if (event.pressed)
    {
      set_flag(_focused, &pressable_t::_held, true);
      return;
    }
    if (!_focused->held())
      return;
    set_flag(_focused, &pressable_t::_held, false);
    const auto *item = find(target_t{_focused, nullptr, -1});
    press(_focused, item ? point_t{item->rect.x + item->rect.width / 2.0, item->rect.y + item->rect.height / 2.0} : point_t{});
  }

  // The focusable boxes in the order they are drawn, which is the order a view lists them in.
  void move_focus(bool back)
  {
    std::vector<const pressable_t *> order;
    for (const auto &item : _items)
    {
      const auto *source = item.box->events.source;
      if (source && source->focusable() && std::find(order.begin(), order.end(), source) == order.end())
        order.push_back(source);
    }
    if (order.empty())
      return;
    const auto n = order.size();
    const auto at = std::size_t(std::find(order.begin(), order.end(), _focused) - order.begin());
    const std::size_t next = at == n ? (back ? n - 1 : 0) : (back ? (at + n - 1) % n : (at + 1) % n);
    set_focus(order[next], true);
  }

  void set_focus(const pressable_t *source, bool visible)
  {
    if (_focused != source)
    {
      if (_focused)
      {
        set_flag(_focused, &pressable_t::_focused, false);
        set_flag(_focused, &pressable_t::_focus_visible, false);
      }
      _focused = source;
      if (source)
        set_flag(source, &pressable_t::_focused, true);
    }
    if (source)
      set_flag(source, &pressable_t::_focus_visible, visible);
  }

  void hovering(const pressable_t *source, bool inside)
  {
    auto &hovers = static_cast<detail::source_t<hover_event_t> &>(*const_cast<pressable_t *>(source));
    if (hovers.subscribed())
      hovers.emit(hover_event_t{source, _pointer, inside}, *this);
  }

  void drag(const pressable_t *source, drag_phase_t phase, point_t position, modifier_t modifiers, int clicks)
  {
    auto &drags = static_cast<detail::source_t<drag_event_t> &>(*const_cast<pressable_t *>(source));
    drags.emit(drag_event_t{source, phase, position, modifiers, clicks}, *this);
  }

  void press(const pressable_t *source, point_t position)
  {
    auto &presses = static_cast<detail::source_t<press_event_t> &>(*const_cast<pressable_t *>(source));
    presses.emit(press_event_t{source, position}, *this);
  }

  // ---- teardown
  void shutdown()
  {
    std::vector<slot_t *> leaving;
    for (auto &[component, slot] : _slots)
    {
      leaving.push_back(slot.get());
      _dying.push_back(std::move(slot));
    }
    _slots.clear();
    _root = nullptr;
    _items.clear();
    _popup_items.clear();
    _popups.clear();
    _prims.clear();
    _runs.clear();
    _hovered = nullptr;
    _focused = nullptr;
    _focus_request = nullptr;
    _held = {};
    _drag.reset();
    _drop_target = nullptr;
    for (auto *slot : leaving)
      unmount(*slot, {});
    reap();
  }

  struct drained_awaitable_t
  {
    runtime_t &runtime;
    [[nodiscard]] bool await_ready() const noexcept
    {
      return false;
    }
    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
      runtime._drained_waiter = handle;
      runtime.check_drained();
    }
    void await_resume() const noexcept
    {
    }
  };
  // Resolves once every scope has no behaviour left. Always suspends and is resumed from a node of
  // its own, never from inside beat(): what it resumes goes on to destroy this runtime.
  [[nodiscard]] drained_awaitable_t drained() noexcept
  {
    return {*this};
  }

  void request_focus(const pressable_t *source) noexcept
  {
    _focus_request = source;
    _focus_request_frames = 0;
    take_focus_request();
    if (_focus_request)
      wake_frame();
  }

  // A requested focus lands once a box showing it is in the hit-test list, and is given up after a
  // few frames without one: whatever asked has changed its mind.
  void take_focus_request() noexcept
  {
    if (!_focus_request)
      return;
    if (owner_of(_focus_request))
    {
      set_focus(std::exchange(_focus_request, nullptr), true);
      return;
    }
    if (++_focus_request_frames > 3)
      _focus_request = nullptr;
  }

  void invalidate_component(void *component, node_t (*view)(const void *)) noexcept
  {
    if (auto it = _slots.find(key_t{component, view}); it != _slots.end())
      invalidate(*it->second);
  }

  void set_root(mount_t root) noexcept
  {
    _root_mount = root;
  }

private:
  // Layout is in logical units: the framebuffer divided by the scale, which is the window's size on
  // Wayland and macOS and its size divided by the UI scale on Windows and X11 (G9).
  // Returns whether the scale changed.
  bool set_units(const frame_t &frame)
  {
    _framebuffer = frame.framebuffer;
    const float previous = _scale;
    _scale = frame.scale > 0 ? frame.scale : 1.0f;
    _logical = {int(std::lround(frame.framebuffer.width / _scale)), int(std::lround(frame.framebuffer.height / _scale))};
    _input_x = frame.size.width > 0 ? double(_logical.width) / frame.size.width : 1.0;
    _input_y = frame.size.height > 0 ? double(_logical.height) / frame.size.height : 1.0;
    return _scale != previous;
  }

public:
  [[nodiscard]] point_t to_logical(point_t position) const noexcept
  {
    return {position.x * _input_x, position.y * _input_y};
  }

private:
  slot_t *make_slot(const mount_t &mount)
  {
    auto slot = std::make_unique<slot_t>();
    slot->component = mount.component;
    slot->view = mount.view;
    slot->behave = mount.behave;
    slot->scope = std::make_unique<scope_t>(*this, slot.get());
    slot->keep = mount.keep;
    slot->scale = mount.scale > 0 ? mount.scale : 1;
    slot->popup = mount.popup;
    slot->dismiss = mount.dismiss;
    if (mount.props)
      mount.apply_props(slot->component, mount.props.get());
    auto *raw = slot.get();
    _slots.emplace(key_of(mount), std::move(slot));
    if (raw->behave)
      raw->scope->spawn(raw->behave(raw->component, *raw->scope));
    return raw;
  }

  slot_t *slot_for(const mount_t &mount)
  {
    auto it = _slots.find(key_of(mount));
    return it == _slots.end() ? make_slot(mount) : it->second.get();
  }

  slot_t *existing(const mount_t &mount) const
  {
    auto it = _slots.find(key_of(mount));
    return it == _slots.end() ? nullptr : it->second.get();
  }

  // Re-run the views that are dirty, starting at `slot`, and mark what is still mounted. Returns
  // whether anything at or below `slot` changed, which is what marks it subtree_dirty: the mark
  // travels up through the return values and never re-runs a view.
  bool build(slot_t &slot)
  {
    slot.reached = true;
    bool changed = false;
    if (slot.dirty)
    {
      slot.dirty = false;
      ++_stats.views_run;
      slot.output = slot.view(slot.component);
      slot.mounts.clear();
      find_mounts(slot.output, slot.mounts);
      changed = true;
    }
    // Only the mounts, found once when the view ran: walking a clean view's whole tree every frame
    // to find them cost as much as a frame of a large scene (G45).
    for (const auto *mount : slot.mounts)
    {
      if (visit_mount(*mount))
        changed = true;
    }
    if (changed || !slot.laid_out)
      mark(slot);
    return slot.subtree_dirty;
  }

  static void find_mounts(const node_t &node, std::vector<const mount_t *> &out)
  {
    if (const auto *mount = std::get_if<mount_t>(&node))
    {
      out.push_back(mount);
      return;
    }
    if (const auto *children = children_of(node))
    {
      for (const auto &child : *children)
        find_mounts(child, out);
    }
  }

  bool visit_mount(const mount_t &mount)
  {
    auto *child = slot_for(mount);
    if (mount.keep && child->keep != mount.keep)
      child->keep = mount.keep;
    child->scale = mount.scale > 0 ? mount.scale : 1;
    child->popup = mount.popup;
    child->dismiss = mount.dismiss;
    if (mount.props && !mount.props_equal(child->component, mount.props.get()))
    {
      mount.apply_props(child->component, mount.props.get());
      child->dirty = true;
    }
    return !child->reached && build(*child);
  }

  void mark(slot_t &slot)
  {
    if (slot.subtree_dirty)
      return;
    slot.subtree_dirty = true;
    _touched.push_back(&slot);
  }

  // ---- layout. A subtree that did not change keeps its size and its entries; one that did lays
  // its children out first and then places its own output around them, in one pass, since a row
  // or column needs each child's size only once it has been placed.
  // `want` is the size the parent gives it, -1 in a dimension it leaves to the content (G42). A
  // slot laid out for the same want and unchanged below keeps what it has.
  void lay_out(slot_t &slot, extent_t want = unconstrained)
  {
    if (!slot.subtree_dirty && slot.laid_out && slot.want == want)
      return;
    mark(slot);
    slot.entries.clear();
    slot.subtree_prims = 0;
    int ordinal = 0;
    slot.size = place(slot, slot.output, 0, 0, ordinal, want).size;
    slot.want = want;
    slot.laid_out = true;
    slot.targets = 0;
    for (const auto &entry : slot.entries)
    {
      if (entry.child)
        slot.targets += entry.child->targets;
      else if (entry.box && (entry.box->events.source || entry.box->on_press))
        ++slot.targets;
    }
    if (want == unconstrained)
    {
      slot.natural = slot.size;
      slot.natural_valid = true;
    }
  }

  static constexpr extent_t unconstrained{-1, -1};

  // What placing a node produced: its size, and its first baseline below its top if it has one.
  struct placed_t
  {
    extent_t size;
    float baseline = -1;
    [[nodiscard]] bool has_baseline() const noexcept
    {
      return baseline >= 0;
    }
  };

  // A child a container has placed at its start position: the range of entries it produced, so
  // that moving it into position reaches all of them.
  struct placed_child_t
  {
    std::size_t begin = 0;
    std::size_t end = 0;
    placed_t placed;
  };

  placed_child_t place_child(slot_t &slot, const node_t &node, float x, float y, int &ordinal, extent_t want = unconstrained)
  {
    placed_child_t child;
    child.begin = slot.entries.size();
    child.placed = place(slot, node, x, y, ordinal, want);
    child.end = slot.entries.size();
    return child;
  }

  void shift(slot_t &slot, const placed_child_t &child, float dx, float dy)
  {
    if (dx == 0 && dy == 0)
      return;
    for (std::size_t i = child.begin; i < child.end; ++i)
    {
      auto &entry = slot.entries[i];
      entry.x += dx;
      entry.y += dy;
      if (!entry.child)
        move_prim(entry.prim, dx, dy);
    }
  }

  // A node's size with nothing given to it: placed and then taken back. Cached for the frame by
  // the node's address, so a flex container's children are measured once however deep it nests; a
  // mounted child that did not change answers from its last layout.
  placed_t measure(slot_t &slot, const node_t &node)
  {
    if (const auto *mount = std::get_if<mount_t>(&node))
    {
      if (auto *child = existing(*mount); child && child->natural_valid && !child->subtree_dirty)
        return {scaled(child->natural, child->scale)};
    }
    if (auto it = _measured.find(&node); it != _measured.end())
      return it->second;
    const auto entries = slot.entries.size();
    const auto prims = slot.subtree_prims;
    int ordinal = 0;
    const auto placed = place(slot, node, 0, 0, ordinal, unconstrained);
    slot.entries.resize(entries);
    slot.subtree_prims = prims;
    _measured.emplace(&node, placed);
    return placed;
  }

  static float grow_of(const node_t &node) noexcept
  {
    if (const auto *box = std::get_if<box_t>(&node))
      return box->grow;
    if (const auto *row = std::get_if<row_t>(&node))
      return row->grow;
    if (const auto *column = std::get_if<column_t>(&node))
      return column->grow;
    if (const auto *grid = std::get_if<grid_t>(&node))
      return grid->grow;
    if (const auto *spacer = std::get_if<spacer_t>(&node))
      return spacer->grow;
    return 0;
  }

  // Layout is children first. A container places each child at its start position, and moves it
  // once its own size is known -- which is only once every child has been placed. Only a container
  // that hands out space -- to children that grow or stretch, or because it was given a size --
  // measures its children first (G42).
  placed_t place(slot_t &slot, const node_t &node, float x, float y, int &ordinal, extent_t want = unconstrained)
  {
    if (const auto *box = std::get_if<box_t>(&node))
      return place_box(slot, *box, x, y, ordinal, want);
    if (const auto *text = std::get_if<text_t>(&node))
    {
      const auto laid = _text.add_text(*text, default_font(), x, y, _glyphs);
      place_glyphs(slot);
      return {laid.size, laid.baseline};
    }
    if (const auto *icon = std::get_if<icon_t>(&node))
    {
      const auto size = _text.add_icon(*icon, x, y, _glyphs);
      place_glyphs(slot);
      return {size};
    }
    if (const auto *line = std::get_if<polyline_t>(&node))
      return place_polyline(slot, *line, x, y);
    if (const auto *map = std::get_if<heatmap_t>(&node))
    {
      _text.add_heatmap(*map, x, y, _glyphs);
      place_glyphs(slot);
      return {map->size};
    }
    if (const auto *image = std::get_if<image_t>(&node))
    {
      _text.add_image(*image, x, y, _glyphs);
      place_glyphs(slot);
      return {image->size};
    }
    if (const auto *spacer = std::get_if<spacer_t>(&node))
      return {{std::max(spacer->size.width, want.width), std::max(spacer->size.height, want.height)}};
    if (const auto *at = std::get_if<at_t>(&node))
    {
      extent_t size;
      for (const auto &child : at->children)
      {
        const auto placed = place(slot, child, x + at->x, y + at->y, ordinal);
        size.width = std::max(size.width, int(std::ceil(at->x + float(placed.size.width))));
        size.height = std::max(size.height, int(std::ceil(at->y + float(placed.size.height))));
      }
      return {size};
    }
    if (const auto *mount = std::get_if<mount_t>(&node))
    {
      auto *child = existing(*mount);
      if (!child)
        return {};
      if (child->popup)
      {
        // Laid out for its own content, placed here, and taking no room (G46).
        lay_out(*child);
        entry_t entry;
        entry.child = child;
        entry.x = x;
        entry.y = y;
        slot.entries.push_back(entry);
        slot.subtree_prims += child->subtree_prims;
        return {};
      }
      // A scaled child is laid out in its own units and takes up its scaled size.
      const float s = child->scale;
      lay_out(*child, s == 1 ? want : extent_t{want.width >= 0 ? int(float(want.width) / s) : -1, want.height >= 0 ? int(float(want.height) / s) : -1});
      entry_t entry;
      entry.child = child;
      entry.x = x;
      entry.y = y;
      slot.entries.push_back(entry);
      slot.subtree_prims += child->subtree_prims;
      // A component's baseline is not kept between frames, so a mounted child has none.
      return {scaled(child->size, s)};
    }
    if (const auto *row = std::get_if<row_t>(&node))
      return place_line(slot, row->children, true, row->gap, row->align, row->padding, x, y, ordinal, want);
    if (const auto *grid = std::get_if<grid_t>(&node))
      return place_grid(slot, *grid, x, y, ordinal, want);
    const auto &column = std::get<column_t>(node);
    return place_line(slot, column.children, false, column.gap, column.align, column.padding, x, y, ordinal, want);
  }

  placed_t place_box(slot_t &slot, const box_t &box, float x, float y, int &ordinal, extent_t want)
  {
    // The box is drawn before its children, so its entries go in first and are filled in once the
    // children have given it its size.
    const bool shadowed = has_shadow(box);
    if (shadowed)
    {
      slot.entries.push_back(entry_t{});
      ++slot.subtree_prims;
    }
    const std::size_t index = slot.entries.size();
    slot.entries.push_back(entry_t{});
    ++slot.subtree_prims;
    const int box_ordinal = ordinal++;

    const auto &pad = box.padding;
    const bool stretch_x = box.align_x == align_t::stretch;
    const bool stretch_y = box.align_y == align_t::stretch;
    // A child stretched to the room needs the room first, and the room needs the children's sizes.
    extent_t room{-1, -1};
    if (stretch_x || stretch_y)
    {
      extent_t content;
      for (const auto &node : box.children)
      {
        const auto natural = measure(slot, node).size;
        content.width = std::max(content.width, natural.width);
        content.height = std::max(content.height, natural.height);
      }
      room = {fit(box, box.size.width, content.width + pad.left + pad.right, want.width) - pad.left - pad.right, fit(box, box.size.height, content.height + pad.top + pad.bottom, want.height) - pad.top - pad.bottom};
    }
    const extent_t child_want{stretch_x ? room.width : -1, stretch_y ? room.height : -1};
    const std::size_t clip_index = slot.entries.size();
    if (box.clip)
      slot.entries.push_back(entry_t{.clip = 1});

    std::vector<placed_child_t> children;
    children.reserve(box.children.size());
    extent_t content;
    for (const auto &node : box.children)
    {
      children.push_back(place_child(slot, node, x + float(pad.left), y + float(pad.top), ordinal, child_want));
      content.width = std::max(content.width, children.back().placed.size.width);
      content.height = std::max(content.height, children.back().placed.size.height);
    }
    const extent_t size{fit(box, box.size.width, content.width + pad.left + pad.right, want.width), fit(box, box.size.height, content.height + pad.top + pad.bottom, want.height)};
    room = {size.width - pad.left - pad.right, size.height - pad.top - pad.bottom};
    float baseline = -1;
    for (const auto &child : children)
    {
      const float dx = aligned(box.align_x, room.width, child.placed.size.width, _scale);
      const float dy = aligned(box.align_y, room.height, child.placed.size.height, _scale);
      shift(slot, child, dx, dy);
      if (baseline < 0 && child.placed.has_baseline())
        baseline = float(pad.top) + dy + child.placed.baseline;
    }

    if (box.clip)
    {
      auto &open = slot.entries[clip_index].prim;
      open.x = x + box.border_width;
      open.y = y + box.border_width;
      open.width = std::max(float(size.width) - 2 * box.border_width, 0.0f);
      open.height = std::max(float(size.height) - 2 * box.border_width, 0.0f);
      slot.entries.push_back(entry_t{.clip = -1});
    }
    if (shadowed)
      slot.entries[index - 1].prim = shadow_prim(box, x, y, size);
    auto &entry = slot.entries[index];
    entry.prim = box_prim(box, x, y, size);
    entry.box = &box;
    entry.x = x;
    entry.y = y;
    entry.ordinal = box_ordinal;
    return {size, baseline};
  }

  // A box's size along one dimension: at least its own and what it is given, and big enough for its
  // content unless it clips and one of the other two decided.
  static int fit(const box_t &box, int own, int content, int given) noexcept
  {
    if (box.clip && (own > 0 || given >= 0))
      return std::max(own, given);
    return std::max({own, content, given});
  }

  placed_t place_line(slot_t &slot, const std::vector<node_t> &nodes, bool horizontal, int gap, align_t align, const edges_t &pad, float x, float y, int &ordinal, extent_t want)
  {
    const int pad_along = horizontal ? pad.left + pad.right : pad.top + pad.bottom;
    const int pad_across = horizontal ? pad.top + pad.bottom : pad.left + pad.right;
    const int want_along = horizontal ? want.width : want.height;
    const int want_across = horizontal ? want.height : want.width;

    // What each child is given: its share of the space left along the line if it grows, the line's
    // whole breadth if the line stretches. Without either, nothing is measured.
    std::vector<extent_t> wants(nodes.size(), unconstrained);
    int across_room = -1;
    float total_grow = 0;
    for (const auto &node : nodes)
      total_grow += std::max(grow_of(node), 0.0f);
    if (total_grow > 0 || align == align_t::stretch || want_along >= 0 || want_across >= 0)
    {
      std::vector<extent_t> naturals;
      naturals.reserve(nodes.size());
      int natural_along = 0;
      int natural_across = 0;
      for (std::size_t i = 0; i < nodes.size(); ++i)
      {
        naturals.push_back(measure(slot, nodes[i]).size);
        natural_along += (horizontal ? naturals.back().width : naturals.back().height) + (i > 0 ? gap : 0);
        natural_across = std::max(natural_across, horizontal ? naturals.back().height : naturals.back().width);
      }
      across_room = std::max(natural_across, want_across - pad_across);
      const int extra = std::max(want_along - pad_along - natural_along, 0);
      // Shares are whole units; what rounding leaves over goes to the last child that grows.
      int handed = 0;
      std::size_t last = nodes.size();
      for (std::size_t i = 0; i < nodes.size(); ++i)
      {
        if (grow_of(nodes[i]) > 0)
          last = i;
      }
      for (std::size_t i = 0; i < nodes.size(); ++i)
      {
        auto &w = wants[i];
        const float g = std::max(grow_of(nodes[i]), 0.0f);
        if (g > 0 && extra > 0)
        {
          const int share = i == last ? extra - handed : int(float(extra) * g / total_grow);
          handed += share;
          (horizontal ? w.width : w.height) = (horizontal ? naturals[i].width : naturals[i].height) + share;
        }
        if (align == align_t::stretch)
          (horizontal ? w.height : w.width) = across_room;
      }
    }

    std::vector<placed_child_t> children;
    children.reserve(nodes.size());
    // Where each child starts along the line, from the content's edge.
    std::vector<int> starts;
    starts.reserve(nodes.size());
    int along = 0;
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
      if (i > 0)
        along += gap;
      starts.push_back(along);
      const float cx = x + float(pad.left + (horizontal ? along : 0));
      const float cy = y + float(pad.top + (horizontal ? 0 : along));
      children.push_back(place_child(slot, nodes[i], cx, cy, ordinal, wants[i]));
      const auto &size = children.back().placed.size;
      along += horizontal ? size.width : size.height;
    }
    along = std::max(along, want_along - pad_along);

    int across = std::max(across_room, 0);
    float baseline = -1;
    if (horizontal && align == align_t::baseline)
    {
      // Above the shared baseline, the tallest ascent; below it, the deepest descent. A child with
      // no baseline stands on it. Text moves by a fraction of a unit here, and that is exact: each
      // baseline is a whole number of pixels below its anchor, so all of them land on one pixel row.
      const auto baseline_of = [](const placed_t &placed) { return placed.has_baseline() ? placed.baseline : float(placed.size.height); };
      float above = 0;
      float below = 0;
      for (const auto &child : children)
      {
        above = std::max(above, baseline_of(child.placed));
        below = std::max(below, float(child.placed.size.height) - baseline_of(child.placed));
      }
      for (const auto &child : children)
        shift(slot, child, 0, above - baseline_of(child.placed));
      across = std::max(across, int(std::ceil(above + below - 1e-3f)));
      if (!children.empty())
        baseline = float(pad.top) + above;
    }
    else
    {
      for (const auto &child : children)
        across = std::max(across, horizontal ? child.placed.size.height : child.placed.size.width);
      for (std::size_t i = 0; i < children.size(); ++i)
      {
        const auto &child = children[i];
        const auto &size = child.placed.size;
        const float d = aligned(align, across, horizontal ? size.height : size.width, _scale);
        shift(slot, child, horizontal ? 0.0f : d, horizontal ? d : 0.0f);
        if (baseline < 0 && child.placed.has_baseline())
          baseline = float(pad.top) + (horizontal ? d : float(starts[i])) + child.placed.baseline;
      }
    }
    const extent_t content = horizontal ? extent_t{along, across} : extent_t{across, along};
    return {{content.width + pad.left + pad.right, content.height + pad.top + pad.bottom}, baseline};
  }

  placed_t place_grid(slot_t &slot, const grid_t &grid, float x, float y, int &ordinal, extent_t want)
  {
    assert(grid.columns > 0);
    const auto &pad = grid.padding;
    const auto columns = std::size_t(grid.columns);
    const std::size_t rows = (grid.children.size() + columns - 1) / columns;

    int cell_width = 0;
    std::vector<int> row_heights(rows, 0);
    for (std::size_t i = 0; i < grid.children.size(); ++i)
    {
      const auto natural = measure(slot, grid.children[i]).size;
      cell_width = std::max(cell_width, natural.width);
      row_heights[i / columns] = std::max(row_heights[i / columns], natural.height);
    }
    const int gaps_x = grid.gap_x * (grid.columns - 1);
    if (want.width >= 0)
      cell_width = std::max(cell_width, (want.width - pad.left - pad.right - gaps_x) / grid.columns);

    const bool stretch = grid.align == align_t::stretch;
    float baseline = -1;
    int top = 0;
    for (std::size_t r = 0; r < rows; ++r)
    {
      if (r > 0)
        top += grid.gap_y;
      for (std::size_t c = 0; c < columns && r * columns + c < grid.children.size(); ++c)
      {
        const int left = int(c) * (cell_width + grid.gap_x);
        const extent_t cell_want = stretch ? extent_t{cell_width, row_heights[r]} : unconstrained;
        const auto child = place_child(slot, grid.children[r * columns + c], x + float(pad.left + left), y + float(pad.top + top), ordinal, cell_want);
        const float dx = aligned(grid.align, cell_width, child.placed.size.width, _scale);
        const float dy = aligned(grid.align, row_heights[r], child.placed.size.height, _scale);
        shift(slot, child, dx, dy);
        if (baseline < 0 && child.placed.has_baseline())
          baseline = float(pad.top + top) + dy + child.placed.baseline;
      }
      top += row_heights[r];
    }
    const int content_width = rows > 0 ? cell_width * grid.columns + gaps_x : 0;
    return {{std::max(content_width + pad.left + pad.right, want.width), std::max(top + pad.top + pad.bottom, want.height)}, baseline};
  }

  // Areas first, so the line covers the antialiased top edge of its own fill; then the segments;
  // then the markers over both.
  placed_t place_polyline(slot_t &slot, const polyline_t &line, float x, float y)
  {
    // Past four points a pixel column, M4 decimation: what is drawn is the same, and a million
    // points become a few thousand segments (G28, G44). Markers are each drawn, so never decimated.
    std::vector<vec2_t> decimated;
    const float columns = float(line.size.width) * _scale;
    if (line.marker <= 0 && columns > 0 && float(line.points.size()) > 4 * columns + 8)
      decimated = decimate_m4(line.points, _scale);
    const auto &points = decimated.empty() ? line.points : decimated;
    const auto push = [&](const prim_t &prim)
    {
      slot.entries.push_back(entry_t{prim});
      ++slot.subtree_prims;
    };
    if (line.fill.a > 0)
    {
      const float bottom = y + float(line.size.height);
      for (std::size_t i = 1; i < points.size(); ++i)
      {
        if (!(points[i].x > points[i - 1].x))
          continue;
        prim_t prim;
        prim.kind = prim_kind_t::area;
        prim.x = x + points[i - 1].x;
        prim.y = y + points[i - 1].y;
        prim.radii[0] = x + points[i].x;
        prim.radii[1] = y + points[i].y;
        prim.radii[2] = bottom;
        prim.fill = line.fill;
        push(prim);
      }
    }
    if (line.width > 0 && line.color.a > 0)
    {
      // The join plane at each inner point: through it, square to the sum of the two directions --
      // the mitre line. Both segments meeting there carry the same normal, so they split the joint's
      // pixels exactly. Sharper than the mitre limit, or where the line doubles back, the joint is
      // round instead: a zero normal, and the two segments' round ends overlap there (G44).
      constexpr float mitre_limit = 4;
      std::vector<vec2_t> joins(points.size());
      for (std::size_t i = 1; i + 1 < points.size(); ++i)
      {
        const vec2_t d0{points[i].x - points[i - 1].x, points[i].y - points[i - 1].y};
        const vec2_t d1{points[i + 1].x - points[i].x, points[i + 1].y - points[i].y};
        const float l0 = std::hypot(d0.x, d0.y);
        const float l1 = std::hypot(d1.x, d1.y);
        if (l0 <= 0 || l1 <= 0)
          continue;
        const vec2_t sum{d0.x / l0 + d1.x / l1, d0.y / l0 + d1.y / l1};
        const float length = std::hypot(sum.x, sum.y);
        // length / 2 is the cosine of half the turn, and the mitre is 1 / that times the width.
        if (length * 0.5f < 1.0f / mitre_limit)
          continue;
        joins[i] = {sum.x / length, sum.y / length};
      }
      for (std::size_t i = 1; i < points.size(); ++i)
      {
        prim_t prim;
        prim.kind = prim_kind_t::segment;
        prim.x = x + points[i - 1].x;
        prim.y = y + points[i - 1].y;
        prim.radii[0] = x + points[i].x;
        prim.radii[1] = y + points[i].y;
        prim.radii[2] = joins[i - 1].x;
        prim.radii[3] = joins[i - 1].y;
        prim.border_color = {joins[i].x, joins[i].y, 0, 0};
        prim.border_width = line.width;
        prim.fill = line.color;
        push(prim);
      }
    }
    if (line.marker > 0 && line.color.a > 0)
    {
      const float r = line.marker * 0.5f;
      for (const auto &point : points)
      {
        prim_t prim;
        prim.x = x + point.x - r;
        prim.y = y + point.y - r;
        prim.width = line.marker;
        prim.height = line.marker;
        for (auto &radius : prim.radii)
          radius = r;
        prim.fill = line.color;
        push(prim);
      }
    }
    return {line.size};
  }

  void place_glyphs(slot_t &slot)
  {
    for (const auto &prim : _glyphs)
      slot.entries.push_back(entry_t{prim});
    slot.subtree_prims += _glyphs.size();
    _glyphs.clear();
  }

  // The font for text that names none, looked up on first use.
  const font_t &default_font()
  {
    if (!_default_font && !_looked_for_font)
    {
      _looked_for_font = true;
      if (_config.font)
        _default_font = _config.font;
      else if (auto font = font_t::system_default())
        _default_font = std::move(*font);
    }
    return _default_font;
  }

  // ---- chunks (G29). The root always holds one. Any other subtree holds one from the threshold
  // up and gives it back below half of it, so a subtree near the limit does not switch every frame.
  static extent_t scaled(extent_t size, float s) noexcept
  {
    if (s == 1)
      return size;
    return {int(std::ceil(float(size.width) * s - 1e-3f)), int(std::ceil(float(size.height) * s - 1e-3f))};
  }

  bool decide_holder(slot_t &slot)
  {
    const std::size_t threshold = std::max<std::size_t>(_config.chunk_threshold, 1);
    // A scaled subtree is drawn as runs of its own, since the scale is a run's.
    const bool now = slot.scale != 1 || slot.popup || (slot.holder ? slot.subtree_prims >= std::max<std::size_t>(threshold / 2, 1) : slot.subtree_prims >= threshold);
    if (now != slot.holder)
    {
      slot.holder = now;
      slot.chunk = {};
    }
    return now;
  }

  void flatten(slot_t &holder)
  {
    auto &chunk = holder.chunk;
    chunk.prims.clear();
    chunk.segments.clear();
    std::uint32_t begin = 0;
    std::vector<clip_rect_t> clips{no_clip};
    auto close_run = [&]
    {
      const auto end = std::uint32_t(chunk.prims.size());
      if (end > begin)
        chunk.segments.push_back(segment_t{begin, end, nullptr, 0, 0, clips.back()});
      begin = end;
    };
    auto walk = [&](auto &self, const slot_t &slot, float dx, float dy) -> void
    {
      for (const auto &entry : slot.entries)
      {
        if (entry.clip != 0)
        {
          close_run();
          if (entry.clip > 0)
            clips.push_back(intersect(clips.back(), {entry.prim.x + dx, entry.prim.y + dy, entry.prim.x + dx + entry.prim.width, entry.prim.y + dy + entry.prim.height}));
          else if (clips.size() > 1)
            clips.pop_back();
          continue;
        }
        if (!entry.child)
        {
          prim_t prim = entry.prim;
          move_prim(prim, dx, dy);
          chunk.prims.push_back(prim);
          continue;
        }
        if (decide_holder(*entry.child))
        {
          close_run();
          chunk.segments.push_back(segment_t{0, 0, entry.child, dx + entry.x, dy + entry.y, clips.back()});
          continue;
        }
        self(self, *entry.child, dx + entry.x, dy + entry.y);
      }
    };
    walk(walk, holder, 0, 0);
    close_run();
    chunk.valid = true;
  }

  // Concatenate the chunks into this frame's prims, one run per segment. A clean chunk is a copy.
  // Whether a subtree laid out at (x, y) can reach the window. Its size bounds its layout, not
  // everything it draws -- a shadow spreads past its box -- so the window is taken a margin larger.
  [[nodiscard]] bool visible(const slot_t &slot, float x, float y, float s) const noexcept
  {
    constexpr float margin = 64;
    return x + float(slot.size.width) * s + margin > 0 && y + float(slot.size.height) * s + margin > 0 && x - margin < float(_logical.width) && y - margin < float(_logical.height);
  }

  // A holder skipped while something below it changed: its chunk, and those of the holders below
  // it on the changed path, no longer match their entries. They are flattened again when next drawn.
  void forget_chunks(slot_t &slot)
  {
    slot.chunk.valid = false;
    for (const auto &entry : slot.entries)
    {
      if (entry.child && entry.child->subtree_dirty)
        forget_chunks(*entry.child);
    }
  }

  // `s` is the scale the holder is drawn at: its own times its ancestors'. `clip` is in the window's
  // units, what the holder's ancestors cut it to.
  void emit(slot_t &holder, float x, float y, float s, const clip_rect_t &clip = no_clip)
  {
    if (holder.subtree_dirty || !holder.chunk.valid)
      flatten(holder);
    for (const auto &segment : holder.chunk.segments)
    {
      clip_rect_t cut = clip;
      if (segment.clip != no_clip)
        cut = intersect(clip, {x + segment.clip[0] * s, y + segment.clip[1] * s, x + segment.clip[2] * s, y + segment.clip[3] * s});
      if (segment.child)
      {
        const float cx = x + segment.x * s;
        const float cy = y + segment.y * s;
        const float cs = s * segment.child->scale;
        // A popup is drawn after everything else, from step().
        if (segment.child->popup)
        {
          _popups.push_back(popup_t{segment.child, cx, cy, cs});
          continue;
        }
        // A chunk wholly outside the window, or cut away entirely, is not drawn (G45).
        if (empty(cut) || !visible(*segment.child, cx, cy, cs))
        {
          if (segment.child->subtree_dirty)
            forget_chunks(*segment.child);
          continue;
        }
        emit(*segment.child, cx, cy, cs, cut);
        continue;
      }
      if (empty(cut))
        continue;
      const auto first = std::uint32_t(_prims.size());
      _prims.insert(_prims.end(), holder.chunk.prims.begin() + segment.begin, holder.chunk.prims.begin() + segment.end);
      run_t run{first, segment.end - segment.begin, x, y, s};
      std::copy(cut.begin(), cut.end(), run.clip);
      _runs.push_back(run);
    }
  }

  // The hit-test list, from the entries as they stand: no view runs for it. A box is cut to the clips
  // around it, and one cut away entirely cannot be hit.
  void collect_items(slot_t &slot, float x, float y, float s, clip_rect_t clip = no_clip)
  {
    // Only what a press can be aimed at, unless someone asked for every box.
    const bool every_box = bool(_config.on_layout);
    if (!every_box && slot.targets == 0)
      return;
    std::vector<clip_rect_t> clips;
    for (const auto &entry : slot.entries)
    {
      if (entry.clip > 0)
      {
        clips.push_back(clip);
        clip = intersect(clip, {x + entry.prim.x * s, y + entry.prim.y * s, x + (entry.prim.x + entry.prim.width) * s, y + (entry.prim.y + entry.prim.height) * s});
        continue;
      }
      if (entry.clip < 0)
      {
        if (!clips.empty())
        {
          clip = clips.back();
          clips.pop_back();
        }
        continue;
      }
      if (entry.child)
      {
        if (entry.child->popup)
          continue;
        const float cs = s * entry.child->scale;
        if (!empty(clip) && visible(*entry.child, x + entry.x * s, y + entry.y * s, cs))
          collect_items(*entry.child, x + entry.x * s, y + entry.y * s, cs, clip);
        continue;
      }
      if (!entry.box || (!every_box && !entry.box->events.source && !entry.box->on_press))
        continue;
      const auto &box = *entry.box;
      const clip_rect_t full{x + entry.x * s, y + entry.y * s, x + (entry.x + entry.prim.width) * s, y + (entry.y + entry.prim.height) * s};
      const clip_rect_t edges = intersect(clip, full);
      if (empty(edges))
        continue;
      const int left = int(std::lround(edges[0]));
      const int top = int(std::lround(edges[1]));
      const rect_t rect{left, top, int(std::lround(edges[2])) - left, int(std::lround(edges[3])) - top};
      const int whole_left = int(std::lround(full[0]));
      const int whole_top = int(std::lround(full[1]));
      const rect_t whole{whole_left, whole_top, int(std::lround(full[2])) - whole_left, int(std::lround(full[3])) - whole_top};
      _items.push_back(draw_item_t{rect, box.color, &box, &slot, entry.ordinal, whole});
    }
  }

  // Each pressable's box as drawn this frame; the first box showing it wins. A change rebuilds the
  // component that shows it, which is how a view learns its own size (G50).
  void update_bounds()
  {
    ++_bounds_frame;
    for (const auto &item : _items)
    {
      auto *source = const_cast<pressable_t *>(item.box->events.source);
      if (!source || source->_bounds_frame == _bounds_frame)
        continue;
      source->_bounds_frame = _bounds_frame;
      if (source->_bounds != item.rect || source->_laid_out != item.whole)
      {
        source->_bounds = item.rect;
        source->_laid_out = item.whole;
        invalidate(*item.slot);
      }
    }
  }

  [[nodiscard]] const draw_item_t *hit(point_t position) const noexcept
  {
    for (auto it = _items.rbegin(); it != _items.rend(); ++it)
    {
      if (it->rect.contains(position) && (it->box->events.source || it->box->on_press))
        return &*it;
    }
    return nullptr;
  }

  // Whether a press on this box is its own. A pressable nothing listens to for presses or drags,
  // and that cannot take focus, lets the press through to what is under it, as the wheel does
  // (G52): a chart that tracks the pointer inside a card that is pressed.
  [[nodiscard]] static bool takes_press(const draw_item_t &item) noexcept
  {
    if (item.box->on_press)
      return true;
    const auto *source = item.box->events.source;
    if (!source)
      return false;
    return source->focusable() || static_cast<const detail::source_t<press_event_t> &>(*source).subscribed() || static_cast<const detail::source_t<drag_event_t> &>(*source).subscribed();
  }

  // A press that lands in a popup stays in it: it may fall through to another of the popup's boxes,
  // never to the backdrop that dismisses it or to anything under it.
  [[nodiscard]] const draw_item_t *press_hit(point_t position) const noexcept
  {
    std::size_t floor = 0;
    bool landed = false;
    for (std::size_t i = _items.size(); i-- > floor;)
    {
      const auto &item = _items[i];
      if (!item.rect.contains(position) || (!item.box->events.source && !item.box->on_press))
        continue;
      if (takes_press(item))
        return &item;
      if (!landed)
      {
        landed = true;
        for (const auto &[first, last] : _popup_items)
        {
          if (i >= first && i < last)
            floor = first;
        }
      }
    }
    return nullptr;
  }

  [[nodiscard]] target_t target_at(point_t position) const noexcept
  {
    const auto *item = press_hit(position);
    if (!item)
      return {};
    if (item->box->events.source)
      return {item->box->events.source, nullptr, -1};
    return {nullptr, item->slot, item->ordinal};
  }

  [[nodiscard]] const draw_item_t *find(const target_t &target) const noexcept
  {
    for (const auto &item : _items)
    {
      if (target.source ? item.box->events.source == target.source : (item.slot == target.slot && item.ordinal == target.ordinal))
        return &item;
    }
    return nullptr;
  }

  // The slot whose view shows `source`, which is the one to rebuild when its state changes.
  slot_t *owner_of(const pressable_t *source) const noexcept
  {
    for (const auto &item : _items)
    {
      if (item.box->events.source == source)
        return item.slot;
    }
    return nullptr;
  }

  void set_flag(const pressable_t *source, bool pressable_t::*flag, bool value)
  {
    auto *mutable_source = const_cast<pressable_t *>(source);
    if (mutable_source->*flag == value)
      return;
    mutable_source->*flag = value;
    if (auto *slot = owner_of(source))
      invalidate(*slot);
  }

  void hover(point_t position)
  {
    const auto *item = hit(position);
    set_hovered(item ? item->box->events.source : nullptr);
  }

  void set_hovered(const pressable_t *source)
  {
    if (source == _hovered)
      return;
    if (_hovered)
    {
      set_flag(_hovered, &pressable_t::_hovered, false);
      hovering(_hovered, false);
    }
    _hovered = source;
    if (source)
      set_flag(source, &pressable_t::_hovered, true);
  }

  struct tracked_owners_t
  {
    slot_t *hovered = nullptr;
    slot_t *focused = nullptr;
    slot_t *held = nullptr;
  };
  [[nodiscard]] tracked_owners_t tracked_owners() const noexcept
  {
    return {_hovered ? owner_of(_hovered) : nullptr, _focused ? owner_of(_focused) : nullptr, _held.source ? owner_of(_held.source) : nullptr};
  }

  void unmount(slot_t &slot, const tracked_owners_t &owners)
  {
    if (owners.hovered == &slot)
      _hovered = nullptr;
    if (owners.focused == &slot)
      _focused = nullptr;
    if (_held.slot == &slot || owners.held == &slot)
      _held = {};
    std::erase(_touched, &slot);
    slot.scope->cancel();
  }

  // Free what has left the tree once nothing in it runs.
  void reap()
  {
    std::erase_if(_dying, [](const std::unique_ptr<slot_t> &slot) { return slot->scope->running() == 0; });
    check_drained();
  }

  void check_drained() noexcept
  {
    if (!_drained_waiter || !_dying.empty() || !_slots.empty())
      return;
    _finish.schedule();
  }

  window_t &_window;
  cursor_t _cursor = cursor_t::arrow;
  vio::event_loop_t *_loop;
  run_config_t _config;
  vio::deferred_call_t _beat;
  vio::deferred_call_t _finish;
  waiter_list_t _ready;
  std::vector<std::coroutine_handle<>> _dead;

  mount_t _root_mount;
  slot_t *_root = nullptr;
  // Keyed by address and type: a component and its first member share an address, never a view.
  struct key_t
  {
    void *component;
    node_t (*view)(const void *);
    friend bool operator==(const key_t &, const key_t &) = default;
  };
  struct key_hash_t
  {
    std::size_t operator()(const key_t &key) const noexcept
    {
      return std::hash<void *>()(key.component) ^ (std::hash<void *>()(reinterpret_cast<void *>(key.view)) << 1);
    }
  };
  static key_t key_of(const mount_t &mount) noexcept
  {
    return {mount.component, mount.view};
  }
  std::unordered_map<key_t, std::unique_ptr<slot_t>, key_hash_t> _slots;
  std::vector<std::unique_ptr<slot_t>> _dying;
  std::vector<slot_t *> _touched;
  std::vector<draw_item_t> _items;
  std::vector<prim_t> _prims;
  std::vector<run_t> _runs;
  text_engine_t _text;
  std::vector<prim_t> _glyphs;
  std::unordered_map<const node_t *, placed_t> _measured;
  frame_stats_t _stats;
  std::chrono::steady_clock::time_point _last_frame;
  font_t _default_font;
  bool _looked_for_font = false;

  extent_t _logical;
  extent_t _framebuffer;
  float _scale = 1;
  double _input_x = 1;
  double _input_y = 1;

  bool _changed = true;
  waiter_t _change_waiter;
  std::coroutine_handle<> _drained_waiter;

  point_t _pointer;
  bool _has_pointer = false;
  const pressable_t *_hovered = nullptr;
  target_t _held;
  const pressable_t *_focused = nullptr;
  const pressable_t *_focus_request = nullptr;
  int _focus_request_frames = 0;
  const pressable_t *_click_source = nullptr;
  std::chrono::steady_clock::time_point _click_time;
  point_t _click_at;
  int _clicks = 1;
  std::uint64_t _bounds_frame = 0;

  // A drag begun by ui::start_drag (G70), and what it is over. `handed_off` once the window system
  // carries it; `refused_off` when it could not, so the edge is not offered it again and again.
  struct drag_session_t
  {
    const pressable_t *source = nullptr;
    std::shared_ptr<const drag_data_t> data;
    drop_action_t allowed = drop_action_t::none;
    bool handed_off = false;
    bool refused_off = false;
  };
  std::optional<drag_session_t> _drag;
  // The press under way became a drag: its release is no press, and its end came from the drag.
  bool _dragged = false;
  const pressable_t *_drop_target = nullptr;
  std::shared_ptr<const drag_data_t> _drop_data;
  drop_action_t _drop_allowed = drop_action_t::none;
  modifier_t _drop_modifiers = modifier_t::none;
  bool _drop_from_here = false;
  drop_action_t _reported_answer = drop_action_t::none;

  // This frame's popups, where they are drawn, in drawing order. `backdrop` is the box a dismissing
  // popup's whole-window hit-test entry points at.
  struct popup_t
  {
    slot_t *slot = nullptr;
    float x = 0;
    float y = 0;
    float s = 1;
    box_t backdrop;
  };
  std::vector<popup_t> _popups;
  // Where each popup's own boxes are in _items, its backdrop not included.
  std::vector<std::pair<std::size_t, std::size_t>> _popup_items;
};

// ---- the GL stopgap (G23): every primitive is a scissored clear. Corners stay square; borders,
// shadows and text are not drawn. It stays until Vulkan has run on every platform CI covers (G32).
namespace
{
constexpr unsigned gl_color_buffer_bit = 0x4000;
constexpr unsigned gl_scissor_test = 0x0C11;

class gl_renderer_t final : public renderer_t
{
public:
  explicit gl_renderer_t(gl_context_t context)
    : _gl(std::move(context))
  {
  }

  bool load()
  {
    if (!_gl.make_current())
      return false;
    _viewport = _gl.proc<void (*)(int, int, int, int)>("glViewport");
    _scissor = _gl.proc<void (*)(int, int, int, int)>("glScissor");
    _clear_color = _gl.proc<void (*)(float, float, float, float)>("glClearColor");
    _clear = _gl.proc<void (*)(unsigned)>("glClear");
    _enable = _gl.proc<void (*)(unsigned)>("glEnable");
    _disable = _gl.proc<void (*)(unsigned)>("glDisable");
    return _viewport && _scissor && _clear_color && _clear && _enable && _disable;
  }

  [[nodiscard]] std::string describe() const override
  {
    return "gl";
  }

  bool render(const display_list_t &list) override
  {
    if (!_gl.make_current())
      return false;
    const int width = list.framebuffer.width;
    const int height = list.framebuffer.height;
    const double sx = list.logical.width > 0 ? double(width) / list.logical.width : 1.0;
    const double sy = list.logical.height > 0 ? double(height) / list.logical.height : 1.0;
    _viewport(0, 0, width, height);
    _disable(gl_scissor_test);
    _clear_color(list.background.r, list.background.g, list.background.b, list.background.a);
    _clear(gl_color_buffer_bit);
    _enable(gl_scissor_test);
    for (const auto &run : list.runs)
    {
      for (std::uint32_t i = run.first; i < run.first + run.count; ++i)
      {
        const auto &prim = list.prims[i];
        if (prim.blur > 0 || prim.kind != prim_kind_t::shape)
          continue;
        const int x0 = int((std::max(run.x + prim.x * run.scale, run.clip[0])) * sx + 0.5);
        const int x1 = int((std::min(run.x + (prim.x + prim.width) * run.scale, run.clip[2])) * sx + 0.5);
        const int y0 = int((std::max(run.y + prim.y * run.scale, run.clip[1])) * sy + 0.5);
        const int y1 = int((std::min(run.y + (prim.y + prim.height) * run.scale, run.clip[3])) * sy + 0.5);
        if (x1 <= x0 || y1 <= y0)
          continue;
        _scissor(x0, height - y1, x1 - x0, y1 - y0);
        _clear_color(prim.fill.r, prim.fill.g, prim.fill.b, prim.fill.a);
        _clear(gl_color_buffer_bit);
      }
    }
    _disable(gl_scissor_test);
    return bool(_gl.swap_buffers());
  }

private:
  gl_context_t _gl;
  void (*_viewport)(int, int, int, int) = nullptr;
  void (*_scissor)(int, int, int, int) = nullptr;
  void (*_clear_color)(float, float, float, float) = nullptr;
  void (*_clear)(unsigned) = nullptr;
  void (*_enable)(unsigned) = nullptr;
  void (*_disable)(unsigned) = nullptr;
};
} // namespace

std::unique_ptr<renderer_t> make_gl_renderer(window_t &window, std::string &why)
{
  auto made = gl_context_t::create(window);
  if (!made)
  {
    why = made.error().msg;
    return nullptr;
  }
  auto renderer = std::make_unique<gl_renderer_t>(std::move(*made));
  if (!renderer->load())
  {
    why = "GL entry points missing";
    return nullptr;
  }
  return renderer;
}

namespace
{
std::unique_ptr<renderer_t> choose_renderer(window_t &window, const run_config_t &config)
{
  auto report = [&config](std::string_view what)
  {
    if (config.on_renderer)
      config.on_renderer(what);
  };
  if (!config.draw)
  {
    report("none: drawing is off");
    return nullptr;
  }
  // A window with nothing native under it (the null backend) can have no surface. Asking anyway
  // would load the Vulkan driver only to fail, which is slow and, under LeakSanitizer, noisy (G18).
  const auto native = window.native();
  if (!native.handle && !native.id)
  {
    report("none: the window has no native surface");
    return nullptr;
  }
  std::string tried;
  if (config.api == render_api_t::automatic || config.api == render_api_t::vulkan)
  {
    std::string why;
    if (auto renderer = make_vulkan_renderer(window, config.vulkan_device, why))
    {
      report(renderer->describe());
      return renderer;
    }
    tried = "vulkan: " + why;
    if (config.api == render_api_t::vulkan)
    {
      report("none: " + tried);
      return nullptr;
    }
  }
  std::string why;
  if (auto renderer = make_gl_renderer(window, why))
  {
    report(renderer->describe() + (tried.empty() ? "" : " (" + tried + ")"));
    return renderer;
  }
  report("none: " + (tried.empty() ? "" : tried + "; ") + "gl: " + why);
  return nullptr;
}

vio::task_t<void> pointer_loop(window_t &window, runtime_t &runtime)
{
  // Drops in the same queue as the pointer: a drag the window system ends is reported before the
  // release it swallowed, and the order matters.
  auto events = window.subscribe<pointer_move_event_t, pointer_button_event_t, pointer_enter_event_t, scroll_event_t, vui::drop_event_t, drag_end_event_t>();
  while (auto event = co_await events)
  {
    if (auto *move = std::get_if<pointer_move_event_t>(&*event))
      runtime.pointer_moved(move->position, move->modifiers);
    else if (auto *drop = std::get_if<vui::drop_event_t>(&*event))
      runtime.window_drop(*drop);
    else if (auto *ended = std::get_if<drag_end_event_t>(&*event))
      runtime.window_drag_ended(*ended);
    else if (auto *scroll = std::get_if<scroll_event_t>(&*event))
      runtime.wheel(*scroll);
    else if (auto *button = std::get_if<pointer_button_event_t>(&*event))
      runtime.button(*button);
    else if (auto *enter = std::get_if<pointer_enter_event_t>(&*event); enter && !enter->entered)
      runtime.pointer_left();
  }
}

vio::task_t<void> key_loop(window_t &window, runtime_t &runtime)
{
  auto events = window.subscribe<key_event_t, text_event_t>();
  while (auto event = co_await events)
  {
    if (auto *key = std::get_if<key_event_t>(&*event))
      runtime.key(*key);
    else
      runtime.text(std::get<text_event_t>(*event));
  }
}

// A resize or an expose needs a new frame although no view changed; an expose redraws the display
// list as it stands, since the chunks are clean. The end of this subscription is how the frame loop
// learns the window closed while it was waiting for a change.
vio::task_t<void> repaint_loop(window_t &window, runtime_t &runtime)
{
  auto events = window.subscribe<resize_event_t, expose_event_t>();
  while (co_await events)
    runtime.wake_frame();
  runtime.wake_frame();
}
} // namespace

vio::task_t<void> run_root(window_t &window, mount_t root, run_config_t config)
{
  runtime_t runtime(window, std::move(config));
  runtime.set_root(root);
  auto renderer = choose_renderer(window, runtime.config());
  auto pointer = pointer_loop(window, runtime);
  auto keys = key_loop(window, runtime);
  auto repaints = repaint_loop(window, runtime);
  while (window)
  {
    co_await runtime.changed();
    auto frame = co_await window.frame();
    if (!frame)
      break;
    runtime.step(*frame);
    const auto rendering = std::chrono::steady_clock::now();
    if (renderer && !renderer->render(runtime.display_list()))
      renderer.reset();
    runtime.stats().render = std::chrono::steady_clock::now() - rendering;
    if (runtime.config().on_stats)
      runtime.config().on_stats(runtime.stats());
  }
  renderer.reset();
  runtime.shutdown();
  co_await std::move(pointer);
  co_await std::move(keys);
  co_await std::move(repaints);
  co_await runtime.drained();
}

void ready(runtime_t &runtime, waiter_t &waiter) noexcept
{
  runtime.ready(waiter);
}

void invalidate_component(runtime_t &runtime, void *component, node_t (*view)(const void *)) noexcept
{
  runtime.invalidate_component(component, view);
}

void request_focus(runtime_t &runtime, const pressable_t *source) noexcept
{
  runtime.request_focus(source);
}

bool start_drag(runtime_t &runtime, std::vector<std::string> paths, drop_action_t allowed)
{
  return runtime.start_drag(std::move(paths), allowed);
}

bool scope_cancelled(const scope_t &scope) noexcept
{
  return scope.cancelled();
}

runtime_t &scope_runtime(scope_t &scope) noexcept
{
  return *scope._runtime;
}

vio::cancellation_t &scope_token(scope_t &scope) noexcept
{
  return scope._token;
}

void scope_woke(scope_t &scope) noexcept
{
  scope.invalidate();
}

void scope_refused(scope_t &scope, std::coroutine_handle<> frame) noexcept
{
  scope.forget(frame);
  scope._runtime->bury(frame);
}

void scope_finished(scope_t &scope, std::coroutine_handle<> frame) noexcept
{
  scope.forget(frame);
  scope._runtime->bury(frame);
}
} // namespace detail

scope_t::scope_t(scope_t &parent)
  : _runtime(parent._runtime)
  , _slot(parent._slot)
{
  if (parent._token.is_cancelled())
    _token.cancel();
  else
    _on_parent = parent._token.register_callback([this] { _token.cancel(); });
}

void scope_t::spawn(behaviour_t behaviour)
{
  start(behaviour.release());
}

void scope_t::start(std::coroutine_handle<> frame)
{
  if (!frame)
    return;
  if (_token.is_cancelled())
  {
    frame.destroy();
    return;
  }
  _running.push_back(frame);
  invalidate();
  frame.resume();
}

void scope_t::invalidate() noexcept
{
  if (_slot)
    _runtime->invalidate(*_slot);
}

vio::event_loop_t &scope_t::loop() const noexcept
{
  return _runtime->loop();
}

vui::window_t &scope_t::window() const noexcept
{
  return _runtime->window();
}

point_t scope_t::to_logical(point_t position) const noexcept
{
  return _runtime->to_logical(position);
}

extent_t scope_t::viewport() const noexcept
{
  return _runtime->viewport();
}

void scope_t::cancel()
{
  _token.cancel();
}

void scope_t::forget(std::coroutine_handle<> frame) noexcept
{
  std::erase(_running, frame);
  if (_running.empty() && _joiner)
    _runtime->ready(*std::exchange(_joiner, nullptr));
}
} // namespace vui::ui
