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

#include "vui/ui/widgets.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>

namespace vui::ui
{
namespace
{
constexpr color_t clear{0, 0, 0, 0};
constexpr color_t white{1, 1, 1, 1};

// A tick, as one closed outline in the unit square.
std::shared_ptr<const path_t> check_icon()
{
  static const auto path = []
  {
    auto p = std::make_shared<path_t>();
    p->move_to(0.12f, 0.53f).line_to(0.23f, 0.42f).line_to(0.40f, 0.59f).line_to(0.77f, 0.22f).line_to(0.88f, 0.33f).line_to(0.40f, 0.81f).close();
    return std::shared_ptr<const path_t>(std::move(p));
  }();
  return path;
}

// A chevron pointing down.
std::shared_ptr<const path_t> chevron_icon()
{
  static const auto path = []
  {
    auto p = std::make_shared<path_t>();
    p->move_to(0.14f, 0.36f).line_to(0.25f, 0.25f).line_to(0.5f, 0.5f).line_to(0.75f, 0.25f).line_to(0.86f, 0.36f).line_to(0.5f, 0.72f).close();
    return std::shared_ptr<const path_t>(std::move(p));
  }();
  return path;
}

event_ref_t events_if(bool enabled, const pressable_t &source)
{
  return enabled ? event_ref_t(source) : event_ref_t{};
}

// A control's fill for what the pointer is doing to it.
color_t control_color(const theme_t &t, bool enabled, const pressable_t &source)
{
  if (!enabled)
    return t.control_disabled;
  if (source.held())
    return t.control_held;
  return source.hovered() ? t.control_hovered : t.control;
}

color_t accent_color(const theme_t &t, const pressable_t &source)
{
  if (source.held())
    return t.accent_held;
  return source.hovered() ? t.accent_hovered : t.accent;
}
} // namespace

const theme_t &theme() noexcept
{
  static const theme_t dark;
  return dark;
}

// ---- button

node_t button_t::view() const
{
  const auto &t = theme();
  const bool on = props.enabled;
  const bool ring = on && press.focus_visible();
  color_t fill = control_color(t, on, press);
  if (on && props.primary)
    fill = accent_color(t, press);
  return box_t{.size = {72, 32},
               .color = fill,
               .radius = t.radius,
               .border_width = ring ? 2.0f : (props.primary && on ? 0.0f : 1.0f),
               .border_color = ring ? t.focus_ring : (press.hovered() && on ? t.border_hovered : t.border),
               .shadow = on ? shadow_t{.x = 0, .y = 1, .blur = 1.5f, .color = {0, 0, 0, 0.35f}} : shadow_t{},
               .events = events_if(on, press),
               .padding = {14, 0},
               .children = {text_t{.text = props.label, .size = t.text_size, .color = !on ? t.text_disabled : (props.primary ? white : t.text)}}};
}

// ---- checkbox

node_t checkbox_t::view() const
{
  // The parent has answered what was asked by now: these props are its answer.
  _asked.reset();
  const auto &t = theme();
  const bool on = props.enabled;
  const bool ring = on && _press.focus_visible();
  color_t fill = control_color(t, on, _press);
  if (props.checked)
    fill = on ? accent_color(t, _press) : t.border;
  box_t square{.size = {18, 18}, .color = fill, .radius = 4, .border_width = ring ? 2.0f : (props.checked ? 0.0f : 1.5f), .border_color = ring ? t.focus_ring : (_press.hovered() && on ? t.border_hovered : t.border)};
  if (props.checked)
    square.children.push_back(icon_t{.path = check_icon(), .size = 14, .color = on ? white : t.control_disabled});
  // The whole of it, label included, is what is pressed.
  return box_t{.color = clear,
               .events = events_if(on, _press),
               .padding = {0, 3},
               .children = {row_t{.gap = 8, .align = align_t::center, .children = {std::move(square), text_t{.text = props.label, .size = t.text_size, .color = on ? t.text : t.text_disabled}}}}};
}

behaviour_t checkbox_t::behave(scope_t &scope)
{
  auto presses = subscribe<press_event_t>(_press);
  while (co_await presses)
  {
    _asked = !_asked.value_or(props.checked);
    emit(changed, toggle_event_t{this, *_asked}, scope);
  }
}

// ---- combobox

namespace detail
{
menu_t::menu_t(std::size_t count)
  : rows(count)
{
}

node_t menu_t::view() const
{
  const auto &t = theme();
  const auto n = std::min(rows.size(), props.options.size());
  // Two marks, kept apart: the keyboard's row is lit in the accent, and is what Enter picks; the
  // row under the pointer is only tinted. Letting the pointer move the keyboard's row made a pointer
  // resting where the menu opened take the arrow keys over.
  column_t list{.align = align_t::stretch};
  for (std::size_t i = 0; i < n; ++i)
  {
    const bool lit = int(i) == props.highlight;
    const bool chosen = int(i) == props.selected;
    // Padding, text, at least 8 of space and a 12-wide tick: as wide as the field over it, whose
    // padding, gap and chevron add up to the same.
    node_t mark = chosen ? node_t(icon_t{.path = check_icon(), .size = 12, .color = lit ? white : t.accent}) : node_t(spacer_t{.grow = 0, .size = {12, 12}});
    list.children.push_back(
      box_t{.color = lit ? t.accent : (rows[i].hovered() ? t.control_hovered : clear),
            .events = rows[i],
            .padding = {9, 5, 7, 5},
            .align_x = align_t::stretch,
            .children = {row_t{.align = align_t::center, .children = {text_t{.text = props.options[i], .size = t.text_size, .color = lit ? white : t.text}, spacer_t{.grow = 1, .size = {8, 0}}, std::move(mark)}}}});
  }
  box_t panel{.color = t.menu,
              .radius = t.radius,
              .border_width = 1,
              .border_color = t.border,
              .shadow = {.x = 0, .y = 4, .blur = 10, .color = {0, 0, 0, 0.5f}},
              .padding = {1, 4},
              .align_x = align_t::stretch,
              .children = {std::move(list)}};
  // Overlapping the rows, unseen, so the menu is at least as wide as the field's placeholder.
  if (!props.widest.empty())
    panel.children.push_back(box_t{.color = clear, .padding = {9, 0, 27, 0}, .children = {text_t{.text = props.widest, .size = t.text_size, .color = clear}}});
  // A little air between the field and the menu.
  return box_t{.color = clear, .padding = {0, 4, 0, 0}, .children = {std::move(panel)}};
}
} // namespace detail

node_t combobox_t::view() const
{
  _asked.reset();
  const auto &t = theme();
  const bool on = props.enabled;
  const bool ring = on && _field.focus_visible();
  const bool has_choice = props.selected >= 0 && std::size_t(props.selected) < props.options.size();

  // Every option, invisible, under the one shown: the field is as wide as the widest of them and
  // keeps that width whichever is chosen.
  box_t texts{.color = clear, .align_x = align_t::start};
  for (const auto &option : props.options)
    texts.children.push_back(text_t{.text = option, .size = t.text_size, .color = clear});
  if (!props.placeholder.empty())
    texts.children.push_back(text_t{.text = props.placeholder, .size = t.text_size, .color = clear});
  if (has_choice)
    texts.children.push_back(text_t{.text = props.options[std::size_t(props.selected)], .size = t.text_size, .color = on ? t.text : t.text_disabled});
  else
    texts.children.push_back(text_t{.text = props.placeholder, .size = t.text_size, .color = t.text_disabled});

  color_t fill = control_color(t, on, _field);
  if (on && _menu && !_field.held())
    fill = t.control_hovered;
  box_t field{.size = {0, 32},
              .color = fill,
              .radius = t.radius,
              .border_width = ring ? 2.0f : 1.0f,
              .border_color = ring ? t.focus_ring : ((_field.hovered() || _menu) && on ? t.border_hovered : t.border),
              .events = events_if(on, _field),
              .padding = {10, 0, 8, 0},
              .align_x = align_t::start,
              .children = {row_t{.gap = 8, .align = align_t::center, .children = {std::move(texts), icon_t{.path = chevron_icon(), .size = 12, .color = on ? t.text : t.text_disabled}}}}};

  column_t whole{.children = {std::move(field)}};
  if (_menu)
    whole.children.push_back(mount(_menu, detail::menu_t::props_t{.options = props.options, .selected = props.selected, .highlight = _highlight, .widest = props.placeholder}).on_top(_dismiss));
  return whole;
}

void combobox_t::pick(std::size_t index, scope_t &scope)
{
  _menu.reset();
  if (index < props.options.size() && int(index) != current())
  {
    _asked = int(index);
    emit(changed, select_event_t{this, index}, scope);
  }
}

behaviour_t combobox_t::behave(scope_t &scope)
{
  auto presses = subscribe<press_event_t>(_field, _dismiss);
  auto keys = subscribe<key_event_t>(_field);
  const auto count = [this] { return int(props.options.size()); };
  const auto open_menu = [this]
  {
    _menu = std::make_shared<detail::menu_t>(props.options.size());
    _highlight = std::max(current(), 0);
  };

  while (true)
  {
    // Closed: a press opens it, and Up and Down change the choice without opening it.
    if (!_menu)
    {
      auto events = merge(scope, presses, keys);
      auto event = co_await events;
      if (!event)
        co_return;
      if (!props.enabled || count() == 0)
        continue;
      if (event->index() == 0)
      {
        if (std::get<0>(*event).source == &_field)
          open_menu();
        continue;
      }
      const auto &key = std::get<1>(*event);
      if (!key.pressed)
        continue;
      const int last = count() - 1;
      if ((key.key == key_t::down && has(key.modifiers, modifier_t::alt)) || key.key == key_t::enter || key.key == key_t::keypad_enter)
        open_menu();
      else if (key.key == key_t::down)
        pick(std::size_t(std::min(current() + 1, last)), scope);
      else if (key.key == key_t::up)
        pick(std::size_t(std::max(current() - 1, 0)), scope);
      else if (key.key == key_t::home)
        pick(0, scope);
      else if (key.key == key_t::end)
        pick(std::size_t(last), scope);
      continue;
    }

    // Open: the rows join in, for as long as this menu lasts.
    auto rows = subscribe<press_event_t>(_menu->rows);
    auto events = merge(scope, presses, keys, rows);
    while (_menu)
    {
      auto event = co_await events;
      if (!event)
        co_return;
      if (event->index() == 0)
      {
        // Space on the field picks what is lit; a press outside closes it.
        if (std::get<0>(*event).source == &_field)
          pick(std::size_t(_highlight), scope);
        else
          _menu.reset();
      }
      else if (event->index() == 1)
      {
        const auto &key = std::get<1>(*event);
        if (!key.pressed)
          continue;
        const int last = count() - 1;
        const int from = _highlight;
        if (key.key == key_t::down)
          _highlight = std::min(from + 1, last);
        else if (key.key == key_t::up)
          _highlight = std::max(from - 1, 0);
        else if (key.key == key_t::home)
          _highlight = 0;
        else if (key.key == key_t::end)
          _highlight = last;
        else if (key.key == key_t::enter || key.key == key_t::keypad_enter)
          pick(std::size_t(from), scope);
        else if (key.key == key_t::escape || key.key == key_t::tab)
          _menu.reset();
      }
      else
      {
        const auto *row = std::get<2>(*event).source;
        for (std::size_t i = 0; i < _menu->rows.size(); ++i)
        {
          if (&_menu->rows[i] == row)
          {
            pick(i, scope);
            break;
          }
        }
      }
    }
  }
}

// ---- context menu (G53)

bool context_menu_t::choosable(std::size_t index) const noexcept
{
  return index < props.items.size() && index < max_items && props.items[index].kind == menu_item_t::kind_t::item && props.items[index].enabled;
}

node_t context_menu_t::view() const
{
  const auto &t = theme();
  column_t list{.align = align_t::stretch};
  const auto n = std::min(props.items.size(), max_items);
  for (std::size_t i = 0; i < n; ++i)
  {
    const auto &item = props.items[i];
    if (item.kind == menu_item_t::kind_t::separator)
    {
      list.children.push_back(box_t{.color = clear, .padding = {8, 4}, .align_x = align_t::stretch, .children = {box_t{.size = {0, 1}, .color = t.border}}});
      continue;
    }
    if (item.kind == menu_item_t::kind_t::header)
    {
      list.children.push_back(box_t{.color = clear, .padding = {12, 6, 12, 2}, .align_x = align_t::start, .children = {text_t{.text = item.label, .size = t.text_size - 2, .color = t.text_muted}}});
      continue;
    }
    const bool lit = int(i) == _highlight && item.enabled;
    const bool hovered = _rows[i].hovered() && item.enabled;
    const color_t ink = !item.enabled ? t.text_disabled : (lit ? white : t.text);
    row_t line{.align = align_t::center, .children = {text_t{.text = item.label, .size = t.text_size, .color = ink}, spacer_t{.grow = 1, .size = {24, 0}}}};
    if (!item.detail.empty())
      line.children.push_back(text_t{.text = item.detail, .size = t.text_size - 1, .color = lit ? white : t.text_muted});
    list.children.push_back(box_t{.color = lit ? t.accent : (hovered ? t.control_hovered : clear),
                                  .radius = 4,
                                  .events = item.enabled ? event_ref_t(_rows[i]) : event_ref_t{},
                                  .padding = {12, 5},
                                  .align_x = align_t::stretch,
                                  .children = {std::move(line)}});
  }
  return box_t{.size = {200, 0},
               .color = t.menu,
               .radius = t.radius,
               .border_width = 1,
               .border_color = t.border,
               .shadow = {.x = 0, .y = 4, .blur = 12, .color = {0, 0, 0, 0.55f}},
               .padding = {4, 4},
               .align_x = align_t::stretch,
               .children = {std::move(list)}};
}

behaviour_t context_menu_t::behave(scope_t &scope)
{
  // Opened afresh: nothing is lit until a key asks.
  _highlight = -1;
  auto presses = subscribe<press_event_t>(_rows);
  while (auto press = co_await presses)
  {
    const auto index = std::size_t(press->source - &_rows[0]);
    if (choosable(index))
      emit(chosen, menu_choice_t{this, index}, scope);
  }
}

bool context_menu_t::key(const key_event_t &event, scope_t &scope)
{
  if (!event.pressed)
    return false;
  const int n = int(std::min(props.items.size(), max_items));
  const auto step = [&](int from, int by)
  {
    for (int i = from + by; i >= 0 && i < n; i += by)
    {
      if (choosable(std::size_t(i)))
        return i;
    }
    return from;
  };
  const int before = _highlight;
  switch (event.key)
  {
  case key_t::down:
    _highlight = step(_highlight, 1);
    break;
  case key_t::up:
    _highlight = _highlight < 0 ? step(n, -1) : step(_highlight, -1);
    break;
  case key_t::home:
    _highlight = step(-1, 1);
    break;
  case key_t::end:
    _highlight = step(n, -1);
    break;
  case key_t::enter:
  case key_t::keypad_enter:
    if (_highlight >= 0 && choosable(std::size_t(_highlight)))
      emit(chosen, menu_choice_t{this, std::size_t(_highlight)}, scope);
    return true;
  default:
    return false;
  }
  if (_highlight != before)
    invalidate(scope, *this);
  return true;
}

// ---- text (G50)

namespace
{
bool continuation(char c) noexcept
{
  return (static_cast<unsigned char>(c) & 0xc0) == 0x80;
}

std::size_t next_boundary(const std::string &s, std::size_t b) noexcept
{
  if (b >= s.size())
    return s.size();
  ++b;
  while (b < s.size() && continuation(s[b]))
    ++b;
  return b;
}

std::size_t prev_boundary(const std::string &s, std::size_t b) noexcept
{
  if (b == 0)
    return 0;
  --b;
  while (b > 0 && continuation(s[b]))
    --b;
  return b;
}

std::size_t code_points(std::string_view s) noexcept
{
  std::size_t n = 0;
  for (char c : s)
    n += continuation(c) ? 0 : 1;
  return n;
}

// The byte offset after `column` code points, or the end.
std::size_t byte_at(const std::string &s, std::size_t column) noexcept
{
  std::size_t b = 0;
  for (std::size_t c = 0; c < column && b < s.size(); ++c)
    b = next_boundary(s, b);
  return b;
}

// What a word is made of, for moving and selecting by word: letters, digits and anything past
// ASCII; then spaces; then punctuation.
int char_class(const std::string &s, std::size_t b) noexcept
{
  const auto c = static_cast<unsigned char>(s[b]);
  if (c >= 0x80 || std::isalnum(c) || c == '_')
    return 0;
  if (c == ' ')
    return 1;
  return 2;
}

struct text_metrics_t
{
  float advance = 0;
  float line = 0;
};

text_metrics_t mono_metrics(float size)
{
  const auto m = monospace_font().metrics(size);
  text_metrics_t out;
  out.advance = m.advance > 0 ? m.advance : size * 0.6f;
  out.line = std::ceil(m.ascent + m.descent > 0 ? m.ascent + m.descent + m.line_gap : size * 1.25f);
  return out;
}

node_t placed(float x, float y, node_t node)
{
  return at_t{.x = x, .y = y, .children = {std::move(node)}};
}

// One line's visible part, on the grid, starting where the grid puts it.
node_t grid_line(const std::string &line, std::size_t first_column, std::size_t columns, float advance, float x, float y, float size, color_t color)
{
  const auto begin = byte_at(line, first_column);
  std::size_t end = begin;
  for (std::size_t c = 0; c < columns && end < line.size(); ++c)
    end = next_boundary(line, end);
  return placed(x + float(first_column) * advance, y, text_t{.text = line.substr(begin, end - begin), .size = size, .color = color, .font = monospace_font(), .advance = advance});
}

bool command_held(modifier_t m) noexcept
{
  return has(m, modifier_t::control) || has(m, modifier_t::super);
}
} // namespace

const font_t &monospace_font()
{
  static const font_t font = []
  {
    if (auto mono = font_t::system_monospace())
      return std::move(*mono);
    if (auto any = font_t::system_default())
      return std::move(*any);
    return font_t{};
  }();
  return font;
}

// ---- label

node_t label(label_props_t props)
{
  const auto &t = theme();
  const float size = props.size > 0 ? props.size : (props.monospace ? t.mono_size : t.text_size);
  const color_t color = props.color.a > 0 ? props.color : (!props.enabled ? t.text_disabled : (props.muted ? t.text_muted : t.text));
  const font_t font = props.monospace ? monospace_font() : font_t{};
  if (props.text.find('\n') == std::string::npos)
    return text_t{.text = std::move(props.text), .size = size, .color = color, .font = font};
  column_t lines;
  std::size_t from = 0;
  while (from <= props.text.size())
  {
    auto to = props.text.find('\n', from);
    if (to == std::string::npos)
      to = props.text.size();
    lines.children.push_back(text_t{.text = props.text.substr(from, to - from), .size = size, .color = color, .font = font});
    from = to + 1;
  }
  return lines;
}

node_t label(std::string text)
{
  return label(label_props_t{.text = std::move(text)});
}

// ---- the editor both text widgets share

namespace detail
{
std::string text_editor_t::text() const
{
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i)
  {
    if (i > 0)
      out += '\n';
    out += lines[i];
  }
  return out;
}

std::string text_editor_t::selected_text() const
{
  const auto a = selection_start();
  const auto b = selection_end();
  if (a.line == b.line)
    return lines[a.line].substr(a.byte, b.byte - a.byte);
  std::string out = lines[a.line].substr(a.byte);
  for (std::size_t i = a.line + 1; i < b.line; ++i)
    out += '\n' + lines[i];
  out += '\n' + lines[b.line].substr(0, b.byte);
  return out;
}

void text_editor_t::set_text(std::string_view text)
{
  lines.assign(1, std::string());
  caret = anchor = {};
  insert(text);
}

void text_editor_t::erase_selection()
{
  if (!has_selection())
    return;
  const auto a = selection_start();
  const auto b = selection_end();
  if (a.line == b.line)
    lines[a.line].erase(a.byte, b.byte - a.byte);
  else
  {
    lines[a.line] = lines[a.line].substr(0, a.byte) + lines[b.line].substr(b.byte);
    lines.erase(lines.begin() + std::ptrdiff_t(a.line) + 1, lines.begin() + std::ptrdiff_t(b.line) + 1);
  }
  caret = anchor = a;
  goal = npos;
}

void text_editor_t::insert(std::string_view text)
{
  erase_selection();
  // Cleaned into the lines it makes: control characters dropped, tabs to the next stop of four.
  std::vector<std::string> pieces(1);
  std::size_t col = column(caret);
  for (std::size_t i = 0; i < text.size(); ++i)
  {
    const char c = text[i];
    if (c == '\n')
    {
      if (multiline)
      {
        pieces.emplace_back();
        col = 0;
      }
      else
      {
        pieces.back() += ' ';
        ++col;
      }
      continue;
    }
    if (c == '\t')
    {
      const std::size_t spaces = 4 - col % 4;
      pieces.back().append(spaces, ' ');
      col += spaces;
      continue;
    }
    if ((static_cast<unsigned char>(c) < 0x20) || c == 0x7f)
      continue;
    pieces.back() += c;
    col += continuation(c) ? 0 : 1;
  }
  auto &line = lines[caret.line];
  const std::string tail = line.substr(caret.byte);
  line.erase(caret.byte);
  line += pieces.front();
  if (pieces.size() == 1)
  {
    caret.byte = line.size();
    line += tail;
  }
  else
  {
    for (std::size_t i = 1; i < pieces.size(); ++i)
      lines.insert(lines.begin() + std::ptrdiff_t(caret.line + i), pieces[i]);
    caret.line += pieces.size() - 1;
    caret.byte = lines[caret.line].size();
    lines[caret.line] += tail;
  }
  anchor = caret;
  goal = npos;
}

void text_editor_t::clamp() noexcept
{
  if (lines.empty())
    lines.emplace_back();
  for (auto *p : {&caret, &anchor})
  {
    p->line = std::min(p->line, lines.size() - 1);
    const auto &line = lines[p->line];
    p->byte = std::min(p->byte, line.size());
    while (p->byte > 0 && p->byte < line.size() && continuation(line[p->byte]))
      --p->byte;
  }
}

std::size_t text_editor_t::column(text_position_t at) const noexcept
{
  const auto &line = lines[at.line];
  return code_points(std::string_view(line).substr(0, std::min(at.byte, line.size())));
}

text_position_t text_editor_t::at(std::size_t line, std::size_t col) const noexcept
{
  line = std::min(line, lines.size() - 1);
  return {line, byte_at(lines[line], col)};
}

void text_editor_t::move_to(text_position_t to, bool extend) noexcept
{
  caret = to;
  if (!extend)
    anchor = caret;
}

void text_editor_t::select_word(text_position_t p) noexcept
{
  const auto &line = lines[p.line];
  if (line.empty())
  {
    caret = anchor = {p.line, 0};
    return;
  }
  std::size_t b = p.byte < line.size() ? p.byte : prev_boundary(line, line.size());
  const int kind = char_class(line, b);
  std::size_t begin = b;
  while (begin > 0 && char_class(line, prev_boundary(line, begin)) == kind)
    begin = prev_boundary(line, begin);
  std::size_t end = next_boundary(line, b);
  while (end < line.size() && char_class(line, end) == kind)
    end = next_boundary(line, end);
  anchor = {p.line, begin};
  caret = {p.line, end};
  goal = npos;
}

void text_editor_t::select_line(std::size_t line) noexcept
{
  line = std::min(line, lines.size() - 1);
  anchor = {line, 0};
  caret = line + 1 < lines.size() ? text_position_t{line + 1, 0} : text_position_t{line, lines[line].size()};
  goal = npos;
}

void text_editor_t::select_all() noexcept
{
  anchor = {};
  caret = {lines.size() - 1, lines.back().size()};
  goal = npos;
}

text_editor_t::outcome_t text_editor_t::key(const key_event_t &event, std::size_t page, bool read_only)
{
  if (!event.pressed)
    return outcome_t::none;
  const auto m = event.modifiers;
  const bool shift = has(m, modifier_t::shift);
  const bool command = command_held(m);
#if defined(__APPLE__)
  const bool word = has(m, modifier_t::alt);
#else
  const bool word = has(m, modifier_t::control);
#endif

  const auto left_of = [&](text_position_t p)
  {
    if (p.byte == 0)
      return p.line > 0 ? text_position_t{p.line - 1, lines[p.line - 1].size()} : p;
    const auto &line = lines[p.line];
    if (!word)
      return text_position_t{p.line, prev_boundary(line, p.byte)};
    std::size_t b = p.byte;
    while (b > 0 && char_class(line, prev_boundary(line, b)) == 1)
      b = prev_boundary(line, b);
    if (b > 0)
    {
      const int kind = char_class(line, prev_boundary(line, b));
      while (b > 0 && char_class(line, prev_boundary(line, b)) == kind)
        b = prev_boundary(line, b);
    }
    return text_position_t{p.line, b};
  };
  const auto right_of = [&](text_position_t p)
  {
    const auto &line = lines[p.line];
    if (p.byte >= line.size())
      return p.line + 1 < lines.size() ? text_position_t{p.line + 1, 0} : p;
    if (!word)
      return text_position_t{p.line, next_boundary(line, p.byte)};
    std::size_t b = p.byte;
    const int kind = char_class(line, b);
    while (b < line.size() && char_class(line, b) == kind)
      b = next_boundary(line, b);
    while (b < line.size() && char_class(line, b) == 1)
      b = next_boundary(line, b);
    return text_position_t{p.line, b};
  };
  const auto vertical = [&](long delta)
  {
    if (goal == npos)
      goal = column(caret);
    const long target = std::clamp(long(caret.line) + delta, 0L, long(lines.size()) - 1);
    const auto kept = goal;
    move_to(at(std::size_t(target), goal), shift);
    goal = kept;
    return outcome_t::moved;
  };
  const auto horizontal = [&](text_position_t to)
  {
    move_to(to, shift);
    goal = npos;
    return outcome_t::moved;
  };

  switch (event.key)
  {
  case key_t::left:
    if (has_selection() && !shift)
      return horizontal(selection_start());
    return horizontal(left_of(caret));
  case key_t::right:
    if (has_selection() && !shift)
      return horizontal(selection_end());
    return horizontal(right_of(caret));
  case key_t::up:
    return multiline ? vertical(-1) : horizontal({caret.line, 0});
  case key_t::down:
    return multiline ? vertical(1) : horizontal({caret.line, lines[caret.line].size()});
  case key_t::page_up:
    return multiline ? vertical(-long(std::max<std::size_t>(page, 1))) : outcome_t::none;
  case key_t::page_down:
    return multiline ? vertical(long(std::max<std::size_t>(page, 1))) : outcome_t::none;
  case key_t::home:
    return horizontal(command && multiline ? text_position_t{} : text_position_t{caret.line, 0});
  case key_t::end:
    return horizontal(command && multiline ? text_position_t{lines.size() - 1, lines.back().size()} : text_position_t{caret.line, lines[caret.line].size()});
  case key_t::backspace:
  case key_t::delete_key:
    if (read_only)
      return outcome_t::none;
    if (!has_selection())
    {
      const auto to = event.key == key_t::backspace ? left_of(caret) : right_of(caret);
      if (to == caret)
        return outcome_t::none;
      anchor = to;
    }
    erase_selection();
    return outcome_t::edited;
  case key_t::enter:
  case key_t::keypad_enter:
    if (!multiline)
      return outcome_t::submit;
    if (read_only)
      return outcome_t::none;
    insert("\n");
    return outcome_t::edited;
  case key_t::a:
    if (!command)
      return outcome_t::none;
    select_all();
    return outcome_t::moved;
  case key_t::c:
    return command && has_selection() ? outcome_t::copy : outcome_t::none;
  case key_t::x:
    return command && has_selection() && !read_only ? outcome_t::cut : outcome_t::none;
  case key_t::v:
    return command && !read_only ? outcome_t::paste : outcome_t::none;
  default:
    return outcome_t::none;
  }
}

text_editor_t::outcome_t text_editor_t::type(std::string_view text, bool read_only)
{
  if (read_only)
    return outcome_t::none;
  if (std::none_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x20 && c != 0x7f; }))
    return outcome_t::none;
  insert(text);
  return outcome_t::edited;
}
} // namespace detail

// ---- line edit

namespace
{
constexpr int field_pad_x = 8;
constexpr int field_pad_y = 6;
constexpr int area_pad_x = 6;
constexpr int area_pad_y = 4;
constexpr float caret_width = 1.5f;
} // namespace

node_t line_edit_t::view() const
{
  const auto &t = theme();
  const bool on = props.enabled;
  // The parent's answer: what it gave is the text from here on.
  if (_editor.lines.front() != props.text)
  {
    _editor.lines.assign(1, props.text);
    _editor.clamp();
  }
  if (std::exchange(_select_all, false))
    _editor.select_all();
  const auto m = mono_metrics(t.mono_size);
  const auto &line = _editor.lines.front();
  const float width = float(std::max(props.columns, 1)) * m.advance;

  // How much of it shows: the field's own width once it has been drawn, else what was asked for.
  const auto bounds = _field.bounds();
  const float shown = bounds.width > 0 ? float(bounds.width - 2 * field_pad_x) : width;
  const float scroll = std::max(0.0f, _scroll);
  const auto first = std::size_t(scroll / m.advance);
  const auto columns = std::size_t(std::ceil(shown / m.advance)) + 2;

  box_t text_area{.size = {int(std::ceil(width)), int(m.line)}, .color = clear, .align_x = align_t::start, .align_y = align_t::start, .clip = true};
  const bool focused = on && _field.focused();
  if (focused && _editor.has_selection())
  {
    const float a = float(_editor.column(_editor.selection_start())) * m.advance - scroll;
    const float b = float(_editor.column(_editor.selection_end())) * m.advance - scroll;
    text_area.children.push_back(placed(a, 0, box_t{.size = {int(std::ceil(b - a)), int(m.line)}, .color = t.selection}));
  }
  if (line.empty() && !props.placeholder.empty())
    text_area.children.push_back(grid_line(props.placeholder, 0, columns, m.advance, 0, 0, t.mono_size, t.text_disabled));
  else
    text_area.children.push_back(grid_line(line, first, columns, m.advance, -scroll, 0, t.mono_size, on ? t.text : t.text_disabled));
  if (focused && !props.read_only)
    text_area.children.push_back(placed(float(_editor.column(_editor.caret)) * m.advance - scroll, 0, box_t{.size = {2, int(m.line)}, .color = t.caret, .radius = caret_width / 2}));

  return box_t{.color = on ? t.field : t.control_disabled,
               .radius = t.radius,
               .border_width = focused ? 2.0f : 1.0f,
               .border_color = focused ? t.focus_ring : (_field.hovered() && on ? t.border_hovered : t.border),
               .events = events_if(on, _field),
               .cursor = on ? std::optional(cursor_t::text) : std::nullopt,
               .padding = {field_pad_x, field_pad_y},
               .align_x = align_t::stretch,
               .children = {std::move(text_area)}};
}

void line_edit_t::keep_caret_in_view()
{
  const auto m = mono_metrics(theme().mono_size);
  const auto bounds = _field.bounds();
  const float shown = bounds.width > 0 ? float(bounds.width - 2 * field_pad_x) : float(std::max(props.columns, 1)) * m.advance;
  const float x = float(_editor.column(_editor.caret)) * m.advance;
  if (x - _scroll > shown - caret_width)
    _scroll = x - shown + caret_width;
  if (x < _scroll)
    _scroll = x;
  const float end = float(_editor.column({0, _editor.lines.front().size()})) * m.advance;
  _scroll = std::clamp(_scroll, 0.0f, std::max(0.0f, end + caret_width - shown));
}

behaviour_t line_edit_t::behave(scope_t &scope)
{
  auto typed = subscribe<typing_event_t>(_field);
  auto drags = subscribe<drag_event_t>(_field);
  auto events = merge(scope, typed, drags);
  const auto ask = [this, &scope]
  {
    keep_caret_in_view();
    emit(changed, line_edit_event_t{this, _editor.lines.front()}, scope);
  };
  const auto column_at = [this](point_t at)
  {
    const auto m = mono_metrics(theme().mono_size);
    const float x = float(at.x) - float(_field.bounds().x + field_pad_x) + _scroll;
    return _editor.at(0, std::size_t(std::max(0.0f, std::round(x / m.advance))));
  };

  while (auto event = co_await events)
  {
    if (!props.enabled)
      continue;
    using outcome_t = detail::text_editor_t::outcome_t;
    if (event->index() == 1)
    {
      const auto &drag = std::get<1>(*event);
      const auto at = column_at(drag.position);
      if (drag.phase == drag_phase_t::begin && drag.clicks == 2)
        _editor.select_word(at);
      else if (drag.phase == drag_phase_t::begin && drag.clicks >= 3)
        _editor.select_all();
      else if (drag.phase == drag_phase_t::begin)
        _editor.move_to(at, has(drag.modifiers, modifier_t::shift));
      else if (drag.phase == drag_phase_t::move && drag.clicks == 1)
        _editor.move_to(at, true);
      keep_caret_in_view();
      continue;
    }
    const auto &input = std::get<0>(*event).input;
    const auto outcome = input.index() == 0 ? _editor.key(std::get<0>(input), 1, props.read_only) : _editor.type(std::get<1>(input).text, props.read_only);
    switch (outcome)
    {
    case outcome_t::moved:
      keep_caret_in_view();
      break;
    case outcome_t::edited:
      ask();
      break;
    case outcome_t::submit:
      emit(submitted, line_edit_event_t{this, _editor.lines.front()}, scope);
      break;
    case outcome_t::copy:
      scope.window().set_clipboard_text(_editor.selected_text());
      break;
    case outcome_t::cut:
      scope.window().set_clipboard_text(_editor.selected_text());
      _editor.erase_selection();
      ask();
      break;
    case outcome_t::paste:
      if (auto pasted = co_await scope.window().clipboard_text(); pasted && !pasted->empty())
      {
        _editor.insert(*pasted);
        ask();
      }
      break;
    case outcome_t::none:
      break;
    }
  }
}

// ---- text edit

text_edit_t::text_edit_t(std::string_view text)
{
  _editor.set_text(text);
  _editor.caret = _editor.anchor = {};
}

text_edit_t::geometry_t text_edit_t::geometry() const
{
  const auto m = mono_metrics(theme().mono_size);
  geometry_t g;
  g.advance = m.advance;
  g.line = m.line;
  const auto bounds = _area.bounds();
  if (bounds.width > 0 && bounds.height > 0)
  {
    g.x = float(bounds.x + area_pad_x);
    g.y = float(bounds.y + area_pad_y);
    g.width = std::max(float(bounds.width - 2 * area_pad_x), m.advance);
    g.height = std::max(float(bounds.height - 2 * area_pad_y), m.line);
  }
  else
  {
    g.width = float(std::max(props.columns, 1)) * m.advance;
    g.height = float(std::max(props.rows, 1)) * m.line;
  }
  return g;
}

float text_edit_t::max_scroll_y(const geometry_t &g) const noexcept
{
  return std::max(0.0f, float(_editor.lines.size()) * g.line - g.height);
}

float text_edit_t::max_scroll_x(const geometry_t &g) const
{
  if (!_widest_valid)
  {
    _widest = 0;
    for (const auto &line : _editor.lines)
      _widest = std::max(_widest, code_points(line));
    _widest_valid = true;
  }
  return std::max(0.0f, float(_widest + 1) * g.advance - g.width);
}

float text_edit_t::scroll_y(const geometry_t &g) const noexcept
{
  if (props.follow && _pinned)
    return max_scroll_y(g);
  return std::clamp(_scroll_y, 0.0f, max_scroll_y(g));
}

void text_edit_t::scroll_to(const geometry_t &g, float x, float y)
{
  const float bottom = max_scroll_y(g);
  _scroll_y = std::clamp(y, 0.0f, bottom);
  _scroll_x = std::clamp(x, 0.0f, std::max(max_scroll_x(g), _scroll_x));
  _pinned = _scroll_y >= bottom - 0.5f;
}

void text_edit_t::keep_caret_in_view(const geometry_t &g)
{
  float x = _scroll_x;
  float y = scroll_y(g);
  const float cx = float(_editor.column(_editor.caret)) * g.advance;
  const float cy = float(_editor.caret.line) * g.line;
  if (cy < y)
    y = cy;
  if (cy + g.line > y + g.height)
    y = cy + g.line - g.height;
  if (cx < x)
    x = cx;
  if (cx + caret_width > x + g.width)
    x = cx + caret_width - g.width;
  _widest_valid = false;
  _scroll_y = std::clamp(y, 0.0f, max_scroll_y(g));
  _scroll_x = std::max(0.0f, x);
  _pinned = _scroll_y >= max_scroll_y(g) - 0.5f;
}

detail::text_position_t text_edit_t::position_at(const geometry_t &g, point_t at) const noexcept
{
  const float y = float(at.y) - g.y + scroll_y(g);
  const float x = float(at.x) - g.x + _scroll_x;
  const auto line = std::size_t(std::clamp(std::floor(y / g.line), 0.0f, float(_editor.lines.size() - 1)));
  return _editor.at(line, std::size_t(std::max(0.0f, std::round(x / g.advance))));
}

void text_edit_t::trim()
{
  if (props.max_lines == 0 || _editor.lines.size() <= props.max_lines)
    return;
  const std::size_t drop = _editor.lines.size() - props.max_lines;
  _editor.lines.erase(_editor.lines.begin(), _editor.lines.begin() + std::ptrdiff_t(drop));
  for (auto *p : {&_editor.caret, &_editor.anchor})
    *p = p->line >= drop ? detail::text_position_t{p->line - drop, p->byte} : detail::text_position_t{};
  // What is in view stays in view, unless it was what went.
  _scroll_y = std::max(0.0f, _scroll_y - float(drop) * mono_metrics(theme().mono_size).line);
  _widest_valid = false;
}

void text_edit_t::edited_externally(scope_t &scope)
{
  trim();
  _editor.clamp();
  invalidate(scope, *this);
}

void text_edit_t::set_text(scope_t &scope, std::string_view text)
{
  _editor.set_text(text);
  _editor.caret = _editor.anchor = {};
  _scroll_x = _scroll_y = 0;
  _pinned = true;
  _widest_valid = false;
  edited_externally(scope);
}

void text_edit_t::append(scope_t &scope, std::string_view text)
{
  // Inserted at the end without moving the user's caret or selection.
  const auto caret = _editor.caret;
  const auto anchor = _editor.anchor;
  _editor.caret = _editor.anchor = {_editor.lines.size() - 1, _editor.lines.back().size()};
  _editor.insert(text);
  _editor.caret = caret;
  _editor.anchor = anchor;
  if (_widest_valid)
  {
    for (const auto &line : _editor.lines)
      _widest = std::max(_widest, code_points(line));
  }
  edited_externally(scope);
}

void text_edit_t::append_line(scope_t &scope, std::string_view line)
{
  if (_editor.lines.size() == 1 && _editor.lines.front().empty())
  {
    append(scope, line);
    return;
  }
  std::string text;
  text.reserve(line.size() + 1);
  text += '\n';
  text += line;
  append(scope, text);
}

void text_edit_t::clear(scope_t &scope)
{
  set_text(scope, std::string_view());
}

node_t text_edit_t::view() const
{
  const auto &t = theme();
  const bool on = props.enabled;
  const bool focused = on && _area.focused();
  const auto g = geometry();
  const float sy = scroll_y(g);
  const float sx = std::max(0.0f, _scroll_x);
  const auto &lines = _editor.lines;

  const auto first_line = std::size_t(sy / g.line);
  const auto last_line = std::min(lines.size(), std::size_t(std::ceil((sy + g.height) / g.line)) + 1);
  const auto first_column = std::size_t(sx / g.advance);
  const auto columns = std::size_t(std::ceil(g.width / g.advance)) + 2;

  box_t text_area{.size = {int(std::ceil(float(std::max(props.columns, 1)) * g.advance)), int(std::ceil(float(std::max(props.rows, 1)) * g.line))},
                  .color = {0, 0, 0, 0},
                  .align_x = align_t::start,
                  .align_y = align_t::start,
                  .clip = true};
  if (_editor.has_selection())
  {
    const auto a = _editor.selection_start();
    const auto b = _editor.selection_end();
    for (auto i = std::max(first_line, a.line); i < last_line && i <= b.line; ++i)
    {
      const float c0 = i == a.line ? float(_editor.column(a)) : 0.0f;
      // A selection that carries on past the end of a line takes the line break's cell too.
      const float c1 = i == b.line ? float(_editor.column(b)) : float(code_points(lines[i]) + 1);
      if (c1 <= c0)
        continue;
      text_area.children.push_back(placed(c0 * g.advance - sx, float(i) * g.line - sy, box_t{.size = {int(std::ceil((c1 - c0) * g.advance)), int(g.line)}, .color = t.selection}));
    }
  }
  for (auto i = first_line; i < last_line; ++i)
  {
    if (!lines[i].empty())
      text_area.children.push_back(grid_line(lines[i], first_column, columns, g.advance, -sx, float(i) * g.line - sy, t.mono_size, on ? t.text : t.text_disabled));
  }
  if (focused && !props.read_only)
    text_area.children.push_back(placed(float(_editor.column(_editor.caret)) * g.advance - sx, float(_editor.caret.line) * g.line - sy, box_t{.size = {2, int(g.line)}, .color = t.caret, .radius = caret_width / 2}));

  // A scrollbar over the right edge of the text, once there is more than fits and the edit knows its
  // size. Inside the clipping box, which does not grow to fit it: placed from last frame's bounds
  // anywhere that did, it would widen the edit, which would move it out again, every frame.
  const float content = float(lines.size()) * g.line;
  if (_area.bounds().height > 0 && content > g.height)
  {
    const float thumb = std::max(20.0f, g.height * g.height / content);
    const float y = max_scroll_y(g) > 0 ? sy / max_scroll_y(g) * (g.height - thumb) : 0.0f;
    text_area.children.push_back(placed(g.width - 7, y, box_t{.size = {6, int(thumb)}, .color = _thumb.held() || _thumb.hovered() ? t.border_hovered : t.scrollbar, .radius = 3, .events = _thumb}));
  }

  box_t whole{.color = on ? t.field : t.control_disabled,
              .radius = t.radius,
              .border_width = focused ? 2.0f : 1.0f,
              .border_color = focused ? t.focus_ring : (_area.hovered() && on ? t.border_hovered : t.border),
              .events = events_if(on, _area),
              .cursor = on ? std::optional(cursor_t::text) : std::nullopt,
              .padding = {area_pad_x, area_pad_y},
              .align_x = align_t::stretch,
              .align_y = align_t::stretch,
              .children = {std::move(text_area)}};
  return whole;
}

behaviour_t text_edit_t::behave(scope_t &scope)
{
  auto typed = subscribe<typing_event_t>(_area);
  auto drags = subscribe<drag_event_t>(_area, _thumb);
  auto wheels = subscribe<wheel_event_t>(_area, _thumb);
  auto events = merge(scope, typed, drags, wheels);
  const auto edited = [this, &scope](const geometry_t &g)
  {
    _widest_valid = false;
    keep_caret_in_view(g);
    emit(changed, text_edit_event_t{this}, scope);
  };

  while (auto event = co_await events)
  {
    if (!props.enabled)
      continue;
    const auto g = geometry();
    using outcome_t = detail::text_editor_t::outcome_t;
    if (event->index() == 2)
    {
      const auto &wheel = std::get<2>(*event);
      const bool sideways = has(wheel.modifiers, modifier_t::shift) || wheel.dx != 0;
      const double amount = sideways ? (wheel.dx != 0 ? wheel.dx : wheel.dy) : wheel.dy;
      if (sideways)
        scroll_to(g, _scroll_x - float(amount) * 3 * g.advance, scroll_y(g));
      else
        scroll_to(g, _scroll_x, scroll_y(g) - float(amount) * 3 * g.line);
      continue;
    }
    if (event->index() == 1)
    {
      const auto &drag = std::get<1>(*event);
      if (drag.source == &_thumb)
      {
        const float content = float(_editor.lines.size()) * g.line;
        const float thumb = std::max(20.0f, g.height * g.height / content);
        const float top = scroll_y(g) / std::max(max_scroll_y(g), 1.0f) * (g.height - thumb);
        if (drag.phase == drag_phase_t::begin)
          _grab = float(drag.position.y) - g.y - top;
        else if (drag.phase == drag_phase_t::move && _grab >= 0 && g.height > thumb)
          scroll_to(g, _scroll_x, (float(drag.position.y) - g.y - _grab) / (g.height - thumb) * max_scroll_y(g));
        if (drag.phase == drag_phase_t::end)
          _grab = -1;
        continue;
      }
      const auto at = position_at(g, drag.position);
      if (drag.phase == drag_phase_t::begin && drag.clicks == 2)
        _editor.select_word(at);
      else if (drag.phase == drag_phase_t::begin && drag.clicks >= 3)
        _editor.select_line(at.line);
      else if (drag.phase == drag_phase_t::begin)
        _editor.move_to(at, has(drag.modifiers, modifier_t::shift));
      else if (drag.phase == drag_phase_t::move && drag.clicks == 1)
        _editor.move_to(at, true);
      else
        continue;
      _editor.goal = detail::text_editor_t::npos;
      keep_caret_in_view(g);
      continue;
    }
    const auto page = std::size_t(std::max(1.0f, std::floor(g.height / g.line) - 1));
    const auto &input = std::get<0>(*event).input;
    const auto outcome = input.index() == 0 ? _editor.key(std::get<0>(input), page, props.read_only) : _editor.type(std::get<1>(input).text, props.read_only);
    switch (outcome)
    {
    case outcome_t::moved:
      keep_caret_in_view(g);
      break;
    case outcome_t::edited:
      edited(g);
      break;
    case outcome_t::copy:
      scope.window().set_clipboard_text(_editor.selected_text());
      break;
    case outcome_t::cut:
      scope.window().set_clipboard_text(_editor.selected_text());
      _editor.erase_selection();
      edited(g);
      break;
    case outcome_t::paste:
      if (auto pasted = co_await scope.window().clipboard_text(); pasted && !pasted->empty())
      {
        _editor.insert(*pasted);
        trim();
        _editor.clamp();
        edited(geometry());
      }
      break;
    case outcome_t::submit:
    case outcome_t::none:
      break;
    }
  }
}
} // namespace vui::ui
