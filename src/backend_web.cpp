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

// The browser backend (WEB_PLAN.md, part 2): a window is a <canvas>. Every browser callback records
// what happened, as the other backends' callbacks do, and then runs a loop turn on the spot with
// vio::loop_run_nested(), so the waiters it woke are resumed before the callback returns (vio D69).
// Frames come from requestAnimationFrame, which a hidden tab does not call.

#if defined(__EMSCRIPTEN__)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <emscripten.h>
#include <emscripten/html5.h>

#include <vio/host_loop.h>

#include "vui/backends.h"
#include "vui/detail/core.h"

EM_JS_DEPS(vui_web, "$UTF8ToString");

// The canvas `id`, made at `width` x `height` CSS pixels unless the page already has one.
EM_JS(void, vui_web_make_canvas, (const char *id, int width, int height), {
  const name = UTF8ToString(id);
  let canvas = document.getElementById(name);
  if (!canvas) {
    canvas = document.createElement('canvas');
    canvas.id = name;
    canvas.style.width = width + 'px';
    canvas.style.height = height + 'px';
    document.body.appendChild(canvas);
    canvas.dataset.vuiMade = '1';
  }
  canvas.tabIndex = 0;
  canvas.style.outline = 'none';
  canvas.style.touchAction = 'none';
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
});

EM_JS(void, vui_web_remove_canvas, (const char *id), {
  const canvas = document.getElementById(UTF8ToString(id));
  if (canvas && canvas.dataset.vuiMade === '1')
    canvas.remove();
});

// The canvas's box in the page, in CSS pixels: left, top, width, height.
EM_JS(void, vui_web_canvas_box, (const char *id, double *out), {
  const canvas = document.getElementById(UTF8ToString(id));
  const box = canvas ? canvas.getBoundingClientRect() : {left : 0, top : 0, width : 0, height : 0};
  HEAPF64[(out >> 3) + 0] = box.left;
  HEAPF64[(out >> 3) + 1] = box.top;
  HEAPF64[(out >> 3) + 2] = box.width;
  HEAPF64[(out >> 3) + 3] = box.height;
});

EM_JS(void, vui_web_set_cursor, (const char *id, const char *cursor), {
  const canvas = document.getElementById(UTF8ToString(id));
  if (canvas)
    canvas.style.cursor = UTF8ToString(cursor);
});

EM_JS(void, vui_web_focus, (const char *id), {
  const canvas = document.getElementById(UTF8ToString(id));
  if (canvas)
    canvas.focus();
});

EM_JS(void, vui_web_write_clipboard, (const char *text), {
  if (navigator.clipboard)
    navigator.clipboard.writeText(UTF8ToString(text)).catch(() => {});
});

EM_JS(int, vui_web_hidden, (), { return document.hidden ? 1 : 0; });

namespace vui::detail
{
namespace
{
class web_display_t;

modifier_t modifiers_of(bool shift, bool control, bool alt, bool meta)
{
  modifier_t m = modifier_t::none;
  if (shift)
    m = m | modifier_t::shift;
  if (control)
    m = m | modifier_t::control;
  if (alt)
    m = m | modifier_t::alt;
  if (meta)
    m = m | modifier_t::super;
  return m;
}

// KeyboardEvent.code: the physical key, whatever the layout.
key_t key_of(std::string_view code)
{
  struct entry_t
  {
    const char *code;
    key_t key;
  };
  static const entry_t table[] = {
    {"Space", key_t::space},
    {"Quote", key_t::apostrophe},
    {"Comma", key_t::comma},
    {"Minus", key_t::minus},
    {"Period", key_t::period},
    {"Slash", key_t::slash},
    {"Semicolon", key_t::semicolon},
    {"Equal", key_t::equal},
    {"BracketLeft", key_t::left_bracket},
    {"Backslash", key_t::backslash},
    {"BracketRight", key_t::right_bracket},
    {"Backquote", key_t::grave_accent},
    {"Escape", key_t::escape},
    {"Enter", key_t::enter},
    {"Tab", key_t::tab},
    {"Backspace", key_t::backspace},
    {"Insert", key_t::insert},
    {"Delete", key_t::delete_key},
    {"ArrowRight", key_t::right},
    {"ArrowLeft", key_t::left},
    {"ArrowDown", key_t::down},
    {"ArrowUp", key_t::up},
    {"PageUp", key_t::page_up},
    {"PageDown", key_t::page_down},
    {"Home", key_t::home},
    {"End", key_t::end},
    {"CapsLock", key_t::caps_lock},
    {"ScrollLock", key_t::scroll_lock},
    {"NumLock", key_t::num_lock},
    {"PrintScreen", key_t::print_screen},
    {"Pause", key_t::pause},
    {"F1", key_t::f1},
    {"F2", key_t::f2},
    {"F3", key_t::f3},
    {"F4", key_t::f4},
    {"F5", key_t::f5},
    {"F6", key_t::f6},
    {"F7", key_t::f7},
    {"F8", key_t::f8},
    {"F9", key_t::f9},
    {"F10", key_t::f10},
    {"F11", key_t::f11},
    {"F12", key_t::f12},
    {"NumpadDecimal", key_t::keypad_decimal},
    {"NumpadDivide", key_t::keypad_divide},
    {"NumpadMultiply", key_t::keypad_multiply},
    {"NumpadSubtract", key_t::keypad_subtract},
    {"NumpadAdd", key_t::keypad_add},
    {"NumpadEnter", key_t::keypad_enter},
    {"NumpadEqual", key_t::keypad_equal},
    {"ShiftLeft", key_t::left_shift},
    {"ControlLeft", key_t::left_control},
    {"AltLeft", key_t::left_alt},
    {"MetaLeft", key_t::left_super},
    {"ShiftRight", key_t::right_shift},
    {"ControlRight", key_t::right_control},
    {"AltRight", key_t::right_alt},
    {"MetaRight", key_t::right_super},
    {"ContextMenu", key_t::menu},
  };
  if (code.size() == 4 && code.starts_with("Key") && code[3] >= 'A' && code[3] <= 'Z')
    return static_cast<key_t>(static_cast<int>(key_t::a) + (code[3] - 'A'));
  if (code.size() == 6 && code.starts_with("Digit") && code[5] >= '0' && code[5] <= '9')
    return static_cast<key_t>(static_cast<int>(key_t::digit_0) + (code[5] - '0'));
  if (code.size() == 7 && code.starts_with("Numpad") && code[6] >= '0' && code[6] <= '9')
    return static_cast<key_t>(static_cast<int>(key_t::keypad_0) + (code[6] - '0'));
  for (const auto &e : table)
  {
    if (code == e.code)
      return e.key;
  }
  return key_t::unknown;
}

// The text a key press types: KeyboardEvent.key when it is one printable character. No input
// method yet (WEB_PLAN.md, part 2).
std::string typed_text(const EmscriptenKeyboardEvent &e)
{
  std::string_view key = e.key;
  if (key.empty() || e.ctrlKey || e.metaKey)
    return {};
  const auto lead = static_cast<unsigned char>(key[0]);
  const std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
  if (length == 0 || key.size() != length || lead < 0x20 || lead == 0x7F)
    return {};
  return std::string(key);
}

// Keys the browser keeps for itself: reloading, the developer tools, tabs and windows.
bool browser_keeps(const EmscriptenKeyboardEvent &e, key_t key)
{
  if (key == key_t::f5 || key == key_t::f11 || key == key_t::f12)
    return true;
  if (!e.ctrlKey && !e.metaKey)
    return false;
  switch (key)
  {
  case key_t::r:
  case key_t::l:
  case key_t::t:
  case key_t::w:
  case key_t::n:
  case key_t::q:
  case key_t::tab:
    return true;
  case key_t::i:
  case key_t::j:
    return e.shiftKey;
  default:
    return false;
  }
}

const char *css_cursor(cursor_t cursor)
{
  switch (cursor)
  {
  case cursor_t::arrow:
    return "default";
  case cursor_t::text:
    return "text";
  case cursor_t::hand:
    return "pointer";
  case cursor_t::crosshair:
    return "crosshair";
  case cursor_t::resize_horizontal:
    return "ew-resize";
  case cursor_t::resize_vertical:
    return "ns-resize";
  case cursor_t::resize_all:
    return "move";
  case cursor_t::not_allowed:
    return "not-allowed";
  case cursor_t::wait:
    return "wait";
  case cursor_t::hidden:
    return "none";
  }
  return "default";
}

button_t button_of(unsigned short button)
{
  switch (button)
  {
  case 0:
    return button_t::left;
  case 1:
    return button_t::middle;
  case 2:
    return button_t::right;
  case 3:
    return button_t::back;
  case 4:
    return button_t::forward;
  default:
    return button_t::other;
  }
}

class web_window_t;

// An animation frame cannot be withdrawn once asked for, so it holds this rather than the window,
// and a window that closes first only clears it.
struct frame_request_t
{
  web_window_t *window = nullptr;
};

class web_window_t final : public window_backend_t
{
public:
  web_window_t(web_display_t &display, window_impl_t &window, std::string id)
    : _display(display)
    , _window(window)
    , _id(std::move(id))
    , _selector("#" + _id)
  {
  }
  ~web_window_t() override;

  void start(const window_config_t &config);

  void set_title(std::string_view title) override
  {
    emscripten_set_window_title(std::string(title).c_str());
  }
  int set_size(extent_t size) override
  {
    emscripten_set_element_css_size(_selector.c_str(), size.width, size.height);
    measure();
    return 0;
  }
  int set_position(point_t) override
  {
    return vio::errc::not_supported;
  }
  [[nodiscard]] std::expected<point_t, int> position() const override
  {
    return std::unexpected(vio::errc::not_supported);
  }
  void set_size_limits(extent_t, extent_t) override
  {
  }
  void set_visible(bool visible) override
  {
    _shown = visible;
    _window.on_visible(_shown && !vui_web_hidden());
  }
  void set_fullscreen(bool fullscreen) override
  {
    if (fullscreen)
    {
      EmscriptenFullscreenStrategy strategy{};
      strategy.scaleMode = EMSCRIPTEN_FULLSCREEN_SCALE_STRETCH;
      strategy.canvasResolutionScaleMode = EMSCRIPTEN_FULLSCREEN_CANVAS_SCALE_HIDEF;
      strategy.filteringMode = EMSCRIPTEN_FULLSCREEN_FILTERING_DEFAULT;
      emscripten_request_fullscreen_strategy(_selector.c_str(), true, &strategy);
    }
    else
    {
      emscripten_exit_fullscreen();
    }
  }
  void minimize() override
  {
  }
  void maximize() override
  {
  }
  void restore() override
  {
  }
  void focus() override
  {
    vui_web_focus(_id.c_str());
  }
  void set_cursor(cursor_t cursor) override
  {
    vui_web_set_cursor(_id.c_str(), css_cursor(cursor));
  }
  void frame_wanted() override
  {
    if (_frame)
      return;
    _frame = new frame_request_t{this};
    emscripten_request_animation_frame(&web_window_t::on_animation_frame, _frame);
  }
  [[nodiscard]] native_window_t native() const override
  {
    native_window_t native;
    native.platform = platform_t::web;
    native.handle = const_cast<char *>(_selector.c_str());
    return native;
  }

  // The canvas's CSS size and the device pixels under it, which the canvas is resized to.
  void measure()
  {
    double width = 0;
    double height = 0;
    emscripten_get_element_css_size(_selector.c_str(), &width, &height);
    const double ratio = emscripten_get_device_pixel_ratio();
    const extent_t size{int(std::lround(width)), int(std::lround(height))};
    const extent_t framebuffer{std::max(1, int(std::lround(width * ratio))), std::max(1, int(std::lround(height * ratio)))};
    int current_width = 0;
    int current_height = 0;
    emscripten_get_canvas_element_size(_selector.c_str(), &current_width, &current_height);
    if (current_width != framebuffer.width || current_height != framebuffer.height)
      emscripten_set_canvas_element_size(_selector.c_str(), framebuffer.width, framebuffer.height);
    _window.on_resize(size, framebuffer, float(ratio));
  }

  // A point in the page to the canvas's CSS pixels, and whether it is inside the canvas.
  point_t to_local(double client_x, double client_y, bool *inside = nullptr) const
  {
    double box[4] = {};
    vui_web_canvas_box(_id.c_str(), box);
    const point_t at{client_x - box[0], client_y - box[1]};
    if (inside)
      *inside = at.x >= 0 && at.y >= 0 && at.x < box[2] && at.y < box[3];
    return at;
  }

  void page_visibility(bool hidden)
  {
    _window.on_visible(_shown && !hidden);
  }

  window_impl_t &impl() noexcept
  {
    return _window;
  }

  bool pointer_inside = false;
  unsigned buttons_held = 0;

private:
  static EM_BOOL on_animation_frame(double, void *request);
  static EM_BOOL on_mouse_down(int, const EmscriptenMouseEvent *event, void *self);
  static EM_BOOL on_wheel(int, const EmscriptenWheelEvent *event, void *self);

  web_display_t &_display;
  window_impl_t &_window;
  std::string _id;
  std::string _selector;
  frame_request_t *_frame = nullptr;
  bool _shown = true;
};

class web_display_t final : public display_backend_t
{
public:
  explicit web_display_t(display_impl_t &display)
    : _display(display)
  {
    const char *window = EMSCRIPTEN_EVENT_TARGET_WINDOW;
    emscripten_set_keydown_callback(window, this, true, &web_display_t::on_key);
    emscripten_set_keyup_callback(window, this, true, &web_display_t::on_key);
    emscripten_set_mousemove_callback(window, this, true, &web_display_t::on_mouse_move);
    emscripten_set_mouseup_callback(window, this, true, &web_display_t::on_mouse_up);
    emscripten_set_resize_callback(window, this, false, &web_display_t::on_resize);
    emscripten_set_blur_callback(window, this, false, &web_display_t::on_focus);
    emscripten_set_focus_callback(window, this, false, &web_display_t::on_focus);
    emscripten_set_visibilitychange_callback(this, false, &web_display_t::on_visibility);
  }

  ~web_display_t() override
  {
    const char *window = EMSCRIPTEN_EVENT_TARGET_WINDOW;
    emscripten_set_keydown_callback(window, nullptr, true, nullptr);
    emscripten_set_keyup_callback(window, nullptr, true, nullptr);
    emscripten_set_mousemove_callback(window, nullptr, true, nullptr);
    emscripten_set_mouseup_callback(window, nullptr, true, nullptr);
    emscripten_set_resize_callback(window, nullptr, false, nullptr);
    emscripten_set_blur_callback(window, nullptr, false, nullptr);
    emscripten_set_focus_callback(window, nullptr, false, nullptr);
    emscripten_set_visibilitychange_callback(nullptr, false, nullptr);
  }

  [[nodiscard]] const char *name() const noexcept override
  {
    return "web";
  }
  [[nodiscard]] platform_t platform() const noexcept override
  {
    return platform_t::web;
  }
  std::expected<std::unique_ptr<window_backend_t>, int> create_window(window_impl_t &window, const window_config_t &config) override
  {
    // The first window takes the page's own canvas, `#canvas`, when it has one.
    std::string id = _next == 0 ? "canvas" : "vui-canvas-" + std::to_string(_next);
    ++_next;
    auto made = std::make_unique<web_window_t>(*this, window, std::move(id));
    _windows.push_back(made.get());
    if (!_focused)
      _focused = made.get();
    made->start(config);
    return made;
  }
  void dispatch() override
  {
  }
  void set_clipboard_text(std::string text) override
  {
    vui_web_write_clipboard(text.c_str());
  }
  void request_clipboard_text() override
  {
    // Reading the clipboard needs a permission prompt or a paste event (WEB_PLAN.md); not yet.
    _display.clipboard_arrived(std::unexpected(vio::errc::not_supported));
  }
  [[nodiscard]] void *native_display() const noexcept override
  {
    return nullptr;
  }

  void forget(web_window_t *window) noexcept
  {
    _windows.erase(std::remove(_windows.begin(), _windows.end(), window), _windows.end());
    if (_focused == window)
      _focused = _windows.empty() ? nullptr : _windows.front();
    if (_captured == window)
      _captured = nullptr;
  }

  void pressed_in(web_window_t *window)
  {
    _captured = window;
    if (_focused != window)
    {
      if (_focused)
        _focused->impl().on_input(focus_event_t{false});
      _focused = window;
      window->impl().on_input(focus_event_t{true});
    }
  }

  // What the browser reported is recorded; now resume whoever it woke.
  void turn() noexcept
  {
    vio::loop_run_nested(*_display.loop);
  }

private:
  static EM_BOOL on_key(int type, const EmscriptenKeyboardEvent *event, void *context)
  {
    auto &self = *static_cast<web_display_t *>(context);
    auto *window = self._focused;
    if (!window)
      return false;
    const key_t key = key_of(event->code);
    if (browser_keeps(*event, key))
      return false;
    const bool pressed = type == EMSCRIPTEN_EVENT_KEYDOWN;
    key_event_t out{key, event->keyCode, modifiers_of(event->shiftKey, event->ctrlKey, event->altKey, event->metaKey), pressed, pressed && event->repeat};
    window->impl().on_input(out);
    if (pressed)
    {
      if (auto text = typed_text(*event); !text.empty())
        window->impl().on_input(text_event_t{std::move(text)});
    }
    self.turn();
    return true;
  }

  static EM_BOOL on_mouse_move(int, const EmscriptenMouseEvent *event, void *context)
  {
    auto &self = *static_cast<web_display_t *>(context);
    const auto modifiers = modifiers_of(event->shiftKey, event->ctrlKey, event->altKey, event->metaKey);
    for (auto *window : self._windows)
    {
      bool inside = false;
      const point_t at = window->to_local(event->clientX, event->clientY, &inside);
      if (inside != window->pointer_inside && window != self._captured)
      {
        window->pointer_inside = inside;
        window->impl().on_input(pointer_enter_event_t{inside, at});
      }
      if (inside || window == self._captured)
        window->impl().on_input(pointer_move_event_t{at, modifiers});
    }
    self.turn();
    return false;
  }

  static EM_BOOL on_mouse_up(int, const EmscriptenMouseEvent *event, void *context)
  {
    auto &self = *static_cast<web_display_t *>(context);
    auto *window = self._captured;
    if (!window)
      return false;
    const point_t at = window->to_local(event->clientX, event->clientY);
    window->impl().on_input(pointer_button_event_t{button_of(event->button), false, at, modifiers_of(event->shiftKey, event->ctrlKey, event->altKey, event->metaKey)});
    window->buttons_held &= ~(1u << event->button);
    if (window->buttons_held == 0)
      self._captured = nullptr;
    self.turn();
    return true;
  }

  static EM_BOOL on_resize(int, const EmscriptenUiEvent *, void *context)
  {
    auto &self = *static_cast<web_display_t *>(context);
    for (auto *window : self._windows)
      window->measure();
    self.turn();
    return false;
  }

  static EM_BOOL on_focus(int type, const EmscriptenFocusEvent *, void *context)
  {
    auto &self = *static_cast<web_display_t *>(context);
    if (self._focused)
      self._focused->impl().on_input(focus_event_t{type == EMSCRIPTEN_EVENT_FOCUS});
    self.turn();
    return false;
  }

  static EM_BOOL on_visibility(int, const EmscriptenVisibilityChangeEvent *event, void *context)
  {
    auto &self = *static_cast<web_display_t *>(context);
    for (auto *window : self._windows)
      window->page_visibility(event->hidden);
    self.turn();
    return false;
  }

  display_impl_t &_display;
  std::vector<web_window_t *> _windows;
  web_window_t *_focused = nullptr;
  web_window_t *_captured = nullptr;
  int _next = 0;
};

void web_window_t::start(const window_config_t &config)
{
  vui_web_make_canvas(_id.c_str(), config.size.width, config.size.height);
  if (_id == "canvas")
    emscripten_set_window_title(config.title.c_str());
  emscripten_set_mousedown_callback(_selector.c_str(), this, true, &web_window_t::on_mouse_down);
  emscripten_set_wheel_callback(_selector.c_str(), this, false, &web_window_t::on_wheel);
  measure();
  _shown = config.visible;
  _window.on_visible(_shown && !vui_web_hidden());
}

web_window_t::~web_window_t()
{
  if (_frame)
    _frame->window = nullptr;
  emscripten_set_mousedown_callback(_selector.c_str(), nullptr, true, nullptr);
  emscripten_set_wheel_callback(_selector.c_str(), nullptr, false, nullptr);
  _display.forget(this);
  vui_web_remove_canvas(_id.c_str());
}

EM_BOOL web_window_t::on_animation_frame(double, void *context)
{
  std::unique_ptr<frame_request_t> request(static_cast<frame_request_t *>(context));
  auto *self = request->window;
  if (!self)
    return false;
  self->_frame = nullptr;
  self->_window.on_frame(now_ns(self->_window));
  self->_display.turn();
  return false;
}

EM_BOOL web_window_t::on_mouse_down(int, const EmscriptenMouseEvent *event, void *context)
{
  auto *self = static_cast<web_window_t *>(context);
  const point_t at = self->to_local(event->clientX, event->clientY);
  self->_display.pressed_in(self);
  self->buttons_held |= 1u << event->button;
  vui_web_focus(self->_id.c_str());
  self->_window.on_input(pointer_button_event_t{button_of(event->button), true, at, modifiers_of(event->shiftKey, event->ctrlKey, event->altKey, event->metaKey)});
  self->_display.turn();
  return true;
}

EM_BOOL web_window_t::on_wheel(int, const EmscriptenWheelEvent *event, void *context)
{
  auto *self = static_cast<web_window_t *>(context);
  // In notches, up and left positive, as Windows reports them: a pixel delta of 100 is one notch.
  const double unit = event->deltaMode == DOM_DELTA_PIXEL ? 100.0 : event->deltaMode == DOM_DELTA_LINE ? 3.0 : 1.0;
  const auto &mouse = event->mouse;
  scroll_event_t out{-event->deltaX / unit, -event->deltaY / unit, self->to_local(mouse.clientX, mouse.clientY), modifiers_of(mouse.shiftKey, mouse.ctrlKey, mouse.altKey, mouse.metaKey)};
  self->_window.on_input(out);
  self->_display.turn();
  return true;
}
} // namespace

std::expected<std::unique_ptr<display_backend_t>, int> make_web_backend(display_impl_t &display)
{
  return std::make_unique<web_display_t>(display);
}
} // namespace vui::detail

#endif
