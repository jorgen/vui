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

// G50: clipping, the input a text field needs, and the label, line edit and text edit built on it.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include <vio/event_loop.h>
#include <vio/operation/sleep.h>
#include <vio/task.h>
#include <vui/ui/ui.h>
#include <vui/ui/widgets.h>
#include <vui/vui.h>

#include "gui_test_helpers.h"

using namespace std::chrono_literals;
namespace ui = vui::ui;

namespace
{
struct seen_t
{
  ui::rect_t rect;
  const ui::pressable_t *source = nullptr;
};

struct frame_t
{
  std::vector<seen_t> items;
  std::vector<ui::run_t> runs;
};

using act_t = std::function<vio::task_t<void>(vio::event_loop_t &, vui::window_t &)>;

template <typename C>
void drive(C &root, frame_t &frame, act_t act)
{
  vio::event_loop_t loop;
  run_gui_test(loop,
               [](vio::event_loop_t &loop, C &root, frame_t &frame, act_t act) -> vio::task_t<void>
               {
                 vui::display_config_t display_config;
                 display_config.backend = "null";
                 display_config.frame_interval_ns = std::chrono::nanoseconds(2ms).count();
                 auto display = vui::display_t::open(loop, display_config);
                 REQUIRE(display.has_value());
                 {
                   auto window = vui::window_t::create(*display, {.size = {400, 300}});
                   ui::run_config_t config;
                   config.on_layout = [&frame](std::span<const ui::draw_item_t> items)
                   {
                     frame.items.clear();
                     for (const auto &item : items)
                       frame.items.push_back({item.rect, item.box->events.source});
                   };
                   config.on_display_list = [&frame](const ui::display_list_t &list) { frame.runs.assign(list.runs.begin(), list.runs.end()); };
                   auto running = ui::run(*window, root, std::move(config));
                   (void)co_await vio::sleep(loop, 40ms);
                   co_await act(loop, *window);
                   window->close();
                   co_await std::move(running);
                 }
                 loop.stop();
               }(loop, root, frame, std::move(act)));
}

vio::task_t<void> settle(vio::event_loop_t &loop)
{
  (void)co_await vio::sleep(loop, 40ms);
}

void click(vui::window_t &window, vui::point_t at, vui::modifier_t modifiers = vui::modifier_t::none)
{
  window.impl()->on_input(vui::pointer_move_event_t{at});
  window.impl()->on_input(vui::pointer_button_event_t{vui::button_t::left, true, at, modifiers});
  window.impl()->on_input(vui::pointer_button_event_t{vui::button_t::left, false, at, modifiers});
}

void key(vui::window_t &window, vui::key_t k, vui::modifier_t modifiers = vui::modifier_t::none)
{
  window.impl()->on_input(vui::key_event_t{k, 0, modifiers, true});
  window.impl()->on_input(vui::key_event_t{k, 0, modifiers, false});
}

void type(vui::window_t &window, std::string text)
{
  window.impl()->on_input(vui::text_event_t{std::move(text)});
}

ui::rect_t rect_of(const frame_t &frame, const ui::pressable_t *source)
{
  for (const auto &s : frame.items)
  {
    if (s.source == source)
      return s.rect;
  }
  return {};
}

constexpr auto ctrl = vui::modifier_t::control;
constexpr auto shift = vui::modifier_t::shift;
// What moves by word: Option on macOS, Control elsewhere.
#if defined(__APPLE__)
constexpr auto word = vui::modifier_t::alt;
#else
constexpr auto word = vui::modifier_t::control;
#endif
} // namespace

TEST_CASE("ui clip: a clipping box keeps its size, and cuts what it holds when drawn and hit-tested")
{
  struct root_t
  {
    ui::pressable_t inside;
    ui::pressable_t below;
    int presses = 0;

    ui::node_t view() const
    {
      return ui::column_t{.padding = 10,
                          .children = {ui::box_t{.size = {100, 40},
                                                 .color = {0.2f, 0.2f, 0.2f, 1},
                                                 .border_width = 2,
                                                 .align_x = ui::align_t::start,
                                                 .align_y = ui::align_t::start,
                                                 .clip = true,
                                                 .children = {ui::box_t{.size = {300, 200}, .color = {1, 0, 0, 1}, .events = inside}}},
                                       ui::box_t{.size = {50, 20}, .color = {0, 0, 1, 1}, .events = below}}};
    }
    ui::behaviour_t behave(ui::scope_t &)
    {
      auto presses_ = ui::subscribe<ui::press_event_t>(inside);
      while (co_await presses_)
        ++presses;
    }
  } root;
  frame_t frame;
  drive(root, frame,
        [&](vio::event_loop_t &loop, vui::window_t &window) -> vio::task_t<void>
        {
          // Cut to the box less its border; what follows sits under the box's own 40, not the 200.
          CHECK(rect_of(frame, &root.inside) == ui::rect_t{12, 12, 96, 36});
          CHECK(rect_of(frame, &root.below) == ui::rect_t{10, 50, 50, 20});
          CHECK(root.inside.bounds() == ui::rect_t{12, 12, 96, 36});
          const auto cut = std::find_if(frame.runs.begin(), frame.runs.end(), [](const ui::run_t &run) { return run.clip[0] > -ui::run_t::clip_none; });
          REQUIRE(cut != frame.runs.end());
          CHECK(cut->clip[0] == doctest::Approx(12));
          CHECK(cut->clip[1] == doctest::Approx(12));
          CHECK(cut->clip[2] == doctest::Approx(108));
          CHECK(cut->clip[3] == doctest::Approx(48));
          // A press where the red box would be, were it not cut, reaches nothing.
          click(window, {200, 30});
          click(window, {50, 30});
          co_await settle(loop);
          CHECK(root.presses == 1);
        });
}

TEST_CASE("ui label: one text per line, quieter when muted")
{
  const auto one = ui::label("Name");
  REQUIRE(std::holds_alternative<ui::text_t>(one));
  CHECK(std::get<ui::text_t>(one).text == "Name");
  const auto two = ui::label({.text = "a\nb", .muted = true});
  REQUIRE(std::holds_alternative<ui::column_t>(two));
  const auto &lines = std::get<ui::column_t>(two).children;
  REQUIRE(lines.size() == 2);
  CHECK(std::get<ui::text_t>(lines[1]).text == "b");
  CHECK(std::get<ui::text_t>(lines[1]).color == ui::theme().text_muted);
  const auto mono = ui::label({.text = "0x1f", .monospace = true});
  CHECK(std::get<ui::text_t>(mono).font == ui::monospace_font());
}

TEST_CASE("ui editor: keys move, select and edit by code point, word and line")
{
  ui::detail::text_editor_t e(true);
  e.set_text("hello w\xc3\xb6rld\nsecond");
  CHECK(e.lines.size() == 2);
  CHECK(e.caret == ui::detail::text_position_t{1, 6});
  const auto press = [&](vui::key_t k, vui::modifier_t m = vui::modifier_t::none) { return e.key(vui::key_event_t{k, 0, m, true}, 10, false); };

  press(vui::key_t::up);
  CHECK(e.caret == ui::detail::text_position_t{0, 6});
  press(vui::key_t::end);
  // "ö" is two bytes and one column.
  CHECK(e.column(e.caret) == 11);
  CHECK(e.caret.byte == 12);
  press(vui::key_t::left);
  press(vui::key_t::left);
  press(vui::key_t::left);
  press(vui::key_t::left);
  CHECK(e.caret.byte == 7);
  press(vui::key_t::left, word);
  CHECK(e.caret.byte == 6);
  press(vui::key_t::left, word);
  CHECK(e.caret.byte == 0);
  press(vui::key_t::right, word | shift);
  CHECK(e.selected_text() == "hello ");
  CHECK(e.type("bye ", false) == ui::detail::text_editor_t::outcome_t::edited);
  CHECK(e.text() == "bye w\xc3\xb6rld\nsecond");
  press(vui::key_t::end, ctrl);
  press(vui::key_t::up, shift);
  CHECK(e.selected_text() == "rld\nsecond");
  CHECK(press(vui::key_t::backspace) == ui::detail::text_editor_t::outcome_t::edited);
  CHECK(e.text() == "bye w\xc3\xb6");
  CHECK(press(vui::key_t::enter) == ui::detail::text_editor_t::outcome_t::edited);
  e.insert("a\tb\x01\r");
  CHECK(e.text() == "bye w\xc3\xb6\na   b");
  CHECK(press(vui::key_t::c, ctrl) == ui::detail::text_editor_t::outcome_t::none);
  press(vui::key_t::a, ctrl);
  CHECK(press(vui::key_t::c, ctrl) == ui::detail::text_editor_t::outcome_t::copy);
  CHECK(e.key(vui::key_event_t{vui::key_t::x, 0, ctrl, true}, 10, true) == ui::detail::text_editor_t::outcome_t::none);
  CHECK(e.type("z", true) == ui::detail::text_editor_t::outcome_t::none);

  ui::detail::text_editor_t line(false);
  line.insert("one\ntwo");
  CHECK(line.text() == "one two");
  CHECK(line.key(vui::key_event_t{vui::key_t::enter, 0, vui::modifier_t::none, true}, 1, false) == ui::detail::text_editor_t::outcome_t::submit);
  line.select_word({0, 5});
  CHECK(line.selected_text() == "two");
}

TEST_CASE("ui line edit: typed text is asked for, the parent decides, and Enter submits")
{
  struct root_t
  {
    std::string text = "ab";
    std::vector<std::string> submitted;
    bool digits_only = false;
    int refused = 0;
    ui::line_edit_t edit;

    ui::node_t view() const
    {
      return ui::column_t{.padding = 10, .children = {ui::label("Name"), ui::mount(edit, {.text = text, .placeholder = "nobody"})}};
    }
    ui::behaviour_t behave(ui::scope_t &scope)
    {
      auto changes = ui::subscribe<ui::line_edit_event_t>(edit.changed);
      auto submits = ui::subscribe<ui::line_edit_event_t>(edit.submitted);
      auto events = ui::merge(scope, changes, submits);
      while (auto event = co_await events)
      {
        const auto &e = event->index() == 0 ? std::get<0>(*event) : std::get<1>(*event);
        CHECK(e.source == &edit);
        if (event->index() == 1)
          submitted.push_back(e.text);
        else if (digits_only && !std::all_of(e.text.begin(), e.text.end(), [](char c) { return c >= '0' && c <= '9'; }))
          ++refused;
        else
          text = e.text;
      }
    }
  } root;
  frame_t frame;
  drive(root, frame,
        [&](vio::event_loop_t &loop, vui::window_t &window) -> vio::task_t<void>
        {
          const auto field = rect_of(frame, &root.edit.field());
          REQUIRE(field.width > 0);
          // A click well past the end puts the caret at the end, and focuses it.
          click(window, {double(field.x + field.width - 4), double(field.y + field.height / 2)});
          co_await settle(loop);
          CHECK(root.edit.field().focused());
          CHECK(root.edit.caret() == 2);
          type(window, "cd");
          co_await settle(loop);
          CHECK(root.text == "abcd");
          key(window, vui::key_t::backspace);
          // Two keys in one turn: the second builds on the first, not on the props both saw.
          key(window, vui::key_t::left, shift);
          key(window, vui::key_t::left, shift);
          type(window, "X");
          co_await settle(loop);
          CHECK(root.text == "aX");
          // Space is text here, not a press.
          type(window, " ");
          key(window, vui::key_t::space);
          co_await settle(loop);
          CHECK(root.text == "aX ");
          key(window, vui::key_t::enter);
          co_await settle(loop);
          REQUIRE(root.submitted.size() == 1);
          CHECK(root.submitted[0] == "aX ");
          // Through the clipboard.
          key(window, vui::key_t::a, ctrl);
          key(window, vui::key_t::c, ctrl);
          key(window, vui::key_t::end);
          key(window, vui::key_t::v, ctrl);
          co_await settle(loop);
          CHECK(root.text == "aX aX ");
          // Refused: the edit shows the parent's text again, and the caret stays inside it.
          root.digits_only = true;
          type(window, "z");
          co_await settle(loop);
          CHECK(root.refused == 1);
          CHECK(root.text == "aX aX ");
          CHECK(root.edit.props.text == "aX aX ");
          CHECK(root.edit.caret() <= root.text.size());
        });
}

TEST_CASE("ui text edit: lines are typed, navigated and selected, and a click places the caret")
{
  struct root_t
  {
    ui::text_edit_t edit{"one\ntwo"};
    int changes = 0;

    ui::node_t view() const
    {
      return ui::column_t{.padding = 10, .children = {ui::mount(edit, {.columns = 20, .rows = 4})}};
    }
    ui::behaviour_t behave(ui::scope_t &)
    {
      auto changes_ = ui::subscribe<ui::text_edit_event_t>(edit.changed);
      while (co_await changes_)
        ++changes;
    }
  } root;
  frame_t frame;
  drive(root, frame,
        [&](vio::event_loop_t &loop, vui::window_t &window) -> vio::task_t<void>
        {
          const auto area = rect_of(frame, &root.edit.area());
          REQUIRE(area.width > 0);
          CHECK(root.edit.area().bounds() == area);
          // The top left of the text: line 0, column 0.
          click(window, {double(area.x + 7), double(area.y + 5)});
          co_await settle(loop);
          CHECK(root.edit.area().focused());
          CHECK(root.edit.caret() == ui::detail::text_position_t{0, 0});
          key(window, vui::key_t::end);
          key(window, vui::key_t::enter);
          type(window, "x");
          co_await settle(loop);
          CHECK(root.edit.text() == "one\nx\ntwo");
          CHECK(root.changes == 2);
          key(window, vui::key_t::end, ctrl);
          key(window, vui::key_t::up, shift);
          co_await settle(loop);
          CHECK(root.edit.selected_text() == "\ntwo");
          // A click below the last line lands on it.
          click(window, {double(area.x + 7), double(area.y + area.height - 3)});
          co_await settle(loop);
          CHECK(root.edit.caret() == ui::detail::text_position_t{2, 0});
          // A double click selects the word under it.
          click(window, {double(area.x + 7), double(area.y + 5)});
          click(window, {double(area.x + 7), double(area.y + 5)});
          co_await settle(loop);
          CHECK(root.edit.selected_text() == "one");
        });
}

TEST_CASE("ui text edit: a read-only log takes lines from its parent, keeps the newest and scrolls")
{
  struct root_t
  {
    ui::text_edit_t log;
    int fed = 0;
    bool done = false;

    ui::node_t view() const
    {
      return ui::column_t{.padding = 10, .children = {ui::mount(log, {.read_only = true, .columns = 30, .rows = 3, .follow = true, .max_lines = 50})}};
    }
    ui::behaviour_t behave(ui::scope_t &scope)
    {
      auto ticks = ui::every(scope, 1ms);
      while (fed < 100 && co_await ticks)
      {
        // Two at a time, so a frame often sees more than one arrive.
        log.append_line(scope, "line " + std::to_string(fed++));
        log.append_line(scope, "line " + std::to_string(fed++));
      }
      done = true;
    }
  } root;
  frame_t frame;
  drive(root, frame,
        [&](vio::event_loop_t &loop, vui::window_t &window) -> vio::task_t<void>
        {
          for (int i = 0; i < 100 && !root.done; ++i)
            co_await settle(loop);
          REQUIRE(root.done);
          co_await settle(loop);
          CHECK(root.log.line_count() == 50);
          CHECK(root.log.line(0) == "line 50");
          CHECK(root.log.line(49) == "line 99");
          // More than fits: a scrollbar, a second source in the hit-test list.
          const auto area = rect_of(frame, &root.log.area());
          std::size_t sources = 0;
          for (const auto &s : frame.items)
            sources += s.source && s.source != &root.log.area() ? 1 : 0;
          CHECK(sources == 1);
          // Read-only: typing changes nothing, the wheel and keys still move.
          click(window, {double(area.x + 20), double(area.y + 10)});
          type(window, "nope");
          key(window, vui::key_t::backspace);
          window.impl()->on_input(vui::scroll_event_t{0, 2, {double(area.x + 20), double(area.y + 10)}});
          co_await settle(loop);
          CHECK(root.log.line_count() == 50);
          CHECK(root.log.line(49) == "line 99");
          key(window, vui::key_t::a, ctrl);
          co_await settle(loop);
          CHECK(root.log.selected_text().starts_with("line 50\nline 51\n"));
        });
}

TEST_CASE("ui hover: a subscriber is told where the pointer is over its pressable, and when it leaves")
{
  struct root_t
  {
    ui::pressable_t area;
    std::vector<ui::hover_event_t> seen;

    ui::node_t view() const
    {
      return ui::column_t{.padding = 10, .children = {ui::box_t{.size = {100, 50}, .color = {0.2f, 0.2f, 0.2f, 1}, .events = area}}};
    }
    ui::behaviour_t behave(ui::scope_t &)
    {
      auto hovers = ui::subscribe<ui::hover_event_t>(area);
      while (auto hover = co_await hovers)
        seen.push_back(*hover);
    }
  } root;
  frame_t frame;
  drive(root, frame,
        [&](vio::event_loop_t &loop, vui::window_t &window) -> vio::task_t<void>
        {
          // The window keeps only the latest of the moves queued in one turn, so one per turn.
          window.impl()->on_input(vui::pointer_move_event_t{{30, 20}});
          co_await settle(loop);
          window.impl()->on_input(vui::pointer_move_event_t{{60, 40}});
          co_await settle(loop);
          window.impl()->on_input(vui::pointer_move_event_t{{300, 200}});
          co_await settle(loop);
          REQUIRE(root.seen.size() == 3);
          CHECK(root.seen[0].source == &root.area);
          CHECK(root.seen[0].inside);
          CHECK(root.seen[0].position == vui::point_t{30, 20});
          CHECK(root.seen[1].position == vui::point_t{60, 40});
          CHECK_FALSE(root.seen[2].inside);
        });
}

TEST_CASE("ui press: a pressable nothing listens to for presses lets the press through to the one under it")
{
  struct root_t
  {
    ui::pressable_t card;
    ui::pressable_t chart;
    ui::pressable_t button;
    int card_presses = 0;
    int button_presses = 0;
    int hovers = 0;

    ui::node_t view() const
    {
      // The chart only tracks the pointer; the button is listened to.
      return ui::column_t{.padding = 10,
                          .children = {ui::box_t{.size = {200, 80},
                                                 .color = {0.2f, 0.2f, 0.2f, 1},
                                                 .events = card,
                                                 .children = {ui::row_t{.gap = 10,
                                                                        .children = {ui::box_t{.size = {80, 40}, .color = {0, 0, 1, 1}, .events = chart},
                                                                                     ui::box_t{.size = {80, 40}, .color = {0, 1, 0, 1}, .events = button}}}}}}};
    }
    ui::behaviour_t behave(ui::scope_t &scope)
    {
      auto presses = ui::subscribe<ui::press_event_t>(card, button);
      auto moves = ui::subscribe<ui::hover_event_t>(chart);
      auto events = ui::merge(scope, presses, moves);
      while (auto event = co_await events)
      {
        if (event->index() == 1)
          ++hovers;
        else
          ++(std::get<0>(*event).source == &card ? card_presses : button_presses);
      }
    }
  } root;
  frame_t frame;
  drive(root, frame,
        [&](vio::event_loop_t &loop, vui::window_t &window) -> vio::task_t<void>
        {
          const auto chart = rect_of(frame, &root.chart);
          const auto button = rect_of(frame, &root.button);
          REQUIRE(chart.width > 0);
          click(window, {chart.x + chart.width / 2.0, chart.y + chart.height / 2.0});
          co_await settle(loop);
          CHECK(root.card_presses == 1);
          CHECK(root.hovers > 0);
          // A listened-to pressable on top still takes its own presses.
          click(window, {button.x + button.width / 2.0, button.y + button.height / 2.0});
          co_await settle(loop);
          CHECK(root.button_presses == 1);
          CHECK(root.card_presses == 1);
        });
}

TEST_CASE("ui context: the right button goes to the topmost pressable with a context subscriber")
{
  struct root_t
  {
    ui::pressable_t list;
    ui::pressable_t row;
    std::vector<ui::context_event_t> seen;

    ui::node_t view() const
    {
      // The row only tracks the pointer, so the list under it takes the right button.
      return ui::column_t{.padding = 10,
                          .children = {ui::box_t{.size = {200, 100}, .color = {0.2f, 0.2f, 0.2f, 1}, .events = list, .children = {ui::box_t{.size = {100, 30}, .color = {0, 0, 1, 1}, .events = row}}}}};
    }
    ui::behaviour_t behave(ui::scope_t &scope)
    {
      auto menus = ui::subscribe<ui::context_event_t>(list);
      auto moves = ui::subscribe<ui::hover_event_t>(row);
      auto events = ui::merge(scope, menus, moves);
      while (auto event = co_await events)
      {
        if (event->index() == 0)
          seen.push_back(std::get<0>(*event));
      }
    }
  } root;
  frame_t frame;
  drive(root, frame,
        [&](vio::event_loop_t &loop, vui::window_t &window) -> vio::task_t<void>
        {
          const auto row = rect_of(frame, &root.row);
          const vui::point_t at{row.x + 20.0, row.y + 10.0};
          window.impl()->on_input(vui::pointer_button_event_t{vui::button_t::right, true, at});
          window.impl()->on_input(vui::pointer_button_event_t{vui::button_t::right, false, at});
          co_await settle(loop);
          REQUIRE(root.seen.size() == 1);
          CHECK(root.seen[0].source == &root.list);
          CHECK(root.seen[0].position == at);
          // A left click is not a context event.
          click(window, at);
          co_await settle(loop);
          CHECK(root.seen.size() == 1);
        });
}

TEST_CASE("ui context menu: opens where placed, chooses by pointer and keyboard, and is dismissed outside")
{
  struct root_t
  {
    ui::context_menu_t menu;
    ui::pressable_t keys{ui::focusable};
    bool open = true;
    std::vector<std::size_t> chosen;
    int dismissed = 0;

    ui::node_t view() const
    {
      ui::box_t page{.size = {400, 300}, .color = {0, 0, 0, 0}, .events = keys, .align_x = ui::align_t::start, .align_y = ui::align_t::start};
      if (open)
        page.children.push_back(ui::at_t{.x = 50,
                                         .y = 40,
                                         .children = {ui::mount(menu, {.items = {{.label = "Open"},
                                                                                 {.kind = ui::menu_item_t::kind_t::separator},
                                                                                 {.label = "Open with", .kind = ui::menu_item_t::kind_t::header},
                                                                                 {.label = "Unavailable", .enabled = false},
                                                                                 {.label = "Notepad", .detail = "default"}}})
                                                        .on_top(menu.dismiss)}});
      return page;
    }
    ui::behaviour_t behave(ui::scope_t &scope)
    {
      auto choices = ui::subscribe<ui::menu_choice_t>(menu.chosen);
      auto outside = ui::subscribe<ui::press_event_t>(menu.dismiss);
      auto typed = ui::subscribe<vui::key_event_t>(keys);
      auto events = ui::merge(scope, choices, outside, typed);
      while (auto event = co_await events)
      {
        if (event->index() == 0)
          chosen.push_back(std::get<0>(*event).index);
        else if (event->index() == 1)
        {
          ++dismissed;
          open = false;
        }
        else
          menu.key(std::get<2>(*event), scope);
      }
    }
  } root;
  frame_t frame;
  drive(root, frame,
        [&](vio::event_loop_t &loop, vui::window_t &window) -> vio::task_t<void>
        {
          // Rows that can be chosen report to their own pressables; the header, the rule and the
          // disabled row report to nothing.
          std::vector<ui::rect_t> rows;
          for (const auto &s : frame.items)
          {
            if (s.source && s.source != &root.keys && s.source != &root.menu.dismiss)
              rows.push_back(s.rect);
          }
          REQUIRE(rows.size() == 2);
          CHECK(rows[0].x >= 50);
          CHECK(rows[0].y >= 40);
          click(window, {rows[1].x + 10.0, rows[1].y + 5.0});
          co_await settle(loop);
          REQUIRE(root.chosen.size() == 1);
          CHECK(root.chosen[0] == 4);
          // From the keyboard, handed over by the parent: Down lands on Open, Down again skips the
          // rule, the header and the disabled row.
          key(window, vui::key_t::tab);
          key(window, vui::key_t::down);
          key(window, vui::key_t::down);
          key(window, vui::key_t::enter);
          co_await settle(loop);
          REQUIRE(root.chosen.size() == 2);
          CHECK(root.chosen[1] == 4);
          click(window, {380, 280});
          co_await settle(loop);
          CHECK(root.dismissed == 1);
          CHECK_FALSE(root.open);
        });
}
