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

// The canvas node and what a terminal draws with it: pixel-exact cells, glyphs by code point, the
// cache of converted paints, decorations, font faces and fallback (G74 to G77). The fonts are
// HarfBuzz's test fonts, so these run only where HarfBuzz was fetched rather than found on the
// system.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include <vio/event_loop.h>
#include <vio/operation/sleep.h>
#include <vio/task.h>
#include <vui/ui/ui.h>
#include <vui/vui.h>

#include "gui_test_helpers.h"
#include "ui_renderer.h"
#include "ui_text.h"

#if defined(VUI_TEST_FONTS)

using namespace std::chrono_literals;
namespace ui = vui::ui;

namespace
{
std::string font_path(const char *relative)
{
  return std::string(VUI_TEST_FONTS) + "/" + relative;
}

ui::font_t load(const char *relative)
{
  auto font = ui::font_t::from_file(font_path(relative));
  REQUIRE_MESSAGE(font.has_value(), font.error());
  return *font;
}

ui::font_t roboto()
{
  return load("perf/fonts/Roboto-Regular.ttf");
}

std::vector<ui::prim_t> of(const std::vector<ui::prim_t> &prims, ui::prim_kind_t kind)
{
  std::vector<ui::prim_t> out;
  for (const auto &prim : prims)
  {
    if (prim.kind == kind)
      out.push_back(prim);
  }
  return out;
}

// Coverage summed under a glyph's atlas rectangle.
long ink(const ui::detail::text_engine_t &engine, const ui::prim_t &prim)
{
  const auto atlas = engine.atlas();
  long sum = 0;
  for (int y = 0; y < int(prim.height); ++y)
  {
    for (int x = 0; x < int(prim.width); ++x)
      sum += atlas.pixels[std::size_t(int(prim.radii[1]) + y) * std::size_t(atlas.extent.width) + std::size_t(int(prim.radii[0]) + x)];
  }
  return sum;
}

// A scratch folder of font files under chosen names, removed again.
struct font_folder_t
{
  std::filesystem::path dir;
  font_folder_t()
  {
    dir = std::filesystem::temp_directory_path() / ("vui_faces_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
  }
  ~font_folder_t()
  {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  std::string add(const char *from, const char *name) const
  {
    std::filesystem::copy_file(font_path(from), dir / name);
    return (dir / name).string();
  }
};
} // namespace

TEST_CASE("ui canvas: rectangles are whole pixels from one anchor, so neighbours meet exactly")
{
  ui::detail::text_engine_t engine;
  engine.set_scale(1.5f);
  auto paint = std::make_shared<ui::paint_t>();
  // Cells 7.3 units wide: 10.95 pixels, so their edges fall at fractions a shape would blend.
  for (int column = 0; column < 10; ++column)
    paint->rect(float(column) * 7.3f, 0, 7.3f, 15, {float(column) / 10, 0, 0, 1});
  // Thinner than a pixel, still a pixel.
  paint->rect(0, 20, 0.2f, 0.2f, {1, 1, 1, 1});
  std::vector<ui::prim_t> prims;
  engine.add_canvas({{80, 30}, paint}, 3.25f, 4.5f, prims);
  REQUIRE(prims.size() == 11);
  for (std::size_t i = 0; i < prims.size(); ++i)
  {
    const auto &prim = prims[i];
    CHECK(prim.kind == ui::prim_kind_t::pixels);
    // The anchor is where the canvas is; the shader rounds it, and everything else is whole pixels.
    CHECK(prim.x == 3.25f);
    CHECK(prim.y == 4.5f);
    CHECK(prim.radii[2] == std::floor(prim.radii[2]));
    CHECK(prim.width == std::floor(prim.width));
    if (i > 0 && i < 10)
      CHECK(prims[i].radii[2] == prims[i - 1].radii[2] + prims[i - 1].width);
  }
  CHECK(prims[9].radii[2] + prims[9].width == std::lround(73 * 1.5f));
  CHECK(prims[0].height == std::lround(15 * 1.5f));
  CHECK(prims[10].width == 1);
  CHECK(prims[10].height == 1);
}

TEST_CASE("ui canvas: a glyph is the font's own for its code point, on a whole-pixel baseline")
{
  ui::detail::text_engine_t engine;
  const auto font = roboto();
  auto paint = std::make_shared<ui::paint_t>();
  const auto face = paint->font(font, 20);
  CHECK(paint->font(font, 20) == face);
  CHECK(paint->font(font, 21) != face);
  // A space draws nothing; three letters a cell of 12 apart.
  paint->text(face, 0, 16, "A bC", 12, {1, 1, 1, 1});
  std::vector<ui::prim_t> prims;
  engine.add_canvas({{60, 20}, paint}, 0, 0, prims);
  REQUIRE(prims.size() == 3);
  for (const auto &prim : prims)
    CHECK(prim.kind == ui::prim_kind_t::coverage);

  // The same glyphs as the font's cmap gives, rasterized into the same atlas text uses: a text_t of
  // "A" at the same size lands on the same atlas rectangle.
  std::vector<ui::prim_t> text;
  engine.add_text({.text = "A", .size = 20, .font = font}, {}, 0, 0, text);
  REQUIRE(text.size() == 1);
  CHECK(text[0].radii[0] == prims[0].radii[0]);
  CHECK(text[0].radii[1] == prims[0].radii[1]);
  // A cell of 12 at scale 1 puts "b" 24 pixels after "A", give or take the glyphs' bearings.
  CHECK(std::abs((prims[1].radii[2] - prims[0].radii[2]) - 24) < 4);
}

TEST_CASE("ui canvas: the same paint again is a copy of what it became, wherever it is placed")
{
  ui::detail::text_engine_t engine;
  const auto font = roboto();
  auto paint = std::make_shared<ui::paint_t>();
  const auto face = paint->font(font, 14);
  paint->rect(0, 0, 10, 10, {0, 0, 1, 1});
  paint->text(face, 0, 12, "cached", 8, {1, 1, 1, 1});
  std::vector<ui::prim_t> first;
  engine.begin_layout();
  engine.add_canvas({{60, 20}, paint}, 0, 0, first);
  REQUIRE(first.size() == 7);

  // Behind the cache's back the paint is emptied: what comes out is still what it was, moved.
  const_cast<ui::paint_t &>(*paint).clear();
  std::vector<ui::prim_t> again;
  engine.begin_layout();
  engine.add_canvas({{60, 20}, paint}, 5, 7, again);
  REQUIRE(again.size() == first.size());
  CHECK(again[0].x == first[0].x + 5);
  CHECK(again[3].radii[0] == first[3].radii[0]);

  // Two layouts without it and it is let go, so the next sees the paint as it is now.
  engine.begin_layout();
  engine.begin_layout();
  engine.begin_layout();
  std::vector<ui::prim_t> later;
  engine.add_canvas({{60, 20}, paint}, 0, 0, later);
  CHECK(later.empty());
}

TEST_CASE("ui canvas: a new scale converts the paint again")
{
  ui::detail::text_engine_t engine;
  auto paint = std::make_shared<ui::paint_t>();
  paint->rect(0, 0, 10, 10, {0, 0, 1, 1});
  std::vector<ui::prim_t> one;
  engine.add_canvas({{10, 10}, paint}, 0, 0, one);
  engine.set_scale(2);
  std::vector<ui::prim_t> two;
  engine.add_canvas({{10, 10}, paint}, 0, 0, two);
  REQUIRE(one.size() == 1);
  REQUIRE(two.size() == 1);
  CHECK(one[0].width == 10);
  CHECK(two[0].width == 20);
}

TEST_CASE("ui canvas: decorations: solid, double, dotted, dashed and curly, continuous across cells")
{
  ui::detail::text_engine_t engine;
  const auto line = [&](ui::line_style_t style, float x, float width)
  {
    auto paint = std::make_shared<ui::paint_t>();
    paint->line(style, x, 10, width, 1, {1, 1, 1, 1});
    std::vector<ui::prim_t> prims;
    engine.add_canvas({{100, 20}, paint}, 0, 0, prims);
    return prims;
  };
  const auto solid = line(ui::line_style_t::solid, 0, 40);
  REQUIRE(solid.size() == 1);
  CHECK(solid[0].width == 40);
  CHECK(solid[0].height == 1);
  CHECK(solid[0].radii[3] == 10);

  const auto twice = line(ui::line_style_t::double_line, 0, 40);
  REQUIRE(twice.size() == 2);
  CHECK(twice[1].radii[3] - twice[0].radii[3] == 2);

  // Dots of one pixel every two; drawn a cell of 7 at a time, the dots are where one long line
  // puts them.
  const auto dots = line(ui::line_style_t::dotted, 0, 42);
  CHECK(dots.size() == 21);
  std::vector<float> cells;
  for (int cell = 0; cell < 6; ++cell)
  {
    for (const auto &prim : line(ui::line_style_t::dotted, float(cell) * 7, 7))
      cells.push_back(prim.radii[2]);
  }
  std::vector<float> whole;
  for (const auto &prim : dots)
    whole.push_back(prim.radii[2]);
  CHECK(cells == whole);

  const auto dashes = line(ui::line_style_t::dashed, 0, 60);
  REQUIRE(dashes.size() >= 5);
  CHECK(dashes[0].width == 4);
  CHECK(dashes[1].radii[2] - dashes[0].radii[2] == 6);

  // A wave is mitred segments, between the line's first and third pixel rows.
  const auto wave = line(ui::line_style_t::curly, 0, 30);
  REQUIRE(wave.size() >= 9);
  for (const auto &prim : wave)
  {
    CHECK(prim.kind == ui::prim_kind_t::segment);
    CHECK(prim.y >= 10);
    CHECK(prim.y <= 13);
  }
  // Two halves end and start at the same point, so a wave drawn by the cell is one wave.
  const auto left = line(ui::line_style_t::curly, 0, 13);
  const auto right = line(ui::line_style_t::curly, 13, 17);
  CHECK(left.back().radii[0] == doctest::Approx(right.front().x));
  CHECK(left.back().radii[1] == doctest::Approx(right.front().y));
}

TEST_CASE("ui canvas: cell metrics are whole pixels at the scale asked for")
{
  const auto font = roboto();
  for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f})
  {
    const auto cell = font.cell_metrics(14, scale);
    for (const float measure : {cell.width, cell.height, cell.baseline, cell.underline, cell.underline_thickness, cell.strikeout, cell.strikeout_thickness})
      CHECK(measure * scale == doctest::Approx(std::round(measure * scale)));
    CHECK(cell.width > 0);
    CHECK(cell.baseline < cell.height);
    CHECK(cell.underline > cell.baseline);
    CHECK(cell.underline + cell.underline_thickness <= cell.height + 1e-3f);
    CHECK(cell.strikeout < cell.baseline);
    CHECK(cell.underline_thickness * scale >= 1 - 1e-3f);
  }
  const auto metrics = font.metrics(14);
  CHECK(metrics.underline_offset > 0);
  CHECK(metrics.underline_thickness > 0);
  CHECK(metrics.strikeout_offset > 0);
}

TEST_CASE("ui canvas: a grid of 12,000 cells becomes one prim each for its colour and its glyph")
{
  ui::detail::text_engine_t engine;
  const auto font = roboto();
  auto paint = std::make_shared<ui::paint_t>();
  const auto face = paint->font(font, 14);
  constexpr int columns = 200;
  constexpr int rows = 60;
  paint->reserve(std::size_t(columns) * rows * 2);
  for (int row = 0; row < rows; ++row)
  {
    for (int column = 0; column < columns; ++column)
    {
      paint->rect(float(column) * 8, float(row) * 16, 8, 16, {float(column % 7) / 7, float(row % 5) / 5, 0.5f, 1});
      paint->glyph(face, float(column) * 8, float(row) * 16 + 12, char32_t('!' + (row * columns + column) % 90), {1, 1, 1, 1});
    }
  }
  std::vector<ui::prim_t> prims;
  const auto started = std::chrono::steady_clock::now();
  engine.begin_layout();
  engine.add_canvas({{columns * 8, rows * 16}, paint}, 0, 0, prims);
  const auto converted = std::chrono::steady_clock::now();
  std::vector<ui::prim_t> again;
  engine.begin_layout();
  engine.add_canvas({{columns * 8, rows * 16}, paint}, 0, 0, again);
  const auto copied = std::chrono::steady_clock::now();
  CHECK(prims.size() == std::size_t(columns) * rows * 2);
  CHECK(again.size() == prims.size());
  MESSAGE("24,000 prims: converted in " << std::chrono::duration<double, std::milli>(converted - started).count() << " ms, then copied from the cache in "
                                        << std::chrono::duration<double, std::milli>(copied - converted).count() << " ms");
}

TEST_CASE("ui faces: a family's bold is found beside it by name and checked by what it says it is")
{
  font_folder_t folder;
  const auto regular_path = folder.add("perf/fonts/Roboto-Regular.ttf", "Family-Regular.ttf");
  folder.add("test/api/fonts/NotoSans-Bold.ttf", "Family-Bold.ttf");
  auto regular = ui::font_t::from_file(regular_path);
  REQUIRE(regular.has_value());
  CHECK(regular->style() == ui::font_style_t::regular);

  const auto bold = regular->styled(ui::font_style_t::bold);
  CHECK(bold.style() == ui::font_style_t::bold);
  CHECK_FALSE(bold.synthetic());
  CHECK(bold != *regular);
  // Asked again, the same face: found once and kept.
  CHECK(regular->styled(ui::font_style_t::bold) == bold);
  // And from the bold, back to the family's regular.
  CHECK(bold.styled(ui::font_style_t::regular) == *regular);

  // There is no italic: the regular face, slanted when it is drawn.
  const auto italic = regular->styled(ui::font_style_t::italic);
  CHECK(italic.style() == ui::font_style_t::italic);
  CHECK(italic.synthetic());
  // A bold italic is made from the real bold rather than from the regular.
  const auto bold_italic = regular->styled(ui::font_style_t::bold_italic);
  CHECK(bold_italic.synthetic());
  CHECK(bold_italic.data() != italic.data());
}

TEST_CASE("ui faces: Windows' short names, and a file named bold that is not refused")
{
  font_folder_t folder;
  const auto short_path = folder.add("perf/fonts/Roboto-Regular.ttf", "fam.ttf");
  folder.add("test/api/fonts/NotoSans-Bold.ttf", "famb.ttf");
  folder.add("test/api/fonts/notosansitalic.ttf", "fami.ttf");
  auto font = ui::font_t::from_file(short_path);
  REQUIRE(font.has_value());
  CHECK_FALSE(font->styled(ui::font_style_t::bold).synthetic());
  CHECK_FALSE(font->styled(ui::font_style_t::italic).synthetic());
  CHECK(font->styled(ui::font_style_t::italic).style() == ui::font_style_t::italic);

  // A regular face under a bold name is not taken for the bold.
  const auto liar_path = folder.add("perf/fonts/Roboto-Regular.ttf", "Liar.ttf");
  folder.add("perf/fonts/Roboto-Regular.ttf", "Liar-Bold.ttf");
  auto liar = ui::font_t::from_file(liar_path);
  REQUIRE(liar.has_value());
  CHECK(liar->styled(ui::font_style_t::bold).synthetic());
}

TEST_CASE("ui faces: synthetic bold inks more and synthetic italic leans, in text and on a canvas")
{
  ui::detail::text_engine_t engine;
  const auto regular = roboto();
  const auto bold = regular.styled(ui::font_style_t::bold);
  const auto italic = regular.styled(ui::font_style_t::italic);
  REQUIRE(bold.synthetic());
  REQUIRE(italic.synthetic());
  auto paint = std::make_shared<ui::paint_t>();
  paint->glyph(paint->font(regular, 24), 0, 20, U'l', {1, 1, 1, 1});
  paint->glyph(paint->font(bold, 24), 20, 20, U'l', {1, 1, 1, 1});
  paint->glyph(paint->font(italic, 24), 40, 20, U'l', {1, 1, 1, 1});
  std::vector<ui::prim_t> prims;
  engine.add_canvas({{60, 30}, paint}, 0, 0, prims);
  REQUIRE(prims.size() == 3);
  CHECK(ink(engine, prims[1]) > ink(engine, prims[0]) * 11 / 10);
  CHECK(prims[1].height == prims[0].height);
  // An upright "l" is a narrow bar; slanted, its box is wider by the lean over its height.
  CHECK(prims[2].width > prims[0].width + 2);

  // A text_t's style draws the same faces.
  std::vector<ui::prim_t> text;
  engine.add_text({.text = "l", .size = 24, .font = regular, .style = ui::font_style_t::bold}, {}, 0, 0, text);
  REQUIRE(text.size() == 1);
  CHECK(text[0].radii[0] == prims[1].radii[0]);
}

TEST_CASE("ui fallback: a code point the font lacks comes from a system font, in the same style")
{
  ui::detail::text_engine_t engine;
  // Roboto has no CJK, no box drawing and no snowman.
  const auto font = roboto();
  REQUIRE_FALSE(font.has_glyph(U'☃'));
  REQUIRE(font.has_glyph(U'A'));
  const auto own = engine.resolve(font, U'A');
  CHECK(*own.font == font);
  const auto snowman = engine.resolve(font, U'☃');
  if (*snowman.font == font)
  {
    MESSAGE("no system font here has U+2603; fallback not checked");
    return;
  }
  CHECK(snowman.glyph != 0);
  CHECK(snowman.font->has_glyph(U'☃'));
  // Remembered: the same answer, the same pointer.
  CHECK(engine.resolve(font, U'☃').font == snowman.font);
  // A bold cell's fallback is bold too.
  const auto bold = engine.resolve(font.styled(ui::font_style_t::bold), U'☃');
  CHECK(bold.font->style() == ui::font_style_t::bold);

  // On a canvas, and in a grid text_t, the glyph is drawn rather than the font's empty box.
  auto paint = std::make_shared<ui::paint_t>();
  paint->glyph(paint->font(font, 20), 0, 16, U'☃', {1, 1, 1, 1});
  std::vector<ui::prim_t> prims;
  engine.add_canvas({{20, 20}, paint}, 0, 0, prims);
  REQUIRE(prims.size() == 1);
  std::vector<ui::prim_t> grid;
  engine.add_text({.text = "☃", .size = 20, .font = font, .advance = 12}, {}, 0, 0, grid);
  REQUIRE(grid.size() == 1);
  CHECK(grid[0].radii[0] == prims[0].radii[0]);
}

namespace
{
// A terminal-like component: a paint it keeps, rebuilt only when its text changes, and a cursor drawn
// separately so that blinking it costs the grid nothing.
struct cells_t
{
  ui::font_t font;
  std::shared_ptr<const ui::paint_t> grid;
  bool cursor = true;
  ui::pressable_t blink;

  void paint(const std::string &text)
  {
    auto paint = std::make_shared<ui::paint_t>();
    const auto face = paint->font(font, 14);
    for (int column = 0; column < int(text.size()); ++column)
      paint->rect(float(column) * 8, 0, 8, 16, {0.1f * float(column), 0.2f, 0.3f, 1});
    paint->text(face, 0, 12, text, 8, {1, 1, 1, 1});
    grid = std::move(paint);
  }

  ui::node_t view() const
  {
    auto caret = std::make_shared<ui::paint_t>();
    if (cursor)
      caret->rect(0, 0, 2, 16, {1, 1, 0, 1});
    return ui::column_t{.children = {
                          ui::box_t{.children = {ui::canvas_t{{80, 16}, grid}, ui::canvas_t{{80, 16}, caret}}},
                          ui::box_t{.size = {10, 10}, .color = {1, 0, 0, 1}, .events = blink},
                        }};
  }

  ui::behaviour_t behave(ui::scope_t &)
  {
    auto presses = ui::subscribe<ui::press_event_t>(blink);
    while (co_await presses)
      cursor = !cursor;
  }
};
} // namespace

TEST_CASE("ui canvas: a component draws its paint, and a cursor blinking beside it costs the grid nothing")
{
  vio::event_loop_t loop;
  std::vector<std::vector<ui::prim_t>> frames;
  run_gui_test(loop,
               [](vio::event_loop_t &loop, std::vector<std::vector<ui::prim_t>> &frames) -> vio::task_t<void>
               {
                 vui::display_config_t display_config;
                 display_config.backend = "null";
                 display_config.frame_interval_ns = std::chrono::nanoseconds(2ms).count();
                 auto display = vui::display_t::open(loop, display_config);
                 REQUIRE(display.has_value());
                 {
                   auto window = vui::window_t::create(*display, {.size = {200, 100}});
                   cells_t cells{roboto()};
                   cells.paint("hello");
                   ui::run_config_t config;
                   config.on_display_list = [&frames](const ui::display_list_t &list) { frames.emplace_back(list.prims.begin(), list.prims.end()); };
                   auto running = ui::run(*window, cells, std::move(config));
                   (void)co_await vio::sleep(loop, 40ms);
                   const auto grid = cells.grid;
                   window->impl()->on_input(vui::pointer_move_event_t{{5, 21}});
                   (void)co_await vio::sleep(loop, 10ms);
                   window->impl()->on_input(vui::pointer_button_event_t{vui::button_t::left, true, {5, 21}});
                   window->impl()->on_input(vui::pointer_button_event_t{vui::button_t::left, false, {5, 21}});
                   (void)co_await vio::sleep(loop, 40ms);
                   CHECK(cells.grid == grid);
                   CHECK_FALSE(cells.cursor);
                   window->close();
                   co_await std::move(running);
                 }
                 loop.stop();
               }(loop, frames));
  REQUIRE(frames.size() >= 2);
  // Five cells, four glyphs ("l" twice is two prims), a cursor and the button's box.
  const auto &first = frames.front();
  CHECK(of(first, ui::prim_kind_t::pixels).size() == 6);
  CHECK(of(first, ui::prim_kind_t::coverage).size() == 5);
  const auto &last = frames.back();
  CHECK(of(last, ui::prim_kind_t::pixels).size() == 5);
  CHECK(of(last, ui::prim_kind_t::coverage).size() == 5);
}

TEST_CASE("vulkan: cells at 1.5x on a fractional origin cover every pixel once, with no seam")
{
  // Two hundred cells of alternating colour, 7.3 units wide, at an origin a third of a unit in.
  auto paint = std::make_shared<ui::paint_t>();
  for (int column = 0; column < 30; ++column)
    paint->rect(float(column) * 7.3f, 0, 7.3f, 10, column % 2 ? ui::color_t{1, 0, 0, 1} : ui::color_t{0, 0, 1, 1});
  ui::detail::text_engine_t engine;
  engine.set_scale(1.5f);
  std::vector<ui::prim_t> prims;
  engine.add_canvas({{220, 10}, paint}, 0, 0, prims);
  ui::run_t run{0, std::uint32_t(prims.size()), 1.0f / 3.0f, 2.2f, 1};
  ui::display_list_t list;
  list.prims = prims;
  list.runs = std::span<const ui::run_t>(&run, 1);
  list.background = {0, 1, 0, 1};
  list.logical = {240, 20};
  list.framebuffer = {360, 30};
  list.scale = 1.5f;
  list.atlas = engine.atlas();
  std::string why;
  auto image = ui::detail::render_offscreen_vulkan(list, why);
  if (!image)
  {
    MESSAGE("no Vulkan here, skipped: " << why);
    return;
  }
  // Along a row through the cells: every pixel is pure red or pure blue, never a blend and never the
  // green background, from the first cell's left edge to the last one's right.
  const int row = 8;
  const int left = int(std::floor(1.0f / 3.0f * 1.5f + 0.5f));
  const int right = left + int(std::lround(30 * 7.3f * 1.5f));
  int blends = 0;
  int changes = 0;
  for (int x = left; x < right; ++x)
  {
    const auto *p = &image->rgba[(std::size_t(row) * image->extent.width + std::size_t(x)) * 4];
    const bool red = p[0] == 255 && p[1] == 0 && p[2] == 0;
    const bool blue = p[0] == 0 && p[1] == 0 && p[2] == 255;
    if (!red && !blue)
      ++blends;
    if (x > left && p[0] != p[-4])
      ++changes;
  }
  CHECK(blends == 0);
  CHECK(changes == 29);
  // Outside the cells, the background.
  CHECK(image->rgba[(std::size_t(row) * image->extent.width + std::size_t(right)) * 4 + 1] == 255);
}

#endif
