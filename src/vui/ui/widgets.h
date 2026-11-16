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

// The first widgets (G46): a button, a checkbox and a combobox; then text (G50): a label, a line
// edit and a text edit. Each is an ordinary component --
// nothing here reaches past what ui.h offers any program -- so they are also the worked example of
// how to write one.
//
// They are controlled: the value lives in the parent and comes down as props; a change comes up as
// an event, which the parent applies to its own state. The parent's view mounts the widget with the
// value it holds, so the parent is the one place the value is, and setting it from code is
// assigning a member:
//
//   struct settings_t
//   {
//     bool notify = true;
//     ui::checkbox_t notify_box;
//
//     ui::node_t view() const
//     {
//       return ui::mount(notify_box, {.label = "Notify me", .checked = notify});
//     }
//     ui::behaviour_t behave(ui::scope_t &scope)
//     {
//       auto changes = ui::subscribe<ui::toggle_event_t>(notify_box.changed);
//       while (auto change = co_await changes)
//         notify = change->checked;
//     }
//   };
//
// What a widget keeps itself is only what no parent cares about: whether a menu is open, which row
// the keyboard is on.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vui/ui/ui.h"

namespace vui::ui
{
class checkbox_t;
class combobox_t;
class line_edit_t;
class text_edit_t;
class context_menu_t;

// Colours and sizes every widget draws with. One dark theme for now; making it a prop, or something
// a subtree inherits, is open.
struct theme_t
{
  color_t text{0.92f, 0.92f, 0.94f, 1};
  color_t text_disabled{0.5f, 0.5f, 0.53f, 1};
  color_t control{0.22f, 0.22f, 0.25f, 1};
  color_t control_hovered{0.27f, 0.27f, 0.31f, 1};
  color_t control_held{0.17f, 0.17f, 0.19f, 1};
  color_t control_disabled{0.18f, 0.18f, 0.2f, 1};
  color_t border{0.36f, 0.36f, 0.4f, 1};
  color_t border_hovered{0.5f, 0.5f, 0.56f, 1};
  color_t accent{0.24f, 0.5f, 0.95f, 1};
  color_t accent_hovered{0.33f, 0.58f, 1.0f, 1};
  color_t accent_held{0.18f, 0.4f, 0.8f, 1};
  color_t focus_ring{0.55f, 0.75f, 1.0f, 1};
  color_t menu{0.2f, 0.2f, 0.23f, 1};
  color_t text_muted{0.66f, 0.66f, 0.7f, 1};
  color_t field{0.13f, 0.13f, 0.15f, 1};
  color_t selection{0.24f, 0.45f, 0.85f, 0.45f};
  color_t caret{0.92f, 0.92f, 0.94f, 1};
  color_t scrollbar{1, 1, 1, 0.22f};
  float text_size = 14;
  float mono_size = 13;
  float radius = 6;
};

[[nodiscard]] const theme_t &theme() noexcept;

// The fixed-width font the text widgets use: font_t::system_monospace(), else the UI font, looked
// up once.
[[nodiscard]] const font_t &monospace_font();

// A label: text that is only read. Each line of `text` is a line of the label. Stateless, so it is
// a function rather than a component: `ui::label("Name")`, `ui::label({.text = "3 files",
// .muted = true})`.
struct label_props_t
{
  std::string text;
  // The theme's quieter text colour, for what is secondary.
  bool muted = false;
  // In the fixed-width font: a value that lines up with a text edit, a path, a number column.
  bool monospace = false;
  bool enabled = true;
  // 0 for the theme's size.
  float size = 0;
  // With alpha above 0, this colour rather than the theme's.
  color_t color{0, 0, 0, 0};
};
[[nodiscard]] node_t label(label_props_t props);
[[nodiscard]] node_t label(std::string text);

// A push button. Subscribe to its `press`: `ui::subscribe<ui::press_event_t>(save.press)`. Takes
// focus; Space presses it.
class button_t
{
public:
  struct props_t
  {
    std::string label;
    // The accent colour: the button a dialog expects.
    bool primary = false;
    bool enabled = true;
    friend bool operator==(const props_t &, const props_t &) = default;
  };
  props_t props;
  pressable_t press{focusable};

  [[nodiscard]] node_t view() const;
};

struct toggle_event_t
{
  const checkbox_t *source = nullptr;
  bool checked = false;
};

// A box that is ticked or not, with a label beside it; the label is part of what is pressed. It
// asks for the opposite of what it was given through `changed`.
class checkbox_t
{
public:
  struct props_t
  {
    std::string label;
    bool checked = false;
    bool enabled = true;
    friend bool operator==(const props_t &, const props_t &) = default;
  };
  props_t props;
  event_source_t<toggle_event_t> changed;

  [[nodiscard]] node_t view() const;
  behaviour_t behave(scope_t &scope);

private:
  pressable_t _press{focusable};
  // What it last asked for, until its next view: two presses in one turn both see the props of
  // the frame before, so the second must ask from what the first asked, not from those.
  mutable std::optional<bool> _asked;
};

struct select_event_t
{
  const combobox_t *source = nullptr;
  std::size_t index = 0;
};

namespace detail
{
// A combobox's open list: a popup component the combobox creates when it opens and drops when it
// closes. Its rows are fixed when it is made, so a deque, whose elements never move.
class menu_t
{
public:
  struct props_t
  {
    std::vector<std::string> options;
    int selected = -1;
    // The row the keyboard is on.
    int highlight = -1;
    // Not shown, but as wide as it is: the combobox's placeholder, so menu and field match.
    std::string widest;
    friend bool operator==(const props_t &, const props_t &) = default;
  };
  explicit menu_t(std::size_t rows);

  props_t props;
  std::deque<pressable_t> rows;

  [[nodiscard]] node_t view() const;
};
// A place in a text: a line, and a byte offset into it that is always on a code point's start.
struct text_position_t
{
  std::size_t line = 0;
  std::size_t byte = 0;
  friend auto operator<=>(const text_position_t &, const text_position_t &) = default;
};

// The editing both text widgets share (G50): lines of UTF-8, a caret and the anchor a selection
// runs from, and what each key does to them. Columns are code points, which is what a grid
// draws. Typed and pasted text is cleaned on the way in: control characters go, a tab becomes
// spaces to the next multiple of four, and a single-line editor turns line breaks into spaces.
class text_editor_t
{
public:
  explicit text_editor_t(bool multiline) noexcept
    : multiline(multiline)
  {
  }

  enum class outcome_t : std::uint8_t
  {
    none,
    moved,
    edited,
    copy,
    cut,
    paste,
    submit,
  };

  std::deque<std::string> lines{std::string()};
  text_position_t caret;
  text_position_t anchor;
  // The column Up and Down aim for, kept across lines shorter than it; npos when none.
  std::size_t goal = npos;
  bool multiline;
  static constexpr std::size_t npos = std::size_t(-1);

  [[nodiscard]] bool has_selection() const noexcept
  {
    return caret != anchor;
  }
  [[nodiscard]] text_position_t selection_start() const noexcept
  {
    return std::min(caret, anchor);
  }
  [[nodiscard]] text_position_t selection_end() const noexcept
  {
    return std::max(caret, anchor);
  }
  [[nodiscard]] std::string selected_text() const;
  [[nodiscard]] std::string text() const;
  // Replaces everything and puts the caret at the end.
  void set_text(std::string_view text);
  // Replaces the selection, and leaves the caret after what went in.
  void insert(std::string_view text);
  void erase_selection();
  // Puts the caret and anchor back inside the text, on code point starts.
  void clamp() noexcept;

  [[nodiscard]] std::size_t column(text_position_t at) const noexcept;
  [[nodiscard]] text_position_t at(std::size_t line, std::size_t column) const noexcept;
  void move_to(text_position_t to, bool extend) noexcept;
  void select_word(text_position_t at) noexcept;
  void select_line(std::size_t line) noexcept;
  void select_all() noexcept;

  // What a key press does, with `page` lines to a Page Up. Clipboard work is left to the caller.
  outcome_t key(const key_event_t &event, std::size_t page, bool read_only);
  // Typed text: edited, or none if nothing of it was printable or the editor is read-only.
  outcome_t type(std::string_view text, bool read_only);
};
} // namespace detail

// One of a list of strings, picked from a menu that opens under it. As wide as its widest option,
// so it does not change size as the choice does. Keyboard: Space, Enter or Alt+Down open it; Up and
// Down move through the options, open or closed; Enter or Space picks; Escape and Tab close. A
// press outside the open menu closes it and goes no further.
class combobox_t
{
public:
  struct props_t
  {
    std::vector<std::string> options;
    // -1 for none, which shows `placeholder`.
    int selected = -1;
    std::string placeholder;
    bool enabled = true;
    friend bool operator==(const props_t &, const props_t &) = default;
  };
  props_t props;
  event_source_t<select_event_t> changed;

  [[nodiscard]] node_t view() const;
  behaviour_t behave(scope_t &scope);

  [[nodiscard]] bool open() const noexcept
  {
    return bool(_menu);
  }

private:
  void pick(std::size_t index, scope_t &scope);
  // The choice as far as this widget knows: what it last asked for this frame, else its props.
  [[nodiscard]] int current() const noexcept
  {
    return _asked.value_or(props.selected);
  }

  pressable_t _field{focusable};
  pressable_t _dismiss;
  std::shared_ptr<detail::menu_t> _menu;
  int _highlight = -1;
  mutable std::optional<int> _asked;
};

// One row of a context menu.
struct menu_item_t
{
  enum class kind_t : std::uint8_t
  {
    item,
    // A thin rule between groups.
    separator,
    // A quiet title over a group: "Open with".
    header,
  };
  std::string label;
  // Shown at the right, quieter: a shortcut, or where the item leads.
  std::string detail;
  bool enabled = true;
  kind_t kind = kind_t::item;
  friend bool operator==(const menu_item_t &, const menu_item_t &) = default;
};

struct menu_choice_t
{
  const context_menu_t *source = nullptr;
  std::size_t index = 0;
};

// A menu that opens where its parent places it and over everything else: a context menu (G53). The
// parent shows it while it is open, at the pointer that opened it, and closes it on `chosen` or on
// a press on `dismiss`, which is everywhere outside it:
//
//   if (menu_at)
//     nodes.push_back(ui::at_t{.x = menu_at->x, .y = menu_at->y,
//                              .children = {ui::mount(menu, {.items = items}).on_top(menu.dismiss)}});
//
// It does not take focus from what opened it, so the parent hands it the keys it receives while it
// is open: key() moves through the items with Up and Down and chooses with Enter.
class context_menu_t
{
public:
  struct props_t
  {
    std::vector<menu_item_t> items;
    friend bool operator==(const props_t &, const props_t &) = default;
  };
  props_t props;
  event_source_t<menu_choice_t> chosen;
  pressable_t dismiss;

  [[nodiscard]] node_t view() const;
  behaviour_t behave(scope_t &scope);
  // Whether it used the key: Up, Down, Home, End and Enter.
  bool key(const key_event_t &event, scope_t &scope);

  static constexpr std::size_t max_items = 64;

private:
  [[nodiscard]] bool choosable(std::size_t index) const noexcept;
  std::array<pressable_t, max_items> _rows;
  int _highlight = -1;
};

struct line_edit_event_t
{
  const line_edit_t *source = nullptr;
  std::string text;
};

// One line of editable text, in the fixed-width font (G50). Controlled like the other widgets: the
// text comes down as props and every edit asks for the text it would make through `changed`, so a
// parent that does not apply it has refused it -- which is how a field takes only digits. Enter
// sends the text through `submitted`. The caret, the selection and how far the text is scrolled are
// the widget's own.
//
// Keys: arrows, Home and End, Ctrl (Alt on macOS) to move by word, Shift to select, Backspace and
// Delete, Ctrl+A, and Ctrl or Cmd with C, X and V for the clipboard. A double click selects a word.
class line_edit_t
{
public:
  struct props_t
  {
    std::string text;
    // Shown, dimmed, while the text is empty.
    std::string placeholder;
    // How many characters wide it is when nothing gives it more room.
    int columns = 20;
    bool enabled = true;
    bool read_only = false;
    friend bool operator==(const props_t &, const props_t &) = default;
  };
  props_t props;
  event_source_t<line_edit_event_t> changed;
  event_source_t<line_edit_event_t> submitted;

  [[nodiscard]] node_t view() const;
  behaviour_t behave(scope_t &scope);

  [[nodiscard]] const pressable_t &field() const noexcept
  {
    return _field;
  }
  [[nodiscard]] std::size_t caret() const noexcept
  {
    return _editor.caret.byte;
  }

private:
  void keep_caret_in_view();

  pressable_t _field{text_input};
  // The text as edited since the last view. That view put it back to the props, so an edit the
  // parent never applied is undone there.
  mutable detail::text_editor_t _editor{false};
  float _scroll = 0;
};

struct text_edit_event_t
{
  const text_edit_t *source = nullptr;
};

// Many lines of text in the fixed-width font, scrolled in both directions (G50): an input pane, or
// with `read_only` and `follow` a log. Unlike the other widgets it holds its own text, because a
// document copied down through props and compared every frame costs as much as the document; the
// parent reads it with text() or line() and changes it with set_text(), append() and
// append_line(), which rebuild it. A user's edit is reported through `changed`.
//
// The keys of a line edit, plus Up and Down, Page Up and Page Down, Ctrl+Home and Ctrl+End, and
// Enter for a new line. The wheel scrolls, Shift+wheel sideways; the scrollbar can be dragged. A
// triple click selects a line.
class text_edit_t
{
public:
  struct props_t
  {
    bool read_only = false;
    bool enabled = true;
    // Its size in characters when nothing gives it more room; a parent box that stretches its
    // children makes it fill that box instead.
    int columns = 60;
    int rows = 12;
    // While scrolled to the end, stay there as lines arrive: a log. Scrolling up stops following;
    // scrolling back to the end starts again.
    bool follow = false;
    // Drop the oldest lines past this many; 0 keeps them all.
    std::size_t max_lines = 0;
    friend bool operator==(const props_t &, const props_t &) = default;
  };
  props_t props;
  event_source_t<text_edit_event_t> changed;

  text_edit_t() = default;
  explicit text_edit_t(std::string_view text);

  [[nodiscard]] node_t view() const;
  behaviour_t behave(scope_t &scope);

  [[nodiscard]] std::string text() const
  {
    return _editor.text();
  }
  [[nodiscard]] std::size_t line_count() const noexcept
  {
    return _editor.lines.size();
  }
  [[nodiscard]] const std::string &line(std::size_t index) const
  {
    return _editor.lines[index];
  }
  [[nodiscard]] std::string selected_text() const
  {
    return _editor.selected_text();
  }
  [[nodiscard]] detail::text_position_t caret() const noexcept
  {
    return _editor.caret;
  }
  [[nodiscard]] const pressable_t &area() const noexcept
  {
    return _area;
  }

  // From a behaviour of any component in the same window, whose scope is how the edit is rebuilt.
  void set_text(scope_t &scope, std::string_view text);
  // At the very end, as typed: a "\n" in it starts a new line.
  void append(scope_t &scope, std::string_view text);
  // As a line of its own after the last, unless the text is empty.
  void append_line(scope_t &scope, std::string_view line);
  void clear(scope_t &scope);

private:
  struct geometry_t
  {
    float advance = 0;
    float line = 0;
    // The text area, inside the padding.
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
  };
  [[nodiscard]] geometry_t geometry() const;
  [[nodiscard]] float max_scroll_y(const geometry_t &g) const noexcept;
  [[nodiscard]] float max_scroll_x(const geometry_t &g) const;
  [[nodiscard]] float scroll_y(const geometry_t &g) const noexcept;
  void scroll_to(const geometry_t &g, float x, float y);
  void keep_caret_in_view(const geometry_t &g);
  [[nodiscard]] detail::text_position_t position_at(const geometry_t &g, point_t at) const noexcept;
  void trim();
  void edited_externally(scope_t &scope);

  pressable_t _area{text_input};
  pressable_t _thumb;
  detail::text_editor_t _editor{true};
  float _scroll_x = 0;
  float _scroll_y = 0;
  // Following the end, while `follow` is set.
  bool _pinned = true;
  // Where on the thumb a drag took hold of it.
  float _grab = -1;
  mutable std::size_t _widest = 0;
  mutable bool _widest_valid = false;
};
} // namespace vui::ui
