// A task manager: the advanced example. A sampling thread reads the system once a second through
// system.h and hands each snapshot to the loop through a vio::event_pipe_t, so the UI never waits on
// the system. On top of it:
//
// - Performance: cards that pick a view, a CPU chart with a crosshair readout, one chart per core,
//   a heatmap of every core over two minutes, and memory.
// - Processes: a table of every process -- sorted by any column, filtered as you type, scrolled by
//   wheel, scrollbar or keyboard -- a panel for the selected one with its own CPU history, and a log
//   of processes starting and exiting.
// - A footer with what the toolkit itself spends per frame.
//
// `vui_task_manager --probe` takes two snapshots without opening a window and checks them, which is
// how CI exercises the system layer on each platform.

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <vio/event_loop.h>
#include <vio/operation/sleep.h>
#include <vio/run.h>
#include <vui/ui/ui.h>
#include <vui/ui/widgets.h>
#include <vui/vui.h>

#include "system.h"

namespace ui = vui::ui;
using namespace std::chrono_literals;

namespace
{
// ---- look

constexpr ui::color_t clear{0, 0, 0, 0};
constexpr ui::color_t panel{0.155f, 0.155f, 0.18f, 1};
constexpr ui::color_t panel_raised{0.19f, 0.19f, 0.225f, 1};
constexpr ui::color_t chart_back{0.11f, 0.11f, 0.13f, 1};
constexpr ui::color_t rule{1, 1, 1, 0.07f};
constexpr ui::color_t cpu_color{0.3f, 0.74f, 1.0f, 1};
constexpr ui::color_t memory_color{0.72f, 0.52f, 1.0f, 1};
constexpr ui::color_t swap_color{1.0f, 0.62f, 0.3f, 1};
constexpr ui::color_t sampler_color{0.4f, 0.85f, 0.55f, 1};

constexpr std::size_t history = 60;
constexpr int heat_columns = 120;

ui::color_t faded(ui::color_t c, float alpha)
{
  c.a = alpha;
  return c;
}

ui::node_t text(std::string s, float size, ui::color_t color, bool mono = false)
{
  return ui::label({.text = std::move(s), .monospace = mono, .size = size, .color = color});
}

ui::node_t muted(std::string s, float size = 13)
{
  return text(std::move(s), size, ui::theme().text_muted);
}

std::string fixed(double v, int decimals)
{
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%.*f", decimals, v);
  return buffer;
}

std::string bytes(std::uint64_t n)
{
  const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  double v = double(n);
  int unit = 0;
  while (v >= 1024 && unit < 4)
  {
    v /= 1024;
    ++unit;
  }
  return fixed(v, unit == 0 ? 0 : (v < 10 ? 2 : 1)) + " " + units[unit];
}

std::string grouped(std::uint64_t n)
{
  auto s = std::to_string(n);
  for (int i = int(s.size()) - 3; i > 0; i -= 3)
    s.insert(std::size_t(i), ",");
  return s;
}

std::string duration(std::chrono::seconds t)
{
  const auto s = static_cast<long long>(t.count());
  char buffer[48];
  if (s >= 86400)
    std::snprintf(buffer, sizeof buffer, "%lldd %02lld:%02lld:%02lld", s / 86400, s / 3600 % 24, s / 60 % 60, s % 60);
  else
    std::snprintf(buffer, sizeof buffer, "%02lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
  return buffer;
}

std::string ms(std::chrono::nanoseconds t)
{
  return fixed(double(t.count()) / 1e6, 2) + " ms";
}

std::shared_ptr<const ui::path_t> chevron(bool up)
{
  auto make = [](bool up_)
  {
    auto p = std::make_shared<ui::path_t>();
    if (up_)
      p->move_to(0.14f, 0.64f).line_to(0.5f, 0.28f).line_to(0.86f, 0.64f).line_to(0.75f, 0.75f).line_to(0.5f, 0.5f).line_to(0.25f, 0.75f).close();
    else
      p->move_to(0.14f, 0.36f).line_to(0.25f, 0.25f).line_to(0.5f, 0.5f).line_to(0.75f, 0.25f).line_to(0.86f, 0.36f).line_to(0.5f, 0.72f).close();
    return std::shared_ptr<const ui::path_t>(std::move(p));
  };
  static const auto up_path = make(true);
  static const auto down_path = make(false);
  return up ? up_path : down_path;
}

ui::color_t state_color(sys::process_state_t state)
{
  switch (state)
  {
  case sys::process_state_t::running:
    return {0.35f, 0.82f, 0.5f, 1};
  case sys::process_state_t::disk_wait:
    return {0.35f, 0.65f, 1.0f, 1};
  case sys::process_state_t::stopped:
  case sys::process_state_t::suspended:
    return {1.0f, 0.72f, 0.3f, 1};
  case sys::process_state_t::zombie:
    return {1.0f, 0.4f, 0.4f, 1};
  default:
    return {0.55f, 0.55f, 0.6f, 1};
  }
}

void push_capped(std::deque<float> &d, float v, std::size_t cap = history)
{
  d.push_back(v);
  while (d.size() > cap)
    d.pop_front();
}

// ---- a chart: a line over the last `capacity` samples, filled beneath, sized to the room it is
// given. With `readout`, the pointer gets a crosshair and the value under it.

class chart_t
{
public:
  struct props_t
  {
    std::vector<float> values;
    ui::color_t color{1, 1, 1, 1};
    int height = 120;
    bool grid = true;
    bool readout = false;
    // What a value of 1 reads as, and its unit.
    float scale = 100;
    std::string unit = "%";
    friend bool operator==(const props_t &, const props_t &) = default;
  };
  props_t props;

  // `height` is the least it takes; mounted in a box that hands it more, it fills that.
  ui::node_t view() const
  {
    const auto bounds = _area.bounds();
    const float w = float(std::max(bounds.width, 1));
    const int tall = std::max(bounds.height, props.height);
    const float h = float(tall);
    ui::box_t box{.size = {60, props.height}, .color = chart_back, .radius = 6, .events = _area, .align_x = ui::align_t::start, .align_y = ui::align_t::start, .clip = true};
    const vui::extent_t size{int(w), tall};
    if (props.grid)
    {
      for (float f : {0.25f, 0.5f, 0.75f})
        box.children.push_back(ui::polyline_t{.size = size, .points = {{0, h * f}, {w, h * f}}, .width = 1, .color = rule});
      for (int i = 1; i < 6; ++i)
      {
        const float x = w * float(i) / 6;
        box.children.push_back(ui::polyline_t{.size = size, .points = {{x, 0}, {x, h}}, .width = 1, .color = rule});
      }
    }
    if (props.values.size() >= 2 && bounds.width > 0)
    {
      std::vector<ui::vec2_t> points;
      points.reserve(props.values.size());
      const auto offset = history - props.values.size();
      for (std::size_t i = 0; i < props.values.size(); ++i)
        points.push_back({w * float(offset + i) / float(history - 1), 2 + (h - 4) * (1 - std::clamp(props.values[i], 0.0f, 1.0f))});
      box.children.push_back(ui::polyline_t{.size = size, .points = std::move(points), .width = props.grid ? 2.0f : 1.5f, .color = props.color, .fill = faded(props.color, 0.22f)});
    }
    if (props.readout && _pointer && !props.values.empty() && bounds.width > 0)
    {
      const float x = std::clamp(*_pointer, 0.0f, w);
      const auto offset = float(history - props.values.size());
      const auto index = long(std::lround(x / w * float(history - 1) - offset));
      box.children.push_back(ui::polyline_t{.size = size, .points = {{x, 0}, {x, h}}, .width = 1, .color = {1, 1, 1, 0.45f}});
      if (index >= 0 && std::size_t(index) < props.values.size())
      {
        const float v = props.values[std::size_t(index)];
        const float y = 2 + (h - 4) * (1 - std::clamp(v, 0.0f, 1.0f));
        box.children.push_back(ui::at_t{.x = x - 4, .y = y - 4, .children = {ui::box_t{.size = {8, 8}, .color = props.color, .radius = 4, .border_width = 2, .border_color = chart_back}}});
        const long ago = long(props.values.size()) - 1 - index;
        const auto label = fixed(v * props.scale, 1) + props.unit + (ago > 0 ? "  " + std::to_string(ago) + " s ago" : "  now");
        const float lx = x + 10 > w - 130 ? x - 136 : x + 10;
        box.children.push_back(ui::at_t{.x = lx, .y = 8, .children = {ui::box_t{.color = {0.05f, 0.05f, 0.07f, 0.85f}, .radius = 4, .padding = {8, 4}, .children = {text(label, 12, ui::theme().text, true)}}}});
      }
    }
    return box;
  }

  ui::behaviour_t behave(ui::scope_t &)
  {
    if (!props.readout)
      co_return;
    auto hovers = ui::subscribe<ui::hover_event_t>(_area);
    while (auto hover = co_await hovers)
    {
      if (hover->inside)
        _pointer = float(hover->position.x - _area.bounds().x);
      else
        _pointer.reset();
    }
  }

private:
  ui::pressable_t _area;
  std::optional<float> _pointer;
};

// A box that takes its share of a row and the whole of its height.
ui::node_t filling(ui::node_t child, float grow = 1)
{
  return ui::box_t{.color = clear, .grow = grow, .align_x = ui::align_t::stretch, .align_y = ui::align_t::stretch, .children = {std::move(child)}};
}

// A panel with a title. The first child marked to grow takes whatever height the panel is given
// beyond what it needs.
ui::node_t section(std::string title, ui::node_t right, std::vector<ui::node_t> body, float grow = 0)
{
  ui::column_t column{.gap = 10, .align = ui::align_t::stretch};
  column.children.push_back(ui::row_t{.gap = 10, .align = ui::align_t::center, .children = {text(std::move(title), 15, ui::theme().text), ui::spacer_t{}, std::move(right)}});
  for (auto &node : body)
    column.children.push_back(std::move(node));
  return ui::box_t{.color = panel, .radius = 10, .grow = grow, .padding = 14, .align_x = ui::align_t::stretch, .align_y = ui::align_t::stretch, .children = {std::move(column)}};
}

ui::node_t section(std::string title, std::string detail, std::vector<ui::node_t> body, float grow = 0)
{
  return section(std::move(title), muted(std::move(detail), 12), std::move(body), grow);
}

// ---- the process table

struct row_t
{
  std::uint32_t pid = 0;
  std::string name;
  sys::process_state_t state = sys::process_state_t::unknown;
  float cpu = 0;
  std::uint64_t memory = 0;
  std::uint32_t threads = 0;
  bool partial = false;
};

struct process_selected_t
{
  std::uint32_t pid = 0;
};

class process_table_t
{
public:
  struct props_t
  {
    std::shared_ptr<const std::vector<row_t>> rows;
    std::string filter;
    bool running_only = false;
    friend bool operator==(const props_t &, const props_t &) = default;
  };
  props_t props;
  ui::event_source_t<process_selected_t> selected;

  [[nodiscard]] std::size_t shown() const
  {
    return order().size();
  }

  ui::node_t view() const
  {
    const auto &t = ui::theme();
    const auto &order_ = order();
    const auto &rows = *props.rows;

    ui::row_t header{.align = ui::align_t::center};
    for (std::size_t c = 0; c < columns.size(); ++c)
    {
      const auto &column = columns[c];
      ui::row_t label{.gap = 4, .align = ui::align_t::center, .children = {text(column.title, 12, int(c) == _sort ? t.text : t.text_muted)}};
      if (int(c) == _sort)
        label.children.push_back(ui::icon_t{.path = chevron(!_descending), .size = 10, .color = t.text});
      header.children.push_back(ui::box_t{.size = {column.width, 30},
                                          .color = _headers[c].hovered() ? panel_raised : clear,
                                          .radius = 4,
                                          .events = _headers[c],
                                          .grow = column.width == 0 ? 1.0f : 0.0f,
                                          .padding = {10, 0},
                                          .align_x = column.right ? ui::align_t::end : ui::align_t::start,
                                          .children = {std::move(label)}});
    }

    const auto bounds = _body.bounds();
    const float height = std::max(float(bounds.height), row_height);
    const float width = float(std::max(bounds.width, 200));
    const float top = scroll_limit(height) > 0 ? std::clamp(_scroll, 0.0f, scroll_limit(height)) : 0.0f;
    const auto first = std::size_t(top / row_height);
    const auto last = std::min(order_.size(), first + std::size_t(height / row_height) + 2);

    ui::box_t body{.size = {300, 200},
                   .color = chart_back,
                   .radius = 6,
                   .border_width = _body.focus_visible() ? 2.0f : 0.0f,
                   .border_color = t.focus_ring,
                   .events = _body,
                   .grow = 1,
                   .align_x = ui::align_t::start,
                   .align_y = ui::align_t::start,
                   .clip = true};
    for (std::size_t i = first; i < last; ++i)
    {
      const auto &r = rows[order_[i]];
      const bool chosen = r.pid == _selected && _has_selection;
      const bool lit = int(i) == _hovered;
      ui::row_t cells{.align = ui::align_t::center};
      for (std::size_t c = 0; c < columns.size(); ++c)
        cells.children.push_back(cell(c, r));
      body.children.push_back(ui::at_t{.x = 0,
                                       .y = float(i) * row_height - top,
                                       .children = {ui::box_t{.size = {int(width), int(row_height)},
                                                              .color = chosen ? faded(t.accent, 0.35f) : (lit ? ui::color_t{1, 1, 1, 0.05f} : clear),
                                                              .align_x = ui::align_t::stretch,
                                                              .children = {std::move(cells)}}}});
    }
    if (order_.empty())
      body.children.push_back(ui::at_t{.x = 16, .y = 14, .children = {muted("No process matches.")}});
    const float content = float(order_.size()) * row_height;
    if (content > height && bounds.height > 0)
    {
      const float thumb = std::max(24.0f, height * height / content);
      const float y = top / scroll_limit(height) * (height - thumb);
      body.children.push_back(ui::at_t{.x = width - 9, .y = y, .children = {ui::box_t{.size = {6, int(thumb)}, .color = _thumb.held() || _thumb.hovered() ? t.border_hovered : t.scrollbar, .radius = 3, .events = _thumb}}});
    }
    return ui::column_t{.gap = 4, .align = ui::align_t::stretch, .children = {std::move(header), std::move(body)}};
  }

  ui::behaviour_t behave(ui::scope_t &scope)
  {
    auto heads = ui::subscribe<ui::press_event_t>(_headers);
    auto keys = ui::subscribe<vui::key_event_t>(_body);
    auto drags = ui::subscribe<ui::drag_event_t>(_body, _thumb);
    auto wheels = ui::subscribe<ui::wheel_event_t>(_body, _thumb);
    auto hovers = ui::subscribe<ui::hover_event_t>(_body);
    auto events = ui::merge(scope, heads, keys, drags, wheels, hovers);
    while (auto event = co_await events)
    {
      const float height = float(std::max(_body.bounds().height, int(row_height)));
      const auto row_at = [&](double y) { return long(std::floor((float(y) - float(_body.bounds().y) + clamped(height)) / row_height)); };
      switch (event->index())
      {
      case 0:
      {
        const auto c = int(std::get<0>(*event).source - &_headers[0]);
        if (c == _sort)
          _descending = !_descending;
        else
        {
          _sort = c;
          _descending = columns[std::size_t(c)].descending;
        }
        break;
      }
      case 1:
      {
        const auto &key = std::get<1>(*event);
        if (!key.pressed || order().empty())
          break;
        const long n = long(order().size());
        long at = index_of_selected();
        const long page = std::max(1L, long(height / row_height) - 1);
        if (key.key == vui::key_t::down)
          at = std::min(at + 1, n - 1);
        else if (key.key == vui::key_t::up)
          at = std::max(at - 1, 0L);
        else if (key.key == vui::key_t::page_down)
          at = std::min(at + page, n - 1);
        else if (key.key == vui::key_t::page_up)
          at = std::max(at - page, 0L);
        else if (key.key == vui::key_t::home)
          at = 0;
        else if (key.key == vui::key_t::end)
          at = n - 1;
        else
          break;
        select(std::size_t(std::max(at, 0L)), scope);
        // Keep it in view.
        const float y = float(at) * row_height;
        if (y < _scroll)
          _scroll = y;
        if (y + row_height > _scroll + height)
          _scroll = y + row_height - height;
        break;
      }
      case 2:
      {
        const auto &drag = std::get<2>(*event);
        if (drag.source == &_thumb)
        {
          const float content = float(order().size()) * row_height;
          const float thumb = std::max(24.0f, height * height / std::max(content, 1.0f));
          if (drag.phase == ui::drag_phase_t::begin)
            _grab = float(drag.position.y) - float(_body.bounds().y) - clamped(height) / std::max(scroll_limit(height), 1.0f) * (height - thumb);
          else if (drag.phase == ui::drag_phase_t::move && height > thumb)
            _scroll = (float(drag.position.y) - float(_body.bounds().y) - _grab) / (height - thumb) * scroll_limit(height);
          break;
        }
        if (drag.phase != ui::drag_phase_t::begin)
          break;
        const long at = row_at(drag.position.y);
        if (at >= 0 && std::size_t(at) < order().size())
          select(std::size_t(at), scope);
        break;
      }
      case 3:
        _scroll -= float(std::get<3>(*event).dy) * 3 * row_height;
        break;
      case 4:
      {
        const auto &hover = std::get<4>(*event);
        _hovered = hover.inside ? int(row_at(hover.position.y)) : -1;
        break;
      }
      }
      _scroll = clamped(height);
    }
  }

private:
  struct column_t
  {
    const char *title;
    // 0 takes what is left.
    int width;
    bool right;
    // Which way a first press sorts it: biggest first for the numbers worth hunting.
    bool descending;
  };
  static constexpr float row_height = 26;
  inline static const std::vector<column_t> columns{{"Name", 0, false, false}, {"PID", 80, true, false}, {"Status", 112, false, false},
                                                    {"CPU", 96, true, true},   {"Memory", 110, true, true}, {"Threads", 84, true, true}};

  [[nodiscard]] float scroll_limit(float height) const
  {
    return std::max(0.0f, float(order().size()) * row_height - height);
  }
  [[nodiscard]] float clamped(float height) const
  {
    return std::clamp(_scroll, 0.0f, scroll_limit(height));
  }

  ui::node_t cell(std::size_t c, const row_t &r) const
  {
    const auto &t = ui::theme();
    const auto &column = columns[c];
    ui::box_t box{.size = {column.width, int(row_height)}, .color = clear, .grow = column.width == 0 ? 1.0f : 0.0f, .padding = {10, 0}, .align_x = column.right ? ui::align_t::end : ui::align_t::start};
    switch (c)
    {
    case 0:
      box.clip = true;
      box.size.width = 120;
      box.children.push_back(text(r.name.empty() ? "?" : r.name, 13, r.partial ? t.text_muted : t.text));
      break;
    case 1:
      box.children.push_back(text(std::to_string(r.pid), 12, t.text_muted, true));
      break;
    case 2:
    {
      const auto color = state_color(r.state);
      box.children.push_back(ui::box_t{.color = faded(color, 0.16f),
                                       .radius = 9,
                                       .padding = {8, 2},
                                       .children = {ui::row_t{.gap = 6, .align = ui::align_t::center, .children = {ui::box_t{.size = {6, 6}, .color = color, .radius = 3}, text(sys::to_string(r.state), 12, color)}}}});
      break;
    }
    case 3:
    {
      // A bar behind the number, as hot as the process.
      const float share = std::clamp(r.cpu / 100.0f, 0.0f, 1.0f);
      box.align_x = ui::align_t::end;
      box.children.push_back(ui::at_t{.x = -10, .y = 4, .children = {ui::box_t{.size = {std::max(int(float(column.width - 4) * share), share > 0 ? 2 : 0), int(row_height) - 8}, .color = faded(cpu_color, 0.18f + 0.5f * share), .radius = 3}}});
      box.children.push_back(text(r.partial ? "-" : fixed(r.cpu, 1), 12, t.text, true));
      break;
    }
    case 4:
      box.children.push_back(text(r.partial && r.memory == 0 ? "-" : bytes(r.memory), 12, t.text, true));
      break;
    default:
      box.children.push_back(text(r.partial && r.threads == 0 ? "-" : std::to_string(r.threads), 12, t.text_muted, true));
      break;
    }
    return box;
  }

  // The rows shown, in order: filtered, then sorted. Kept until what decides it changes.
  const std::vector<std::size_t> &order() const
  {
    if (_cached_rows == props.rows && _cached_filter == props.filter && _cached_running == props.running_only && _cached_sort == _sort && _cached_descending == _descending)
      return _order;
    _cached_rows = props.rows;
    _cached_filter = props.filter;
    _cached_running = props.running_only;
    _cached_sort = _sort;
    _cached_descending = _descending;
    _order.clear();
    if (!props.rows)
      return _order;
    const auto &rows = *props.rows;
    std::string needle = props.filter;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
      const auto &r = rows[i];
      if (props.running_only && r.state != sys::process_state_t::running)
        continue;
      if (!needle.empty())
      {
        std::string name = r.name;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (name.find(needle) == std::string::npos && std::to_string(r.pid).find(needle) == std::string::npos)
          continue;
      }
      _order.push_back(i);
    }
    const auto less = [this, &rows](std::size_t a, std::size_t b)
    {
      const auto &x = rows[a];
      const auto &y = rows[b];
      switch (_sort)
      {
      case 0:
      {
        const auto n = std::min(x.name.size(), y.name.size());
        for (std::size_t i = 0; i < n; ++i)
        {
          const int cx = std::tolower(static_cast<unsigned char>(x.name[i]));
          const int cy = std::tolower(static_cast<unsigned char>(y.name[i]));
          if (cx != cy)
            return cx < cy;
        }
        return x.name.size() != y.name.size() ? x.name.size() < y.name.size() : x.pid < y.pid;
      }
      case 1:
        return x.pid < y.pid;
      case 2:
        return x.state != y.state ? x.state < y.state : x.pid < y.pid;
      case 3:
        return x.cpu != y.cpu ? x.cpu < y.cpu : x.pid < y.pid;
      case 4:
        return x.memory != y.memory ? x.memory < y.memory : x.pid < y.pid;
      default:
        return x.threads != y.threads ? x.threads < y.threads : x.pid < y.pid;
      }
    };
    if (_descending)
      std::stable_sort(_order.begin(), _order.end(), [&less](std::size_t a, std::size_t b) { return less(b, a); });
    else
      std::stable_sort(_order.begin(), _order.end(), less);
    return _order;
  }

  [[nodiscard]] long index_of_selected() const
  {
    if (!_has_selection)
      return -1;
    const auto &o = order();
    for (std::size_t i = 0; i < o.size(); ++i)
    {
      if ((*props.rows)[o[i]].pid == _selected)
        return long(i);
    }
    return -1;
  }

  void select(std::size_t index, ui::scope_t &scope)
  {
    const auto pid = (*props.rows)[order()[index]].pid;
    if (_has_selection && pid == _selected)
      return;
    _selected = pid;
    _has_selection = true;
    ui::emit(selected, process_selected_t{pid}, scope);
  }

  std::array<ui::pressable_t, 6> _headers;
  ui::pressable_t _body{ui::focusable};
  ui::pressable_t _thumb;
  int _sort = 3;
  bool _descending = true;
  float _scroll = 0;
  float _grab = 0;
  int _hovered = -1;
  std::uint32_t _selected = 0;
  bool _has_selection = false;

  mutable std::vector<std::size_t> _order;
  mutable std::shared_ptr<const std::vector<row_t>> _cached_rows;
  mutable std::string _cached_filter;
  mutable bool _cached_running = false;
  mutable int _cached_sort = -1;
  mutable bool _cached_descending = false;
};

// ---- sampling, on a thread of its own

class sampler_t
{
public:
  sampler_t(std::unique_ptr<sys::probe_t> probe, vio::event_pipe_t<sys::snapshot_t> &pipe, std::chrono::milliseconds period)
    : _thread([this, probe = std::move(probe), &pipe, period] { run(*probe, pipe, period); })
  {
  }
  sampler_t(const sampler_t &) = delete;
  sampler_t &operator=(const sampler_t &) = delete;
  ~sampler_t()
  {
    {
      std::lock_guard lock(_mutex);
      _stop = true;
    }
    _wake.notify_all();
    _thread.join();
  }

private:
  void run(sys::probe_t &probe, vio::event_pipe_t<sys::snapshot_t> &pipe, std::chrono::milliseconds period)
  {
    std::unique_lock lock(_mutex);
    while (!_stop)
    {
      lock.unlock();
      pipe.post_event(probe.sample());
      lock.lock();
      _wake.wait_for(lock, period, [this] { return _stop; });
    }
  }

  std::mutex _mutex;
  std::condition_variable _wake;
  bool _stop = false;
  // Last, so it starts once the rest exists.
  std::thread _thread;
};

// ---- the whole window

enum class tab_t
{
  performance,
  processes,
};

enum class view_t
{
  cpu,
  memory,
};

struct task_manager_t
{
  explicit task_manager_t(std::shared_ptr<ui::frame_stats_t> stats)
    : probe(std::make_unique<sys::probe_t>())
    , machine(probe->machine())
    , frame(std::move(stats))
  {
    const auto cores = std::max<unsigned>(machine.logical_cores, 1);
    core_charts.resize(cores);
    core_history.resize(cores);
    heat->columns = heat_columns;
    heat->rows = int(cores);
    heat->values.assign(std::size_t(heat_columns) * cores, 0.0f);
  }

  // Owned until the sampler takes it.
  std::unique_ptr<sys::probe_t> probe;
  sys::machine_t machine;
  std::shared_ptr<ui::frame_stats_t> frame;

  tab_t tab = tab_t::performance;
  view_t view_kind = view_t::cpu;
  int core_mode = 0;
  std::string filter;
  bool running_only = false;
  std::uint32_t selected = 0;
  bool has_selected = false;

  // What the snapshots add up to.
  bool sampled = false;
  sys::snapshot_t last;
  std::deque<float> cpu_history, memory_history, swap_history, cost_history;
  std::vector<std::deque<float>> core_history;
  std::shared_ptr<ui::heatmap_data_t> heat = std::make_shared<ui::heatmap_data_t>();
  std::shared_ptr<const std::vector<row_t>> rows = std::make_shared<const std::vector<row_t>>();
  std::unordered_map<std::uint32_t, std::deque<float>> process_history;
  std::uint64_t threads = 0;
  std::size_t samples = 0;

  // The widgets.
  ui::button_t performance_tab, processes_tab;
  ui::pressable_t cpu_card, memory_card;
  chart_t cpu_chart, memory_chart, cpu_spark, memory_spark, swap_spark, cost_spark, process_chart;
  std::deque<chart_t> core_charts;
  ui::combobox_t core_mode_box;
  ui::line_edit_t filter_edit;
  ui::checkbox_t running_box;
  process_table_t table;
  ui::text_edit_t activity;
  ui::pressable_t heat_area;

  static std::vector<float> values(const std::deque<float> &d)
  {
    return {d.begin(), d.end()};
  }

  void take(sys::snapshot_t s, ui::scope_t &scope)
  {
    ++samples;
    const auto cores = core_history.size();
    if (sampled)
    {
      const auto ratio = [](const sys::cpu_times_t &now, const sys::cpu_times_t &before)
      {
        const auto total = now.total > before.total ? now.total - before.total : 0;
        const auto busy = now.busy > before.busy ? now.busy - before.busy : 0;
        return total > 0 ? float(double(busy) / double(total)) : 0.0f;
      };
      push_capped(cpu_history, ratio(s.cpu, last.cpu));
      // Shift the heatmap one column left and paint the newest on the right.
      for (std::size_t c = 0; c < cores; ++c)
      {
        const float v = c < s.cores.size() && c < last.cores.size() ? ratio(s.cores[c], last.cores[c]) : 0.0f;
        push_capped(core_history[c], v);
        auto *row = heat->values.data() + c * std::size_t(heat_columns);
        std::move(row + 1, row + heat_columns, row);
        row[heat_columns - 1] = v;
      }
      ++heat->version;
    }
    push_capped(memory_history, s.memory_total ? float(double(s.memory_used) / double(s.memory_total)) : 0.0f);
    push_capped(swap_history, s.swap_total ? float(double(s.swap_used) / double(s.swap_total)) : 0.0f);
    push_capped(cost_history, float(double(s.cost.count()) / 1e6 / 20.0));

    // Per process: CPU since the last snapshot, as a share of the whole machine.
    std::unordered_map<std::uint32_t, std::uint64_t> before;
    std::unordered_set<std::uint32_t> known;
    for (const auto &p : last.processes)
    {
      before.emplace(p.pid, p.cpu_ns);
      known.insert(p.pid);
    }
    const double elapsed = sampled ? double(std::chrono::duration_cast<std::chrono::nanoseconds>(s.taken - last.taken).count()) * double(std::max<std::size_t>(cores, 1)) : 0;
    auto next = std::make_shared<std::vector<row_t>>();
    next->reserve(s.processes.size());
    threads = 0;
    std::unordered_set<std::uint32_t> present;
    for (const auto &p : s.processes)
    {
      row_t r{p.pid, p.name, p.state, 0, p.memory, p.threads, p.partial};
      if (auto it = before.find(p.pid); elapsed > 0 && it != before.end() && p.cpu_ns >= it->second)
        r.cpu = float(double(p.cpu_ns - it->second) / elapsed * 100.0);
      threads += p.threads;
      push_capped(process_history[p.pid], r.cpu / 100.0f);
      present.insert(p.pid);
      if (sampled && !known.contains(p.pid))
        activity.append_line(scope, "+ started  " + p.name + "  (" + std::to_string(p.pid) + ")");
      next->push_back(std::move(r));
    }
    if (sampled)
    {
      for (const auto &p : last.processes)
      {
        if (!present.contains(p.pid))
        {
          activity.append_line(scope, "- exited   " + p.name + "  (" + std::to_string(p.pid) + ")");
          process_history.erase(p.pid);
        }
      }
    }
    else
      activity.append_line(scope, "watching " + std::to_string(s.processes.size()) + " processes on " + machine.os_name);
    rows = std::move(next);
    last = std::move(s);
    sampled = true;
  }

  // ---- views

  ui::node_t card(const ui::pressable_t *source, bool chosen, std::string title, std::string value, std::string detail, const chart_t &spark, chart_t::props_t spark_props) const
  {
    const auto &t = ui::theme();
    return ui::box_t{.color = chosen ? panel_raised : panel,
                     .radius = 10,
                     .border_width = chosen ? 1.5f : 0.0f,
                     .border_color = faded(spark_props.color, 0.7f),
                     .events = source ? ui::event_ref_t(*source) : ui::event_ref_t{},
                     .padding = {14, 10},
                     .align_x = ui::align_t::stretch,
                     .children = {ui::column_t{.gap = 4,
                                               .align = ui::align_t::stretch,
                                               .children = {ui::row_t{.align = ui::align_t::baseline, .children = {text(std::move(title), 13, t.text_muted), ui::spacer_t{}, text(std::move(value), 18, spark_props.color)}},
                                                            muted(std::move(detail), 12),
                                                            ui::mount(spark, std::move(spark_props))}}}};
  }

  ui::node_t sidebar() const
  {
    const float cpu = cpu_history.empty() ? 0 : cpu_history.back() * 100;
    const auto cost = last.cost;
    return ui::column_t{
      .gap = 10,
      .align = ui::align_t::stretch,
      .children = {card(&cpu_card, view_kind == view_t::cpu, "CPU", fixed(cpu, 0) + "%", std::to_string(machine.logical_cores) + " logical processors", cpu_spark, {.values = values(cpu_history), .color = cpu_color, .height = 26, .grid = false}),
                   card(&memory_card, view_kind == view_t::memory, "Memory", bytes(last.memory_used), "of " + bytes(last.memory_total), memory_spark, {.values = values(memory_history), .color = memory_color, .height = 26, .grid = false}),
                   card(nullptr, false, "Swap", bytes(last.swap_used), last.swap_total ? "of " + bytes(last.swap_total) : "none configured", swap_spark, {.values = values(swap_history), .color = swap_color, .height = 26, .grid = false}),
                   card(nullptr, false, "Sampler", ms(cost), "per snapshot, on its own thread", cost_spark, {.values = values(cost_history), .color = sampler_color, .height = 26, .grid = false, .scale = 20, .unit = " ms"})}};
  }

  ui::node_t stat(std::string name, std::string value) const
  {
    return ui::column_t{.gap = 2, .children = {muted(std::move(name), 12), text(std::move(value), 17, ui::theme().text)}};
  }

  ui::node_t cpu_view() const
  {
    const auto cores = core_charts.size();
    ui::node_t per_core;
    if (core_mode == 0)
    {
      const std::size_t per_row = cores <= 8 ? std::max<std::size_t>(cores, 1) : 8;
      ui::column_t grid{.gap = 6, .align = ui::align_t::stretch};
      for (std::size_t first = 0; first < cores; first += per_row)
      {
        ui::row_t line{.gap = 8, .align = ui::align_t::stretch};
        for (std::size_t c = first; c < first + per_row; ++c)
        {
          if (c >= cores)
          {
            line.children.push_back(ui::spacer_t{});
            continue;
          }
          const float now = core_history[c].empty() ? 0 : core_history[c].back() * 100;
          line.children.push_back(filling(ui::column_t{.gap = 2,
                                                        .align = ui::align_t::stretch,
                                                        .children = {muted(std::to_string(c) + "  " + fixed(now, 0) + "%", 11),
                                                                     ui::mount(core_charts[c], {.values = values(core_history[c]), .color = cpu_color, .height = cores > 16 ? 22 : 30, .grid = false})}}));
        }
        grid.children.push_back(std::move(line));
      }
      per_core = std::move(grid);
    }
    else
    {
      const auto heat_bounds = heat_area.bounds();
      const int heat_height = std::clamp(int(cores) * 6, 48, 120);
      ui::box_t heatmap{.size = {100, heat_height}, .color = chart_back, .radius = 6, .events = heat_area, .align_x = ui::align_t::start, .align_y = ui::align_t::start, .clip = true};
      if (heat_bounds.width > 0)
        heatmap.children.push_back(ui::heatmap_t{.size = {heat_bounds.width, heat_height}, .data = heat, .low = 0, .high = 1});
      per_core = ui::column_t{.gap = 4, .align = ui::align_t::stretch, .children = {std::move(heatmap), muted("One row per core over the last two minutes, the newest on the right. Brighter is busier.", 11)}};
    }

    const float cpu = cpu_history.empty() ? 0 : cpu_history.back() * 100;
    return ui::column_t{
      .gap = 14,
      .align = ui::align_t::stretch,
      .children = {
        section("CPU", machine.cpu_name,
                {filling(ui::mount(cpu_chart, {.values = values(cpu_history), .color = cpu_color, .height = 110, .readout = true})),
                 ui::row_t{.gap = 36,
                           .children = {stat("Utilization", fixed(cpu, 0) + "%"), stat("Processes", grouped(last.processes.size())), stat("Threads", grouped(threads)), stat("Up time", duration(last.uptime))}}},
                1),
        section("Per core", ui::mount(core_mode_box, {.options = {"Charts, last minute", "Heatmap, two minutes"}, .selected = core_mode}), {std::move(per_core)}),
      }};
  }

  ui::node_t memory_view() const
  {
    const double used = last.memory_total ? double(last.memory_used) / double(last.memory_total) : 0;
    // In use against available, as one bar.
    ui::row_t bar{.align = ui::align_t::stretch,
                  .children = {ui::box_t{.size = {0, 26}, .color = faded(memory_color, 0.75f), .radius = 5, .grow = float(std::max(used, 0.001))},
                               ui::box_t{.size = {0, 26}, .color = faded(memory_color, 0.15f), .radius = 5, .grow = float(std::max(1 - used, 0.001))}}};
    return ui::column_t{
      .gap = 14,
      .align = ui::align_t::stretch,
      .children = {section("Memory", bytes(last.memory_total) + " installed",
                           {filling(ui::mount(memory_chart, {.values = values(memory_history), .color = memory_color, .height = 110, .readout = true})),
                            std::move(bar),
                            ui::row_t{.gap = 36,
                                      .children = {stat("In use", bytes(last.memory_used)), stat("Available", bytes(last.memory_total - std::min(last.memory_used, last.memory_total))),
                                                   stat("Swap", bytes(last.swap_used) + " / " + bytes(last.swap_total)), stat("Processes", grouped(last.processes.size()))}}},
                           1),
                   section("Biggest processes", "by memory", {biggest()})}};
  }

  ui::node_t biggest() const
  {
    std::vector<const row_t *> by_memory;
    for (const auto &r : *rows)
      by_memory.push_back(&r);
    const auto n = std::min<std::size_t>(6, by_memory.size());
    std::partial_sort(by_memory.begin(), by_memory.begin() + std::ptrdiff_t(n), by_memory.end(), [](const row_t *a, const row_t *b) { return a->memory > b->memory; });
    ui::column_t list{.gap = 6, .align = ui::align_t::stretch};
    const double top = n > 0 ? double(std::max<std::uint64_t>(by_memory[0]->memory, 1)) : 1;
    for (std::size_t i = 0; i < n; ++i)
    {
      const auto &r = *by_memory[i];
      list.children.push_back(ui::row_t{.gap = 10,
                                        .align = ui::align_t::center,
                                        .children = {ui::box_t{.size = {180, 0}, .color = clear, .align_x = ui::align_t::start, .clip = true, .children = {text(r.name, 13, ui::theme().text)}},
                                                     ui::box_t{.size = {int(260 * double(r.memory) / top) + 2, 10}, .color = faded(memory_color, 0.6f), .radius = 3},
                                                     ui::spacer_t{},
                                                     text(bytes(r.memory), 12, ui::theme().text_muted, true)}});
    }
    return list;
  }

  ui::node_t performance() const
  {
    return ui::row_t{.gap = 16,
                     .align = ui::align_t::stretch,
                     .grow = 1,
                     .children = {ui::box_t{.size = {250, 0}, .color = clear, .align_x = ui::align_t::stretch, .align_y = ui::align_t::start, .children = {sidebar()}},
                                  filling(view_kind == view_t::cpu ? cpu_view() : memory_view())}};
  }

  ui::node_t details() const
  {
    const row_t *r = nullptr;
    if (has_selected)
    {
      for (const auto &row : *rows)
      {
        if (row.pid == selected)
          r = &row;
      }
    }
    if (!r)
      return section("Details", "", {muted(has_selected ? "That process has exited." : "Select a process to see it here.")});
    std::vector<float> cpu;
    if (auto it = process_history.find(r->pid); it != process_history.end())
      cpu = values(it->second);
    // A process gets its own scale: most use a sliver of the machine.
    float peak = 0.01f;
    for (float v : cpu)
      peak = std::max(peak, v);
    for (float &v : cpu)
      v /= peak;
    const auto color = state_color(r->state);
    return section("Details", "pid " + std::to_string(r->pid),
                   {text(r->name, 17, ui::theme().text),
                    ui::row_t{.gap = 6, .align = ui::align_t::center, .children = {ui::box_t{.size = {8, 8}, .color = color, .radius = 4}, text(sys::to_string(r->state), 13, color)}},
                    muted("CPU, last minute, up to " + fixed(peak * 100, 1) + "%", 12),
                    ui::mount(process_chart, {.values = std::move(cpu), .color = cpu_color, .height = 90, .readout = true, .scale = peak * 100}),
                    ui::row_t{.gap = 28, .children = {stat("CPU", r->partial ? "-" : fixed(r->cpu, 1) + "%"), stat("Memory", bytes(r->memory)), stat("Threads", std::to_string(r->threads))}},
                    r->partial ? muted("The system does not tell this user\neverything about that process.", 12) : ui::node_t(ui::spacer_t{.grow = 0})});
  }

  ui::node_t processes() const
  {
    return ui::row_t{
      .gap = 16,
      .align = ui::align_t::stretch,
      .grow = 1,
      .children = {
        filling(ui::column_t{.gap = 10,
                             .align = ui::align_t::stretch,
                             .children = {ui::row_t{.gap = 14,
                                                    .align = ui::align_t::center,
                                                    .children = {ui::mount(filter_edit, {.text = filter, .placeholder = "filter by name or pid", .columns = 28}),
                                                                 ui::mount(running_box, {.label = "Running only", .checked = running_only}), ui::spacer_t{},
                                                                 muted(grouped(table.shown()) + " of " + grouped(rows->size()) + " processes  ·  " + grouped(threads) + " threads", 12)}},
                                          filling(ui::mount(table, {.rows = rows, .filter = filter, .running_only = running_only})),
                                          muted("Activity", 12),
                                          ui::mount(activity, {.read_only = true, .columns = 40, .rows = 5, .follow = true, .max_lines = 2000})}}),
        ui::box_t{.size = {300, 0}, .color = clear, .align_x = ui::align_t::stretch, .align_y = ui::align_t::start, .children = {details()}},
      }};
  }

  ui::node_t footer() const
  {
    const auto &f = *frame;
    return muted("frame: views " + ms(f.views) + " · layout " + ms(f.layout) + " · display list " + ms(f.display_list) + " · render " + ms(f.render) + "   " + grouped(f.prims) + " prims in " + grouped(f.runs) + " runs · " +
                   grouped(f.components) + " components · " + std::to_string(samples) + " snapshots",
                 11);
  }

  ui::node_t view() const
  {
    const auto &t = ui::theme();
    return ui::column_t{
      .gap = 16,
      .align = ui::align_t::stretch,
      .padding = 20,
      .children = {ui::row_t{.gap = 10,
                             .align = ui::align_t::center,
                             .children = {text("vui task manager", 22, t.text), ui::box_t{.size = {12, 0}, .color = clear},
                                          ui::mount(performance_tab, {.label = "Performance", .primary = tab == tab_t::performance}),
                                          ui::mount(processes_tab, {.label = "Processes", .primary = tab == tab_t::processes}), ui::spacer_t{},
                                          muted(machine.os_name + "  ·  up " + duration(last.uptime), 12)}},
                   tab == tab_t::performance ? performance() : processes(), footer()}};
  }

  ui::behaviour_t behave(ui::scope_t &scope)
  {
    vio::event_pipe_t<sys::snapshot_t> pipe(scope.loop(),
                                            [this, &scope](sys::snapshot_t &&s)
                                            {
                                              take(std::move(s), scope);
                                              scope.invalidate();
                                            });
    sampler_t sampler(std::move(probe), pipe, 1s);

    auto presses = ui::subscribe<ui::press_event_t>(performance_tab.press, processes_tab.press, cpu_card, memory_card);
    auto edits = ui::subscribe<ui::line_edit_event_t>(filter_edit.changed);
    auto toggles = ui::subscribe<ui::toggle_event_t>(running_box.changed);
    auto picks = ui::subscribe<process_selected_t>(table.selected);
    auto modes = ui::subscribe<ui::select_event_t>(core_mode_box.changed);
    auto events = ui::merge(scope, presses, edits, toggles, picks, modes);
    while (auto event = co_await events)
    {
      switch (event->index())
      {
      case 0:
      {
        const auto *source = std::get<0>(*event).source;
        if (source == &performance_tab.press)
          tab = tab_t::performance;
        else if (source == &processes_tab.press)
          tab = tab_t::processes;
        else if (source == &cpu_card)
          view_kind = view_t::cpu;
        else
          view_kind = view_t::memory;
        break;
      }
      case 1:
        filter = std::get<1>(*event).text;
        break;
      case 2:
        running_only = std::get<2>(*event).checked;
        break;
      case 3:
        selected = std::get<3>(*event).pid;
        has_selected = true;
        break;
      case 4:
        core_mode = int(std::get<4>(*event).index);
        break;
      }
    }
  }
};

// ---- --probe: the system layer on its own, for CI

int probe_and_check()
{
  sys::probe_t probe;
  const auto &m = probe.machine();
  std::printf("machine: %s, %s, %u logical processors\n", m.os_name.c_str(), m.cpu_name.c_str(), m.logical_cores);
  const auto a = probe.sample();
  std::this_thread::sleep_for(500ms);
  const auto b = probe.sample();
  int failures = 0;
  const auto check = [&failures](bool ok, const char *what)
  {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
  };
  const double cpu = b.cpu.total > a.cpu.total ? double(b.cpu.busy - a.cpu.busy) / double(b.cpu.total - a.cpu.total) * 100 : -1;
  std::printf("cpu %.1f%% over %zu cores, memory %s of %s, swap %s of %s, up %s, %zu processes, sampled in %s\n", cpu, b.cores.size(), bytes(b.memory_used).c_str(), bytes(b.memory_total).c_str(), bytes(b.swap_used).c_str(),
              bytes(b.swap_total).c_str(), duration(b.uptime).c_str(), b.processes.size(), ms(b.cost).c_str());
  check(m.logical_cores > 0, "logical processors");
  check(!b.cores.empty(), "per-core times");
  check(b.cpu.total > a.cpu.total, "processor time advances");
  check(cpu >= 0 && cpu <= 100, "utilization is a percentage");
  check(b.memory_total > 0 && b.memory_used > 0 && b.memory_used <= b.memory_total, "memory");
  check(b.uptime.count() > 0, "up time");
  check(b.processes.size() > 1, "more than one process");
  const auto self = std::find_if(b.processes.begin(), b.processes.end(), [](const sys::process_t &p) { return p.pid == sys::current_pid(); });
  check(self != b.processes.end(), "this process is listed");
  if (self != b.processes.end())
  {
    std::printf("  this process: %s, %s, %u threads, %s, %.1f ms of CPU\n", self->name.c_str(), sys::to_string(self->state), self->threads, bytes(self->memory).c_str(), double(self->cpu_ns) / 1e6);
    check(!self->name.empty() && !self->partial, "this process is fully readable");
    check(self->memory > 0 && self->threads > 0, "this process has memory and threads");
  }
  return failures == 0 ? 0 : 1;
}
} // namespace

int main(int argc, char **argv)
{
  if (argc > 1 && std::strcmp(argv[1], "--probe") == 0)
    return probe_and_check();
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
      auto window = vui::window_t::create(*display, {.title = "vui task manager", .size = {1440, 960}});
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
      auto stats = std::make_shared<ui::frame_stats_t>();
      ui::run_config_t config;
      config.on_stats = [stats](const ui::frame_stats_t &s) { *stats = s; };
      co_await ui::run(*window, task_manager_t{stats}, std::move(config));
      co_await std::move(closing);
      co_return 0;
    });
}
