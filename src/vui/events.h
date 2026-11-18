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

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace vui
{
struct extent_t
{
  int width = 0;
  int height = 0;
  friend bool operator==(const extent_t &, const extent_t &) = default;
};

struct point_t
{
  double x = 0;
  double y = 0;
  friend bool operator==(const point_t &, const point_t &) = default;
};

enum class modifier_t : std::uint8_t
{
  none = 0,
  shift = 1,
  control = 2,
  alt = 4,
  super = 8,
  caps_lock = 16,
  num_lock = 32
};

constexpr modifier_t operator|(modifier_t a, modifier_t b) noexcept
{
  return static_cast<modifier_t>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

constexpr modifier_t operator&(modifier_t a, modifier_t b) noexcept
{
  return static_cast<modifier_t>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

constexpr bool has(modifier_t set, modifier_t flag) noexcept
{
  return (set & flag) != modifier_t::none;
}

enum class key_t : std::uint16_t
{
  unknown,
  a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x, y, z,
  digit_0, digit_1, digit_2, digit_3, digit_4, digit_5, digit_6, digit_7, digit_8, digit_9,
  space, apostrophe, comma, minus, period, slash, semicolon, equal, left_bracket, backslash,
  right_bracket, grave_accent,
  escape, enter, tab, backspace, insert, delete_key, right, left, down, up, page_up, page_down,
  home, end, caps_lock, scroll_lock, num_lock, print_screen, pause,
  f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12,
  keypad_0, keypad_1, keypad_2, keypad_3, keypad_4, keypad_5, keypad_6, keypad_7, keypad_8,
  keypad_9, keypad_decimal, keypad_divide, keypad_multiply, keypad_subtract, keypad_add,
  keypad_enter, keypad_equal,
  left_shift, left_control, left_alt, left_super, right_shift, right_control, right_alt,
  right_super, menu
};

enum class button_t : std::uint8_t
{
  left,
  right,
  middle,
  back,
  forward,
  other
};

struct key_event_t
{
  key_t key = key_t::unknown;
  std::uint32_t scancode = 0;
  modifier_t modifiers = modifier_t::none;
  bool pressed = false;
  bool repeat = false;
};

struct text_event_t
{
  std::string text;
};

struct pointer_move_event_t
{
  point_t position;
  modifier_t modifiers = modifier_t::none;
};

struct pointer_button_event_t
{
  button_t button = button_t::left;
  bool pressed = false;
  point_t position;
  modifier_t modifiers = modifier_t::none;
};

struct scroll_event_t
{
  double dx = 0;
  double dy = 0;
  point_t position;
  modifier_t modifiers = modifier_t::none;
};

struct pointer_enter_event_t
{
  bool entered = false;
  point_t position;
};

struct focus_event_t
{
  bool focused = false;
};

struct resize_event_t
{
  extent_t size;
  extent_t framebuffer;
  float scale = 1.0f;
};

// The user asked to close the window, by its close button, Alt+F4 or the like. The window stays
// open until the program closes it.
struct close_request_t
{
};

// The window's contents were lost -- an expose on X11, WM_PAINT, the window shown again -- and must
// be drawn again although nothing in the program changed. The next frame() has `damaged` set; this
// is how a program that only awaits frames when something changed learns that it should.
struct expose_event_t
{
};

// ---- drag and drop (G70)

// What a drop does with what was dragged. The values are Windows' DROPEFFECT bits, which is what
// they become there; a set of them is what a drag allows.
enum class drop_action_t : std::uint8_t
{
  none = 0,
  copy = 1,
  move = 2,
  link = 4
};

constexpr drop_action_t operator|(drop_action_t a, drop_action_t b) noexcept
{
  return static_cast<drop_action_t>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

constexpr drop_action_t operator&(drop_action_t a, drop_action_t b) noexcept
{
  return static_cast<drop_action_t>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

constexpr bool has(drop_action_t set, drop_action_t action) noexcept
{
  return (set & action) != drop_action_t::none;
}

// What is being dragged: files and folders by path, UTF-8, and text.
struct drag_data_t
{
  std::vector<std::string> paths;
  std::string text;
};

enum class drop_phase_t : std::uint8_t
{
  enter,
  over,
  leave,
  drop
};

// Something dragged over the window by the window system: from another program, or from this one
// once its drag left the window it started in. Answer with window_t::accept_drop(); the answer
// given by the time of the next event, or the drop, is what the dragging program is told. `data`
// is shared by every event of one drag; a window system that reveals it only on the drop gives it
// then, and none before.
struct drop_event_t
{
  drop_phase_t phase = drop_phase_t::enter;
  point_t position;
  modifier_t modifiers = modifier_t::none;
  // What the dragging program allows.
  drop_action_t allowed = drop_action_t::none;
  std::shared_ptr<const drag_data_t> data;
};

// A drag this window started with window_t::start_drag() is over, and what the target did with it:
// `none` when it was refused or cancelled.
struct drag_end_event_t
{
  drop_action_t action = drop_action_t::none;
};

// Everything a window reports, and what window_t::subscribe<Ts...>() selects from.
using window_event_t = std::variant<key_event_t, text_event_t, pointer_move_event_t, pointer_button_event_t, scroll_event_t, pointer_enter_event_t, focus_event_t, resize_event_t, close_request_t, expose_event_t,
                                    drop_event_t, drag_end_event_t>;

struct frame_t
{
  extent_t size;
  extent_t framebuffer;
  float scale = 1.0f;
  std::uint64_t number = 0;
  std::int64_t time_ns = 0;
  bool damaged = false;
};

enum class cursor_t : std::uint8_t
{
  arrow,
  text,
  hand,
  crosshair,
  resize_horizontal,
  resize_vertical,
  resize_all,
  not_allowed,
  wait,
  hidden
};
} // namespace vui
