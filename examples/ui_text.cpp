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

// Text (G50): labels, line edits and two text edits -- one to type notes into, one a log that a
// timer and the command line write to. The line edits are controlled: the port only takes digits
// because this form refuses anything else.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <vio/event_loop.h>
#include <vio/operation/sleep.h>
#include <vio/run.h>
#include <vui/ui/ui.h>
#include <vui/ui/widgets.h>
#include <vui/vui.h>

namespace ui = vui::ui;
using namespace std::chrono_literals;

namespace
{
constexpr ui::color_t panel{0.16f, 0.16f, 0.19f, 1};

ui::node_t field(const char *name, ui::node_t control)
{
  return ui::row_t{.gap = 12,
                   .align = ui::align_t::center,
                   .children = {ui::box_t{.size = {90, 0}, .color = {0, 0, 0, 0}, .align_x = ui::align_t::start, .children = {ui::label({.text = name, .muted = true})}}, std::move(control)}};
}

// A child that takes the room it is given: a share of its row, all of its height.
ui::node_t filling(ui::node_t child)
{
  return ui::box_t{.color = {0, 0, 0, 0}, .grow = 1, .align_x = ui::align_t::stretch, .align_y = ui::align_t::stretch, .children = {std::move(child)}};
}

struct console_t
{
  std::string host = "localhost";
  std::string port = "8080";
  std::string command;
  int ticks = 0;

  ui::line_edit_t host_edit, port_edit, command_edit;
  ui::text_edit_t notes{"Notes go here.\nType, select with the mouse or Shift and the arrows,\nand Ctrl+C, Ctrl+X, Ctrl+V."};
  ui::text_edit_t log;

  ui::node_t view() const
  {
    return ui::column_t{
      .gap = 14,
      .align = ui::align_t::stretch,
      .padding = 24,
      .children = {
        ui::label({.text = "vui: text", .size = 22}),
        ui::box_t{.color = panel,
                  .radius = 10,
                  .padding = 16,
                  .align_x = ui::align_t::start,
                  .children = {ui::column_t{.gap = 10,
                                            .children = {field("Host", ui::mount(host_edit, {.text = host, .placeholder = "name or address", .columns = 28})),
                                                         field("Port", ui::mount(port_edit, {.text = port, .columns = 6})),
                                                         field("Connects to", ui::label({.text = host + ":" + port, .monospace = true}))}}}},
        ui::row_t{.gap = 14,
                  .align = ui::align_t::stretch,
                  .grow = 1,
                  .children = {filling(ui::column_t{.gap = 6, .align = ui::align_t::stretch, .children = {ui::label({.text = "Notes", .muted = true}), filling(ui::mount(notes, {.columns = 30, .rows = 8}))}}),
                               filling(ui::column_t{.gap = 6,
                                                    .align = ui::align_t::stretch,
                                                    .children = {ui::label({.text = "Log", .muted = true}),
                                                                 filling(ui::mount(log, {.read_only = true, .columns = 40, .rows = 8, .follow = true, .max_lines = 5000})),
                                                                 ui::mount(command_edit, {.text = command, .placeholder = "type a command and press Enter"})}})}},
        ui::label({.text = "Tab moves between the fields. The log keeps its last 5000 lines,\nand follows the end while it is scrolled there.", .muted = true}),
      }};
  }

  ui::behaviour_t behave(ui::scope_t &scope)
  {
    auto edits = ui::subscribe<ui::line_edit_event_t>(host_edit.changed, port_edit.changed, command_edit.changed);
    auto submits = ui::subscribe<ui::line_edit_event_t>(command_edit.submitted, host_edit.submitted, port_edit.submitted);
    auto clock = ui::every(scope, 250ms);
    auto events = ui::merge(scope, edits, submits, clock);
    log.append_line(scope, "[log] started");
    while (auto event = co_await events)
    {
      if (event->index() == 0)
      {
        const auto &edit = std::get<0>(*event);
        if (edit.source == &host_edit)
          host = edit.text;
        else if (edit.source == &command_edit)
          command = edit.text;
        else if (edit.text.size() <= 5 && edit.text.find_first_not_of("0123456789") == std::string::npos)
          port = edit.text;
      }
      else if (event->index() == 1)
      {
        const auto &submit = std::get<1>(*event);
        if (submit.source == &command_edit)
        {
          log.append_line(scope, "> " + command);
          command.clear();
        }
        else
          log.append_line(scope, "[net] connecting to " + host + ":" + port);
      }
      else
      {
        char line[96];
        std::snprintf(line, sizeof line, "[tick %05d] %s:%s  notes: %zu lines", ++ticks, host.c_str(), port.c_str(), notes.line_count());
        log.append_line(scope, line);
      }
    }
  }
};
} // namespace

int main(int argc, char **argv)
{
  const auto limit = std::chrono::milliseconds(argc > 1 ? std::atoi(argv[1]) : 0);
  return vio::run(
    [limit](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto display = vui::display_t::open(loop);
      if (!display)
      {
        std::fprintf(stderr, "no display: %s\n", display.error().msg.c_str());
        co_return 1;
      }
      auto window = vui::window_t::create(*display, {.title = "vui: text", .size = {960, 720}});
      if (!window)
        co_return 1;
      auto closing = [](vui::window_t &window, std::chrono::milliseconds limit) -> vio::task_t<void>
      {
        auto requests = window.subscribe<vui::close_request_t>();
        if (limit.count() > 0)
          (void)co_await vio::sleep(*window.impl()->display.loop, limit);
        else
          co_await requests;
        window.close();
      }(*window, limit);
      co_await ui::run(*window, console_t{});
      co_await std::move(closing);
      co_return 0;
    });
}
