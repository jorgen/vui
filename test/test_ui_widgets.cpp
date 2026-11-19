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

// G46: keyboard focus, popups, and the widgets built on them.

#include <doctest/doctest.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include <vio/event_loop.h>
#include <vui/vui.h>
#include <vui/ui/ui.h>
#include <vui/ui/widgets.h>
#include <vio/operation/sleep.h>
#include <vio/task.h>

#include "gui_test_helpers.h"

using namespace std::chrono_literals;
namespace ui = vui::ui;

namespace
{
// What the last layout put where, and what each place reports to.
struct seen_t
{
  ui::rect_t rect;
  const ui::pressable_t *source = nullptr;
};

using act_t = std::function<vio::task_t<void>(vio::event_loop_t &, vui::window_t &)>;

template <typename C>
void drive(C &root, std::vector<seen_t> &seen, act_t act)
{
  vio::event_loop_t loop;
  run_gui_test(loop,
               [](vio::event_loop_t &loop, C &root, std::vector<seen_t> &seen, act_t act) -> vio::task_t<void>
               {
                 vui::display_config_t display_config;
                 display_config.backend = "null";
                 display_config.frame_interval_ns = std::chrono::nanoseconds(2ms).count();
                 auto display = vui::display_t::open(loop, display_config);
                 REQUIRE(display.has_value());
                 {
                   auto window = vui::window_t::create(*display, {.size = {400, 300}});
                   ui::run_config_t config;
                   config.on_layout = [&seen](std::span<const ui::draw_item_t> items)
                   {
                     seen.clear();
                     for (const auto &item : items)
                       seen.push_back({item.rect, item.box->events.source});
                   };
                   auto running = ui::run(*window, root, std::move(config));
                   (void)co_await vio::sleep(loop, 40ms);
                   co_await act(loop, *window);
                   window->close();
                   co_await std::move(running);
                 }
                 loop.stop();
               }(loop, root, seen, std::move(act)));
}

vio::task_t<void> settle(vio::event_loop_t &loop)
{
  (void)co_await vio::sleep(loop, 40ms);
}

vui::point_t centre(const ui::rect_t &r)
{
  return {r.x + r.width / 2.0, r.y + r.height / 2.0};
}

void click(vui::window_t &window, vui::point_t at)
{
  window.impl()->on_input(vui::pointer_move_event_t{at});
  window.impl()->on_input(vui::pointer_button_event_t{vui::button_t::left, true, at});
  window.impl()->on_input(vui::pointer_button_event_t{vui::button_t::left, false, at});
}

void key(vui::window_t &window, vui::key_t k, vui::modifier_t modifiers = vui::modifier_t::none)
{
  window.impl()->on_input(vui::key_event_t{k, 0, modifiers, true});
  window.impl()->on_input(vui::key_event_t{k, 0, modifiers, false});
}

// The rect of the first box reporting to `source`.
ui::rect_t rect_of(const std::vector<seen_t> &seen, const ui::pressable_t *source)
{
  for (const auto &s : seen)
  {
    if (s.source == source)
      return s.rect;
  }
  return {};
}

std::size_t count_of(const std::vector<seen_t> &seen, const ui::pressable_t *source)
{
  std::size_t n = 0;
  for (const auto &s : seen)
    n += s.source == source ? 1 : 0;
  return n;
}
} // namespace

TEST_CASE("ui focus: Tab walks the focusable boxes in drawing order, and Space presses the focused one")
{
  struct root_t
  {
    ui::pressable_t a{ui::focusable}, b, c{ui::focusable};
    int presses_a = 0, presses_c = 0;

    ui::node_t view() const
    {
      return ui::row_t{.children = {ui::box_t{.size = {40, 40}, .events = a}, ui::box_t{.size = {40, 40}, .events = b}, ui::box_t{.size = {40, 40}, .events = c}}};
    }
    ui::behaviour_t behave(ui::scope_t &)
    {
      auto presses = ui::subscribe<ui::press_event_t>(a, c);
      while (auto press = co_await presses)
        ++(press->source == &a ? presses_a : presses_c);
    }
  } root;
  std::vector<seen_t> seen;
  drive(root, seen,
        [&root](vio::event_loop_t &loop, vui::window_t &window) -> vio::task_t<void>
        {
          CHECK_FALSE(root.a.focused());
          key(window, vui::key_t::tab);
          co_await settle(loop);
          CHECK(root.a.focused());
          CHECK(root.a.focus_visible());
          // b is not focusable, so it is skipped.
          key(window, vui::key_t::tab);
          co_await settle(loop);
          CHECK_FALSE(root.a.focused());
          CHECK(root.c.focused());
          key(window, vui::key_t::space);
          co_await settle(loop);
          CHECK(root.presses_c == 1);
          // Back round to a, and a click on c focuses it without a ring.
          key(window, vui::key_t::tab, vui::modifier_t::shift);
          co_await settle(loop);
          CHECK(root.a.focused());
          click(window, {100, 20});
          co_await settle(loop);
          CHECK(root.c.focused());
          CHECK_FALSE(root.c.focus_visible());
          CHECK(root.presses_c == 2);
          CHECK(root.presses_a == 0);
        });
}

TEST_CASE("ui raw input: keeps Tab and Shift+Tab, in order with its text, and focus stays on it")
{
  struct root_t
  {
    ui::pressable_t terminal{ui::raw_input}, other{ui::focusable};
    std::vector<std::string> typed;

    ui::node_t view() const
    {
      return ui::row_t{.children = {ui::box_t{.size = {80, 40}, .events = terminal}, ui::box_t{.size = {40, 40}, .events = other}}};
    }
    ui::behaviour_t behave(ui::scope_t &)
    {
      auto typing = ui::subscribe<ui::typing_event_t>(terminal);
      while (auto event = co_await typing)
      {
        if (const auto *k = std::get_if<vui::key_event_t>(&event->input))
        {
          if (k->pressed)
            typed.push_back(k->key == vui::key_t::tab ? (has(k->modifiers, vui::modifier_t::shift) ? "S-tab" : "tab") : "key");
        }
        else
          typed.push_back(std::get<vui::text_event_t>(event->input).text);
      }
    }
  } root;
  std::vector<seen_t> seen;
  drive(root, seen,
        [&root, &seen](vio::event_loop_t &loop, vui::window_t &window) -> vio::task_t<void>
        {
          CHECK(root.terminal.raw_input());
          CHECK(root.terminal.text_input());
          click(window, centre(rect_of(seen, &root.terminal)));
          co_await settle(loop);
          REQUIRE(root.terminal.focused());
          key(window, vui::key_t::tab);
          window.impl()->on_input(vui::text_event_t{"x"});
          key(window, vui::key_t::tab, vui::modifier_t::shift);
          co_await settle(loop);
          CHECK(root.terminal.focused());
          CHECK_FALSE(root.other.focused());
          CHECK(root.typed == std::vector<std::string>{"tab", "x", "S-tab"});
          // A press elsewhere still takes focus from it.
          click(window, centre(rect_of(seen, &root.other)));
          co_await settle(loop);
          CHECK(root.other.focused());
        });
}

TEST_CASE("ui raw input: every mouse button is pressed and released on the pressable that took it")
{
  struct root_t
  {
    ui::pressable_t terminal, beside;
    std::vector<ui::mouse_event_t> events;
    int contexts = 0;
    int navigates = 0;

    ui::node_t view() const
    {
      return ui::row_t{.children = {ui::box_t{.size = {80, 40}, .events = terminal}, ui::box_t{.size = {80, 40}, .events = beside}}};
    }
    ui::behaviour_t behave(ui::scope_t &scope)
    {
      scope.spawn(others(scope));
      auto mice = ui::subscribe<ui::mouse_event_t>(terminal);
      while (auto event = co_await mice)
        events.push_back(*event);
    }
    ui::behaviour_t others(ui::scope_t &scope)
    {
      auto contexts_seen = ui::subscribe<ui::context_event_t>(terminal);
      auto navigates_seen = ui::subscribe<ui::navigate_event_t>(terminal);
      auto both = ui::merge(scope, contexts_seen, navigates_seen);
      while (auto event = co_await both)
        ++(event->index() == 0 ? contexts : navigates);
    }
  } root;
  std::vector<seen_t> seen;
  drive(root, seen,
        [&root, &seen](vio::event_loop_t &loop, vui::window_t &window) -> vio::task_t<void>
        {
          const auto on = centre(rect_of(seen, &root.terminal));
          const auto off = centre(rect_of(seen, &root.beside));
          auto *impl = window.impl();
          using vui::button_t;
          impl->on_input(vui::pointer_move_event_t{on});
          co_await settle(loop);
          impl->on_input(vui::pointer_button_event_t{button_t::middle, true, on, vui::modifier_t::control});
          co_await settle(loop);
          // Dragged off with the middle button down, the moves and the right button stay with it.
          impl->on_input(vui::pointer_move_event_t{off});
          co_await settle(loop);
          impl->on_input(vui::pointer_button_event_t{button_t::right, true, off});
          impl->on_input(vui::pointer_button_event_t{button_t::middle, false, off});
          impl->on_input(vui::pointer_button_event_t{button_t::right, false, off});
          co_await settle(loop);
          // With nothing down, a move off it is not its; a move over it is, and a back button too.
          impl->on_input(vui::pointer_move_event_t{{off.x + 5, off.y}});
          co_await settle(loop);
          impl->on_input(vui::pointer_move_event_t{on});
          co_await settle(loop);
          impl->on_input(vui::pointer_button_event_t{button_t::back, true, on});
          impl->on_input(vui::pointer_button_event_t{button_t::back, false, on});
          co_await settle(loop);
        });
  const auto bit = [](vui::button_t b) { return std::uint8_t(1u << unsigned(b)); };
  using phase = ui::mouse_phase_t;
  const auto &e = root.events;
  REQUIRE(e.size() == 9);
  // The first move, over it with nothing down.
  CHECK(e[0].phase == phase::move);
  CHECK(e[0].held == 0);
  CHECK(e[0].button == vui::button_t::other);
  CHECK(e[1].phase == phase::press);
  CHECK(e[1].button == vui::button_t::middle);
  CHECK(e[1].held == bit(vui::button_t::middle));
  CHECK(e[1].modifiers == vui::modifier_t::control);
  CHECK(e[2].phase == phase::move);
  CHECK(e[2].button == vui::button_t::middle);
  CHECK(e[2].position.x > 80);
  CHECK(e[3].phase == phase::press);
  CHECK(e[3].button == vui::button_t::right);
  CHECK(e[3].held == (bit(vui::button_t::middle) | bit(vui::button_t::right)));
  CHECK(e[4].phase == phase::release);
  CHECK(e[4].button == vui::button_t::middle);
  CHECK(e[5].phase == phase::release);
  CHECK(e[5].button == vui::button_t::right);
  CHECK(e[5].held == 0);
  CHECK(e[6].phase == phase::move);
  CHECK(e[7].phase == phase::press);
  CHECK(e[7].button == vui::button_t::back);
  CHECK(e[8].phase == phase::release);
  // The right press off it opened no context menu on it; the back button still navigates.
  CHECK(root.contexts == 0);
  CHECK(root.navigates == 1);
}

TEST_CASE("ui popup: takes no room, is drawn last and kept in the window, and a press outside it dismisses")
{
  struct menu_t
  {
    ui::pressable_t item;
    ui::node_t view() const
    {
      return ui::box_t{.size = {150, 100}, .color = {1, 0, 0, 1}, .events = item};
    }
  };
  struct root_t
  {
    std::shared_ptr<menu_t> menu = std::make_shared<menu_t>();
    ui::pressable_t under, close;
    bool open = true;
    int under_presses = 0, closes = 0;

    ui::node_t view() const
    {
      ui::column_t column{.children = {ui::box_t{.size = {50, 20}}}};
      if (open)
        column.children.push_back(ui::mount(menu).on_top(close));
      column.children.push_back(ui::box_t{.size = {50, 20}, .events = under});
      // Placed at x 300, which leaves the 150-wide popup 100 short of the 400-wide window.
      return ui::at_t{.x = 300, .y = 0, .children = {std::move(column)}};
    }
    ui::behaviour_t behave(ui::scope_t &)
    {
      auto presses = ui::subscribe<ui::press_event_t>(under, close);
      while (auto press = co_await presses)
      {
        if (press->source == &under)
          ++under_presses;
        else
        {
          ++closes;
          open = false;
        }
      }
    }
  } root;
  std::vector<seen_t> seen;
  drive(root, seen,
        [&](vio::event_loop_t &loop, vui::window_t &window) -> vio::task_t<void>
        {
          // The box after the popup sits right under the first: the popup took no room.
          CHECK(rect_of(seen, &root.under) == ui::rect_t{300, 20, 50, 20});
          // Moved left to fit, and drawn after `under`, which comes later in the view.
          CHECK(rect_of(seen, &root.menu->item) == ui::rect_t{250, 20, 150, 100});
          CHECK(rect_of(seen, &root.close) == ui::rect_t{0, 0, 400, 300});
          std::size_t under_at = 0, item_at = 0;
          for (std::size_t i = 0; i < seen.size(); ++i)
          {
            under_at = seen[i].source == &root.under ? i : under_at;
            item_at = seen[i].source == &root.menu->item ? i : item_at;
          }
          CHECK(item_at > under_at);
          // `under` is covered by the popup, so this lands on the popup and nothing happens.
          click(window, {320, 30});
          co_await settle(loop);
          CHECK(root.under_presses == 0);
          CHECK(root.closes == 0);
          // Outside the popup: the backdrop takes it, not what is under it.
          click(window, {20, 250});
          co_await settle(loop);
          CHECK(root.closes == 1);
          CHECK(count_of(seen, &root.close) == 0);
          click(window, {320, 30});
          co_await settle(loop);
          CHECK(root.under_presses == 1);
        });
}

TEST_CASE("ui widgets: a checkbox asks to change and the parent decides; disabled, it asks nothing")
{
  struct root_t
  {
    bool on = false;
    bool enabled = true;
    int changes = 0;
    ui::checkbox_t box;
    ui::node_t view() const
    {
      return ui::mount(box, {.label = "Remember", .checked = on, .enabled = enabled});
    }
    ui::behaviour_t behave(ui::scope_t &)
    {
      auto changes_ = ui::subscribe<ui::toggle_event_t>(box.changed);
      while (auto change = co_await changes_)
      {
        CHECK(change->source == &box);
        on = change->checked;
        // Disabled after the second, from here, so that the parent is rebuilt with it.
        if (++changes == 2)
          enabled = false;
      }
    }
  } root;
  std::vector<seen_t> seen;
  drive(root, seen,
        [&](vio::event_loop_t &loop, vui::window_t &window) -> vio::task_t<void>
        {
          REQUIRE(!seen.empty());
          // The first hit-testable box is the whole checkbox, label and all.
          ui::rect_t whole;
          for (const auto &s : seen)
          {
            if (s.source)
            {
              whole = s.rect;
              break;
            }
          }
          CHECK(whole.width > 60);
          click(window, centre(whole));
          co_await settle(loop);
          CHECK(root.on);
          CHECK(root.box.props.checked);
          key(window, vui::key_t::space);
          co_await settle(loop);
          CHECK_FALSE(root.on);
          CHECK(root.changes == 2);
          CHECK_FALSE(root.enabled);
          click(window, centre(whole));
          key(window, vui::key_t::space);
          co_await settle(loop);
          CHECK(root.changes == 2);
        });
}

TEST_CASE("ui widgets: a combobox opens over what follows, picks by pointer and by keyboard, and closes")
{
  struct root_t
  {
    int selected = -1;
    std::vector<std::size_t> picks;
    ui::combobox_t combo;
    ui::button_t after;
    int after_presses = 0;
    ui::node_t view() const
    {
      return ui::column_t{.gap = 8, .children = {ui::mount(combo, {.options = {"one", "two", "three"}, .selected = selected, .placeholder = "choose"}), ui::mount(after, {.label = "after"})}};
    }
    ui::behaviour_t behave(ui::scope_t &scope)
    {
      auto selections = ui::subscribe<ui::select_event_t>(combo.changed);
      auto presses = ui::subscribe<ui::press_event_t>(after.press);
      auto events = ui::merge(scope, selections, presses);
      while (auto event = co_await events)
      {
        if (event->index() == 0)
        {
          selected = int(std::get<0>(*event).index);
          picks.push_back(std::get<0>(*event).index);
        }
        else
          ++after_presses;
      }
    }
  } root;
  std::vector<seen_t> seen;
  drive(root, seen,
        [&](vio::event_loop_t &loop, vui::window_t &window) -> vio::task_t<void>
        {
          const auto after_rect = rect_of(seen, &root.after.press);
          REQUIRE(after_rect.width > 0);
          // The field is the box that is neither the button nor anything else pressable yet.
          ui::rect_t field;
          for (const auto &s : seen)
          {
            if (s.source && s.source != &root.after.press)
            {
              field = s.rect;
              break;
            }
          }
          REQUIRE(field.width > 0);
          click(window, centre(field));
          co_await settle(loop);
          REQUIRE(root.combo.open());
          // Opening moved nothing.
          CHECK(rect_of(seen, &root.after.press) == after_rect);
          // Three rows under the field, as wide as it, the second over the button.
          std::vector<ui::rect_t> rows;
          bool past_backdrop = false;
          for (const auto &s : seen)
          {
            if (s.rect == ui::rect_t{0, 0, 400, 300} && s.source)
              past_backdrop = true;
            else if (past_backdrop && s.source)
              rows.push_back(s.rect);
          }
          REQUIRE(rows.size() == 3);
          CHECK(rows[0].y >= field.y + field.height);
          // Inside the menu's 1-unit border; the menu itself is as wide as the field, which the
          // placeholder, wider than any option, decides.
          CHECK(rows[0].width == field.width - 2);
          click(window, centre(rows[1]));
          co_await settle(loop);
          CHECK_FALSE(root.combo.open());
          CHECK(root.selected == 1);
          CHECK(root.after_presses == 0);

          // The keyboard: it has focus from the click. Down changes it closed; Enter opens, Down
          // moves, Enter picks; Escape closes without picking.
          key(window, vui::key_t::down);
          co_await settle(loop);
          CHECK(root.selected == 2);
          key(window, vui::key_t::up);
          key(window, vui::key_t::up);
          co_await settle(loop);
          CHECK(root.selected == 0);
          key(window, vui::key_t::enter);
          co_await settle(loop);
          CHECK(root.combo.open());
          key(window, vui::key_t::down);
          key(window, vui::key_t::down);
          key(window, vui::key_t::enter);
          co_await settle(loop);
          CHECK_FALSE(root.combo.open());
          CHECK(root.selected == 2);
          key(window, vui::key_t::enter);
          key(window, vui::key_t::up);
          key(window, vui::key_t::escape);
          co_await settle(loop);
          CHECK_FALSE(root.combo.open());
          CHECK(root.selected == 2);

          // A press outside the open menu closes it and reaches nothing else.
          click(window, centre(field));
          co_await settle(loop);
          REQUIRE(root.combo.open());
          click(window, {390, 290});
          co_await settle(loop);
          CHECK_FALSE(root.combo.open());
          CHECK(root.picks == std::vector<std::size_t>{1, 2, 1, 0, 2});
        });
}
