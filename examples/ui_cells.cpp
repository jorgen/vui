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

// A grid of styled cells drawn the way a terminal draws them (G74 to G78): one canvas, a prim per
// cell colour and per glyph, the monospace font's bold and italic faces, every underline style,
// glyphs from fallback fonts, a selection and a blinking cursor. The grid's paint is built once per
// scale and kept; the cursor is a canvas of its own, so blinking it converts nothing else. Keys and
// every mouse button arrive raw (G78) and are printed on the status line. A font file given after the
// time limit is used instead of the system's monospace font: one without bold and italic files
// beside it shows the synthetic faces.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <vio/event_loop.h>
#include <vio/operation/sleep.h>
#include <vio/run.h>
#include <vui/ui/ui.h>
#include <vui/vui.h>

namespace ui = vui::ui;
using namespace std::chrono_literals;

namespace
{
constexpr int columns = 88;
constexpr int rows = 20;
constexpr float font_size = 15;

constexpr ui::color_t background{0.07f, 0.08f, 0.10f, 1};
constexpr ui::color_t foreground{0.85f, 0.86f, 0.88f, 1};

// The sixteen colours of xterm.
constexpr ui::color_t ansi[16] = {{0, 0, 0, 1},          {0.80f, 0.19f, 0.19f, 1}, {0.05f, 0.74f, 0.47f, 1}, {0.90f, 0.90f, 0.06f, 1}, {0.14f, 0.45f, 0.78f, 1}, {0.74f, 0.25f, 0.74f, 1},
                                  {0.07f, 0.66f, 0.80f, 1}, {0.90f, 0.90f, 0.90f, 1}, {0.40f, 0.40f, 0.40f, 1}, {0.95f, 0.30f, 0.30f, 1}, {0.14f, 0.82f, 0.55f, 1}, {0.96f, 0.96f, 0.26f, 1},
                                  {0.23f, 0.56f, 0.92f, 1}, {0.84f, 0.44f, 0.84f, 1}, {0.16f, 0.72f, 0.86f, 1}, {1, 1, 1, 1}};

ui::color_t cube(int index)
{
  const auto level = [](int v) { return v == 0 ? 0.0f : (55.0f + 40.0f * float(v)) / 255.0f; };
  return {level(index / 36), level(index / 6 % 6), level(index % 6), 1};
}

// Two cells for the wide characters of East Asian scripts, one for everything else -- the rule a
// terminal takes from Unicode's East Asian Width; these ranges are enough for the sample.
bool wide(char32_t cp)
{
  return (cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf) || (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xff00 && cp <= 0xff60) || (cp >= 0x1f300 && cp <= 0x1faff);
}

std::u32string decode(std::string_view text)
{
  std::u32string out;
  for (std::size_t i = 0; i < text.size();)
  {
    const auto lead = static_cast<unsigned char>(text[i]);
    const int length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    char32_t cp = length == 1 ? lead : char32_t(lead & (0x7f >> length));
    for (int k = 1; k < length && i + std::size_t(k) < text.size(); ++k)
      cp = (cp << 6) | (static_cast<unsigned char>(text[i + std::size_t(k)]) & 0x3f);
    out.push_back(cp);
    i += std::size_t(length);
  }
  return out;
}

struct style_t
{
  ui::color_t fg = foreground;
  ui::color_t bg = {0, 0, 0, 0};
  ui::font_style_t face = ui::font_style_t::regular;
  bool underline = false;
  ui::line_style_t underline_style = ui::line_style_t::solid;
  ui::color_t underline_color = {0, 0, 0, 0};
  bool strike = false;
  bool overline = false;
};

struct cell_t
{
  char32_t cp = U' ';
  style_t style;
  bool spacer = false;
};

struct grid_t
{
  std::vector<cell_t> cells = std::vector<cell_t>(std::size_t(columns) * rows);

  int write(int column, int row, std::string_view text, const style_t &style = {})
  {
    for (const char32_t cp : decode(text))
    {
      if (column >= columns)
        break;
      cells[std::size_t(row) * columns + std::size_t(column)] = {cp, style};
      if (wide(cp) && column + 1 < columns)
        cells[std::size_t(row) * columns + std::size_t(++column)] = {U' ', style, true};
      ++column;
    }
    return column;
  }
};

grid_t sample()
{
  grid_t grid;
  style_t title{.fg = {1, 1, 1, 1}, .face = ui::font_style_t::bold};
  grid.write(1, 0, "vui cells", title);
  grid.write(11, 0, "-- one canvas: a prim per cell colour and per glyph, nothing shaped", {.fg = ansi[8]});

  int c = grid.write(1, 2, "Regular ", {});
  c = grid.write(c, 2, "Bold ", {.fg = {1, 1, 1, 1}, .face = ui::font_style_t::bold});
  c = grid.write(c, 2, "Italic ", {.fg = ansi[14], .face = ui::font_style_t::italic});
  c = grid.write(c, 2, "Bold italic ", {.fg = ansi[11], .face = ui::font_style_t::bold_italic});
  grid.write(c + 1, 2, "0O 1lI| {}[]() <= != -> ~", {});

  for (int i = 0; i < 16; ++i)
  {
    style_t swatch{.fg = i == 0 || i == 8 ? ui::color_t{1, 1, 1, 1} : ui::color_t{0, 0, 0, 1}, .bg = ansi[i]};
    char label[8];
    std::snprintf(label, sizeof label, " %2d  ", i);
    grid.write(1 + i * 5, 4, label, swatch);
  }
  for (int i = 0; i < 216; ++i)
    grid.write(1 + i % 72, 5 + i / 72, " ", {.bg = cube(i)});
  for (int i = 0; i < 24; ++i)
  {
    const float v = (8.0f + 10.0f * float(i)) / 255.0f;
    grid.write(1 + i * 3, 8, "   ", {.bg = {v, v, v, 1}});
  }

  c = grid.write(1, 10, "underline", {.underline = true});
  c = grid.write(c + 2, 10, "double", {.underline = true, .underline_style = ui::line_style_t::double_line});
  c = grid.write(c + 2, 10, "curly misspelt", {.underline = true, .underline_style = ui::line_style_t::curly, .underline_color = ansi[9]});
  c = grid.write(c + 2, 10, "dotted", {.underline = true, .underline_style = ui::line_style_t::dotted});
  c = grid.write(c + 2, 10, "dashed", {.underline = true, .underline_style = ui::line_style_t::dashed});
  c = grid.write(c + 2, 10, "struck out", {.fg = ansi[8], .strike = true});
  grid.write(c + 2, 10, "overline", {.overline = true});

  grid.write(1, 12, "Fallback:", {.fg = ansi[8]});
  c = grid.write(11, 12, "\u250c\u2500\u2500\u252c\u2500\u2500\u2510 ", {.fg = ansi[6]});
  c = grid.write(c, 12, "\u6f22\u5b57 \u304b\u306a \ud55c\uae00 ", {.fg = ansi[11]});
  c = grid.write(c, 12, "\u2603 \u2605 \u2713 \u2192 \u2211 \u03bb \u00df \u00e9 \u00f1 \u2588\u2593\u2592\u2591 ", {.fg = ansi[13]});
  grid.write(c, 12, "\U0001f600\U0001f680\U0001f308", {});
  grid.write(11, 13, "\u2514\u2500\u2500\u2534\u2500\u2500\u2518", {.fg = ansi[6]});

  c = grid.write(1, 15, "user@host", {.fg = ansi[10], .face = ui::font_style_t::bold});
  c = grid.write(c, 15, ":", {});
  c = grid.write(c, 15, "~/src/yat", {.fg = ansi[12], .face = ui::font_style_t::bold});
  c = grid.write(c, 15, "$ ls --color", {});
  c = grid.write(1, 16, "CMakeLists.txt  ", {});
  c = grid.write(c, 16, "app/  core/  tests/  ", {.fg = ansi[12], .face = ui::font_style_t::bold});
  c = grid.write(c, 16, "build.sh", {.fg = ansi[10], .face = ui::font_style_t::bold});
  c = grid.write(1, 17, "user@host", {.fg = ansi[10], .face = ui::font_style_t::bold});
  c = grid.write(c, 17, ":", {});
  c = grid.write(c, 17, "~/src/yat", {.fg = ansi[12], .face = ui::font_style_t::bold});
  grid.write(c, 17, "$ ", {});
  return grid;
}

// The grid as one paint: every background, then every glyph, then every decoration, so a glyph's
// descender is never covered by the next row's background.
std::shared_ptr<const ui::paint_t> paint(const grid_t &grid, const ui::font_t &font, const ui::font_t::cell_metrics_t &cell)
{
  auto paint = std::make_shared<ui::paint_t>();
  paint->reserve(grid.cells.size() * 2);
  std::uint16_t faces[4];
  for (int i = 0; i < 4; ++i)
    faces[i] = paint->font(font.styled(ui::font_style_t(i)), font_size);
  const auto at = [&](int column, int row) { return std::pair{float(column) * cell.width, float(row) * cell.height}; };
  for (int row = 0; row < rows; ++row)
  {
    for (int column = 0; column < columns; ++column)
    {
      const auto &c = grid.cells[std::size_t(row) * columns + std::size_t(column)];
      if (c.style.bg.a > 0)
      {
        const auto [x, y] = at(column, row);
        paint->rect(x, y, cell.width, cell.height, c.style.bg);
      }
    }
  }
  for (int row = 0; row < rows; ++row)
  {
    for (int column = 0; column < columns; ++column)
    {
      const auto &c = grid.cells[std::size_t(row) * columns + std::size_t(column)];
      if (c.spacer || c.cp == U' ')
        continue;
      const auto [x, y] = at(column, row);
      paint->glyph(faces[int(c.style.face)], x, y + cell.baseline, c.cp, c.style.fg);
    }
  }
  for (int row = 0; row < rows; ++row)
  {
    for (int column = 0; column < columns; ++column)
    {
      const auto &c = grid.cells[std::size_t(row) * columns + std::size_t(column)];
      const auto [x, y] = at(column, row);
      if (c.style.underline)
        paint->line(c.style.underline_style, x, y + cell.underline, cell.width, cell.underline_thickness, c.style.underline_color.a > 0 ? c.style.underline_color : c.style.fg);
      if (c.style.strike)
        paint->line(ui::line_style_t::solid, x, y + cell.strikeout, cell.width, cell.strikeout_thickness, c.style.fg);
      if (c.style.overline)
        paint->line(ui::line_style_t::solid, x, y, cell.width, cell.underline_thickness, c.style.fg);
    }
  }
  // A selection over part of the listing, translucent so the cells show through.
  const auto [sx, sy] = at(17, 16);
  paint->rect(sx, sy, cell.width * 21, cell.height, {0.3f, 0.5f, 1.0f, 0.35f});
  return paint;
}

struct terminal_t
{
  ui::font_t font;
  grid_t grid = sample();
  ui::font_t::cell_metrics_t cell;
  std::shared_ptr<const ui::paint_t> cells;
  bool cursor_on = true;
  std::string status = "Press keys or any mouse button over the grid; Tab stays here.";
  ui::pressable_t input{ui::raw_input};

  ui::node_t view() const
  {
    auto cursor = std::make_shared<ui::paint_t>();
    if (cursor_on)
      cursor->rect(cell.width * 21, cell.height * 17, cell.width, cell.height, {0.85f, 0.86f, 0.88f, 0.85f});
    const vui::extent_t size{int(std::ceil(cell.width * columns)), int(std::ceil(cell.height * rows))};
    return ui::column_t{.gap = 8,
                        .padding = 12,
                        .children = {
                          ui::box_t{.color = background,
                                    .border_width = input.focused() ? 1.0f : 0.0f,
                                    .border_color = {0.3f, 0.5f, 1.0f, 1},
                                    .events = input,
                                    .cursor = vui::cursor_t::text,
                                    .padding = 6,
                                    .children = {ui::box_t{.color = {0, 0, 0, 0}, .children = {ui::canvas_t{size, cells}, ui::canvas_t{size, cursor}}}}},
                          ui::text_t{.text = status, .size = 13, .color = {0.6f, 0.62f, 0.66f, 1}},
                        }};
  }

  void repaint(float scale)
  {
    cell = font.cell_metrics(font_size, scale);
    cells = paint(grid, font, cell);
  }

  ui::behaviour_t behave(ui::scope_t &scope)
  {
    repaint(scope.scale());
    ui::request_focus(scope, input);
    auto blink = ui::every(scope, 530ms);
    auto keys = ui::subscribe<ui::typing_event_t>(input);
    auto mice = ui::subscribe<ui::mouse_event_t>(input);
    auto wheels = ui::subscribe<ui::wheel_event_t>(input);
    auto events = ui::merge(scope, blink, keys, mice, wheels);
    float scale = scope.scale();
    while (auto event = co_await events)
    {
      // A new scale: the cell is a new number of pixels, so the paint is built again.
      if (scope.scale() != scale)
      {
        scale = scope.scale();
        repaint(scale);
      }
      char line[160];
      switch (event->index())
      {
      case 0:
        cursor_on = !cursor_on;
        continue;
      case 1:
      {
        const auto &typed = std::get<1>(*event).input;
        if (const auto *key = std::get_if<vui::key_event_t>(&typed))
        {
          if (!key->pressed)
            continue;
          std::snprintf(line, sizeof line, "key %d%s, modifiers %d", int(key->key), key->repeat ? " (repeat)" : "", int(key->modifiers));
        }
        else
          std::snprintf(line, sizeof line, "text \"%s\"", std::get<vui::text_event_t>(typed).text.c_str());
        break;
      }
      case 2:
      {
        const auto &mouse = std::get<2>(*event);
        const char *phase = mouse.phase == ui::mouse_phase_t::press ? "press" : mouse.phase == ui::mouse_phase_t::release ? "release" : "move";
        const auto bounds = input.bounds();
        std::snprintf(line, sizeof line, "mouse %s, button %d, cell %d,%d, modifiers %d", phase, int(mouse.button), int((mouse.position.x - bounds.x - 6) / cell.width), int((mouse.position.y - bounds.y - 6) / cell.height),
                      int(mouse.modifiers));
        break;
      }
      default:
      {
        const auto &wheel = std::get<3>(*event);
        std::snprintf(line, sizeof line, "wheel %.1f, modifiers %d", wheel.dy, int(wheel.modifiers));
        break;
      }
      }
      status = line;
    }
  }
};
} // namespace

int main(int argc, char **argv)
{
  const auto limit = std::chrono::milliseconds(argc > 1 ? std::atoi(argv[1]) : 0);
  const std::string font_file = argc > 2 ? argv[2] : "";
  return vio::run(
    [limit, font_file](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto font = font_file.empty() ? ui::font_t::system_monospace() : ui::font_t::from_file(font_file);
      if (!font)
      {
        std::fprintf(stderr, "no font: %s\n", font.error().c_str());
        co_return 1;
      }
      auto display = vui::display_t::open(loop);
      if (!display)
      {
        std::fprintf(stderr, "no display: %s\n", display.error().msg.c_str());
        co_return 1;
      }
      const auto cell = font->cell_metrics(font_size);
      const float width = cell.width * columns + 40;
      const float height = cell.height * rows + 70;
      auto window = vui::window_t::create(*display, {.title = "vui: cells", .size = {int(width), int(height)}});
      if (!window)
        co_return 1;
      // Sized in layout units: the window system's units are pixels on Windows and X11 (G9, G35).
      if (const auto framebuffer = window->framebuffer_size(); framebuffer.width > 0)
      {
        const float units = float(window->size().width) * window->scale() / float(framebuffer.width);
        (void)window->set_size({int(width * units), int(height * units)});
      }
      auto closing = [](vui::window_t &window, std::chrono::milliseconds limit) -> vio::task_t<void>
      {
        auto requests = window.subscribe<vui::close_request_t>();
        if (limit.count() > 0)
          (void)co_await vio::sleep(*window.impl()->display.loop, limit);
        else
          co_await requests;
        window.close();
      }(*window, limit);
      co_await ui::run(*window, terminal_t{*font});
      co_await std::move(closing);
      co_return 0;
    });
}
