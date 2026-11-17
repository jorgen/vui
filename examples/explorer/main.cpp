// An explorer: the second advanced example. Folders open at once and fill in as they are read --
// shell::list hands over batches, the first of them small -- and nothing slow ever runs on the loop:
// one worker thread lists folders, dropping a listing the moment the user moves on, and another
// answers everything else (type names, the applications that open a file, previews, launching).
// Their results come back as closures through a vio::event_pipe_t and run on the loop, where the
// state is.
//
// What it does: Back, Forward, Up, Home and Refresh; clickable breadcrumbs; a sidebar of the user's
// folders and drives; a table sorted by any column (folders first, "file2" before "file10"),
// filtered as you type, driven by the keyboard (arrows, Enter, Backspace, typing jumps to a name);
// a context menu with Open, the applications that open the file, Copy path, Show in the system's
// file manager, Open terminal here, Delete -- to the Recycle Bin or Trash, after asking -- Delete
// permanently (Shift+Delete, after asking harder) and Properties; a preview of text files; and a status bar that
// says what each folder cost.
//
// `vui_explorer --probe` exercises the shell layer without a window, for CI.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
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

#include "shell.h"

namespace ui = vui::ui;
using namespace std::chrono_literals;

namespace
{
using clock_t_ = std::chrono::steady_clock;

// ---- look

constexpr ui::color_t clear{0, 0, 0, 0};
constexpr ui::color_t panel{0.155f, 0.155f, 0.18f, 1};
constexpr ui::color_t raised{0.2f, 0.2f, 0.235f, 1};
constexpr ui::color_t well{0.11f, 0.11f, 0.13f, 1};
constexpr ui::color_t folder_color{0.98f, 0.76f, 0.33f, 1};
constexpr ui::color_t file_color{0.62f, 0.7f, 0.82f, 1};
constexpr ui::color_t drive_color{0.55f, 0.8f, 0.95f, 1};
constexpr ui::color_t ok_color{0.45f, 0.85f, 0.55f, 1};
constexpr ui::color_t error_color{1.0f, 0.45f, 0.42f, 1};

ui::color_t faded(ui::color_t c, float a)
{
  c.a = a;
  return c;
}

ui::node_t text(std::string s, float size, ui::color_t color, bool mono = false)
{
  return ui::label({.text = std::move(s), .monospace = mono, .size = size, .color = color});
}

ui::node_t muted(std::string s, float size = 12)
{
  return text(std::move(s), size, ui::theme().text_muted);
}

std::string fixed(double v, int decimals)
{
  char b[32];
  std::snprintf(b, sizeof b, "%.*f", decimals, v);
  return b;
}

std::string bytes(std::uint64_t n)
{
  if (n < 1024)
    return std::to_string(n) + " B";
  const char *units[] = {"KB", "MB", "GB", "TB"};
  double v = double(n) / 1024;
  int unit = 0;
  while (v >= 1024 && unit < 3)
  {
    v /= 1024;
    ++unit;
  }
  return fixed(v, v < 10 ? 1 : 0) + " " + units[unit];
}

std::string grouped(std::uint64_t n)
{
  auto s = std::to_string(n);
  for (int i = int(s.size()) - 3; i > 0; i -= 3)
    s.insert(std::size_t(i), ",");
  return s;
}

std::string ms(std::chrono::nanoseconds t)
{
  return fixed(double(t.count()) / 1e6, t < 10ms ? 2 : 1) + " ms";
}

std::string when(std::int64_t seconds)
{
  if (seconds <= 0)
    return "";
  const std::time_t t = std::time_t(seconds);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  char b[32];
  std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &local);
  return b;
}

std::string lower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

std::string extension_of(const std::string &name)
{
  const auto dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0)
    return {};
  return lower(name.substr(dot));
}

// A name folded once, when it arrives, so that comparing and filtering never fold again. ASCII only:
// std::tolower consults the locale on every call, which made sorting 32,000 names cost 112 ms.
std::string fold(const std::string &s)
{
  std::string out = s;
  for (auto &c : out)
  {
    if (c >= 'A' && c <= 'Z')
      c = char(c + ('a' - 'A'));
  }
  return out;
}

bool digit(char c) noexcept
{
  return c >= '0' && c <= '9';
}

// "file2" before "file10" on folded names: runs of digits compare as numbers, the rest as bytes.
int natural_folded(const std::string &a, const std::string &b) noexcept
{
  std::size_t i = 0, j = 0;
  while (i < a.size() && j < b.size())
  {
    if (digit(a[i]) && digit(b[j]))
    {
      while (i < a.size() && a[i] == '0')
        ++i;
      while (j < b.size() && b[j] == '0')
        ++j;
      std::size_t ei = i, ej = j;
      while (ei < a.size() && digit(a[ei]))
        ++ei;
      while (ej < b.size() && digit(b[ej]))
        ++ej;
      if (ei - i != ej - j)
        return ei - i < ej - j ? -1 : 1;
      if (const int c = a.compare(i, ei - i, b, j, ej - j); c != 0)
        return c;
      i = ei;
      j = ej;
      continue;
    }
    if (a[i] != b[j])
      return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[j]) ? -1 : 1;
    ++i;
    ++j;
  }
  if (i < a.size())
    return 1;
  if (j < b.size())
    return -1;
  return 0;
}

// A path as it is best pasted: with forward slashes, which Windows accepts everywhere and which a
// shell, a CMake file or a URL wants. The backslashed form is the one asked for by name.
std::string portable(std::string path)
{
#if defined(_WIN32)
  std::replace(path.begin(), path.end(), '\\', '/');
#endif
  return path;
}

// What someone typed or pasted into the path bar, as a path to go to: quotes and spaces around it
// dropped, ~ for home, and on Windows either slash, with a bare drive meaning its root.
std::string typed_path(std::string text)
{
  const auto trim = [](std::string &s)
  {
    while (!s.empty() && (s.back() == ' ' || s.back() == '"' || s.back() == '\'' || s.back() == '\t'))
      s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '"' || s[i] == '\'' || s[i] == '\t'))
      ++i;
    s.erase(0, i);
  };
  trim(text);
  if (text == "~" || text.rfind("~/", 0) == 0 || text.rfind("~\\", 0) == 0)
    text = shell::home() + text.substr(1);
#if defined(_WIN32)
  std::replace(text.begin(), text.end(), '/', '\\');
  if (text.size() == 2 && text[1] == ':')
    text += '\\';
  while (text.size() > 3 && text.back() == '\\')
    text.pop_back();
#else
  while (text.size() > 1 && text.back() == '/')
    text.pop_back();
#endif
  return text;
}

// ---- the places in the sidebar, which the user arranges and which are kept between runs

std::filesystem::path as_path(const std::string &s)
{
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(s.data()), s.size()));
}

// Where the places are kept; the probe points it elsewhere so it never touches the user's own list.
std::string config_override;

std::string config_dir()
{
  return config_override.empty() ? shell::config_dir() : config_override;
}

std::string places_file()
{
  const auto dir = config_dir();
  return dir.empty() ? std::string() : shell::join(dir, "places");
}

// One place a line: its kind, a tab, its name, a tab, its path. The system's list until the user has
// changed it.
std::vector<shell::place_t> load_places()
{
  const auto file = places_file();
  std::ifstream in(file.empty() ? std::filesystem::path() : as_path(file));
  if (!in)
    return shell::places();
  std::vector<shell::place_t> out;
  for (std::string line; std::getline(in, line);)
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    const auto a = line.find('\t');
    const auto b = a == std::string::npos ? a : line.find('\t', a + 1);
    if (b == std::string::npos)
      continue;
    shell::place_t p;
    p.kind = line.compare(0, a, "home") == 0 ? shell::place_t::kind_t::home : shell::place_t::kind_t::folder;
    p.name = line.substr(a + 1, b - a - 1);
    p.path = line.substr(b + 1);
    out.push_back(std::move(p));
  }
  return out;
}

// Written whole to a file beside it and renamed over it, so a crash never leaves half a list.
std::expected<void, std::string> save_places(const std::vector<shell::place_t> &places)
{
  const auto file = places_file();
  if (file.empty())
    return std::unexpected(std::string("no folder for settings"));
  std::error_code error;
  std::filesystem::create_directories(as_path(config_dir()), error);
  const auto temporary = file + ".new";
  {
    std::ofstream out(as_path(temporary), std::ios::binary | std::ios::trunc);
    out << "# vui explorer places: kind, name and path, separated by tabs\n";
    for (const auto &p : places)
      out << (p.kind == shell::place_t::kind_t::home ? "home" : "folder") << '\t' << p.name << '\t' << p.path << '\n';
    if (!out)
      return std::unexpected("cannot write " + temporary);
  }
  std::filesystem::rename(as_path(temporary), as_path(file), error);
  if (error)
    return std::unexpected("cannot save the places: " + error.message());
  return {};
}

bool same_path(const std::string &a, const std::string &b)
{
#if defined(_WIN32)
  return fold(a) == fold(b);
#else
  return a == b;
#endif
}

// A name without its archive suffix, compressed tars as one: "logs.tar.gz" -> "logs". A folder's or
// a plain file's name is kept whole but for its extension.
std::string archive_stem(const std::string &name)
{
  const auto lowered = fold(name);
  for (const char *s : {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst"})
  {
    const std::string suffix = s;
    if (lowered.size() > suffix.size() && lowered.compare(lowered.size() - suffix.size(), suffix.size(), suffix) == 0)
      return name.substr(0, name.size() - suffix.size());
  }
  const auto dot = name.rfind('.');
  return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

// `base` + `suffix` in `folder`, or "base (2)" and on, whichever is not taken yet.
std::string free_name(const std::string &folder, const std::string &base, const std::string &suffix)
{
  std::error_code error;
  for (int n = 1; n < 1000; ++n)
  {
    const auto name = n == 1 ? base + suffix : base + " (" + std::to_string(n) + ")" + suffix;
    if (!std::filesystem::exists(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(shell::join(folder, name).data()), shell::join(folder, name).size())), error))
      return name;
  }
  return base + suffix;
}

// ---- icons, as paths in the unit square

std::shared_ptr<const ui::path_t> make_path(const std::function<void(ui::path_t &)> &draw)
{
  auto p = std::make_shared<ui::path_t>();
  draw(*p);
  return p;
}

// A ring from `from` to `to` degrees, as a closed band: what a refresh arrow is made of.
void arc(ui::path_t &p, float cx, float cy, float r_out, float r_in, float from, float to)
{
  constexpr int steps = 24;
  const auto at = [&](float r, float deg, bool first)
  {
    const float a = deg * 3.14159265f / 180;
    if (first)
      p.move_to(cx + r * std::cos(a), cy + r * std::sin(a));
    else
      p.line_to(cx + r * std::cos(a), cy + r * std::sin(a));
  };
  for (int i = 0; i <= steps; ++i)
    at(r_out, from + (to - from) * float(i) / steps, i == 0);
  for (int i = steps; i >= 0; --i)
    at(r_in, from + (to - from) * float(i) / steps, false);
  p.close();
}

struct icons_t
{
  std::shared_ptr<const ui::path_t> back = make_path([](ui::path_t &p) { p.move_to(0.62f, 0.18f).line_to(0.72f, 0.28f).line_to(0.5f, 0.5f).line_to(0.72f, 0.72f).line_to(0.62f, 0.82f).line_to(0.3f, 0.5f).close(); });
  std::shared_ptr<const ui::path_t> forward = make_path([](ui::path_t &p) { p.move_to(0.38f, 0.18f).line_to(0.7f, 0.5f).line_to(0.38f, 0.82f).line_to(0.28f, 0.72f).line_to(0.5f, 0.5f).line_to(0.28f, 0.28f).close(); });
  std::shared_ptr<const ui::path_t> up = make_path([](ui::path_t &p) { p.move_to(0.5f, 0.16f).line_to(0.82f, 0.48f).line_to(0.72f, 0.58f).line_to(0.57f, 0.43f).line_to(0.57f, 0.84f).line_to(0.43f, 0.84f).line_to(0.43f, 0.43f).line_to(0.28f, 0.58f).line_to(0.18f, 0.48f).close(); });
  std::shared_ptr<const ui::path_t> home = make_path(
    [](ui::path_t &p)
    {
      p.even_odd = true;
      p.move_to(0.5f, 0.14f).line_to(0.88f, 0.48f).line_to(0.78f, 0.48f).line_to(0.78f, 0.86f).line_to(0.22f, 0.86f).line_to(0.22f, 0.48f).line_to(0.12f, 0.48f).close();
      p.move_to(0.42f, 0.86f).line_to(0.42f, 0.62f).line_to(0.58f, 0.62f).line_to(0.58f, 0.86f).close();
    });
  std::shared_ptr<const ui::path_t> refresh = make_path(
    [](ui::path_t &p)
    {
      arc(p, 0.5f, 0.5f, 0.34f, 0.22f, -30, 250);
      p.move_to(0.79f, 0.12f).line_to(0.84f, 0.46f).line_to(0.52f, 0.36f).close();
    });
  std::shared_ptr<const ui::path_t> folder = make_path([](ui::path_t &p) { p.move_to(0.08f, 0.24f).line_to(0.4f, 0.24f).line_to(0.48f, 0.33f).line_to(0.92f, 0.33f).line_to(0.92f, 0.82f).line_to(0.08f, 0.82f).close(); });
  std::shared_ptr<const ui::path_t> file = make_path(
    [](ui::path_t &p)
    {
      p.even_odd = true;
      p.move_to(0.2f, 0.1f).line_to(0.6f, 0.1f).line_to(0.82f, 0.32f).line_to(0.82f, 0.9f).line_to(0.2f, 0.9f).close();
      p.move_to(0.58f, 0.14f).line_to(0.58f, 0.34f).line_to(0.78f, 0.34f).close();
    });
  std::shared_ptr<const ui::path_t> drive = make_path(
    [](ui::path_t &p)
    {
      p.even_odd = true;
      p.move_to(0.1f, 0.32f).line_to(0.9f, 0.32f).line_to(0.9f, 0.72f).line_to(0.1f, 0.72f).close();
      p.move_to(0.7f, 0.48f).line_to(0.78f, 0.48f).line_to(0.78f, 0.56f).line_to(0.7f, 0.56f).close();
    });
  std::shared_ptr<const ui::path_t> chevron = make_path([](ui::path_t &p) { p.move_to(0.36f, 0.2f).line_to(0.66f, 0.5f).line_to(0.36f, 0.8f).line_to(0.28f, 0.72f).line_to(0.5f, 0.5f).line_to(0.28f, 0.28f).close(); });
  std::shared_ptr<const ui::path_t> sort_up = make_path([](ui::path_t &p) { p.move_to(0.14f, 0.64f).line_to(0.5f, 0.28f).line_to(0.86f, 0.64f).line_to(0.75f, 0.75f).line_to(0.5f, 0.5f).line_to(0.25f, 0.75f).close(); });
  std::shared_ptr<const ui::path_t> sort_down = make_path([](ui::path_t &p) { p.move_to(0.14f, 0.36f).line_to(0.25f, 0.25f).line_to(0.5f, 0.5f).line_to(0.75f, 0.25f).line_to(0.86f, 0.36f).line_to(0.5f, 0.72f).close(); });
};

const icons_t &icons()
{
  static const icons_t i;
  return i;
}

// ---- workers: a thread with a queue of jobs

using apply_t = std::function<void(ui::scope_t &)>;
using pipe_t = vio::event_pipe_t<apply_t>;

class worker_t
{
public:
  explicit worker_t(bool com)
    : _thread([this, com] { run(com); })
  {
  }
  worker_t(const worker_t &) = delete;
  worker_t &operator=(const worker_t &) = delete;
  ~worker_t()
  {
    {
      std::lock_guard lock(_mutex);
      _stop = true;
      _jobs.clear();
    }
    _wake.notify_all();
    _thread.join();
  }

  void post(std::function<void()> job)
  {
    {
      std::lock_guard lock(_mutex);
      _jobs.push_back(std::move(job));
    }
    _wake.notify_one();
  }
  // Drops whatever is waiting, then queues this: only the latest request matters.
  void replace(std::function<void()> job)
  {
    {
      std::lock_guard lock(_mutex);
      _jobs.clear();
      _jobs.push_back(std::move(job));
    }
    _wake.notify_one();
  }

private:
  void run(bool com)
  {
    if (com)
      shell::initialize();
    std::unique_lock lock(_mutex);
    while (true)
    {
      _wake.wait(lock, [this] { return _stop || !_jobs.empty(); });
      if (_stop)
        return;
      auto job = std::move(_jobs.front());
      _jobs.pop_front();
      lock.unlock();
      job();
      lock.lock();
    }
  }

  std::mutex _mutex;
  std::condition_variable _wake;
  std::deque<std::function<void()>> _jobs;
  bool _stop = false;
  std::thread _thread;
};

// ---- the file table

struct table_context_t
{
  // -1 when the press was on no row.
  long index = -1;
  vui::point_t position;
};

class file_table_t
{
public:
  struct props_t
  {
    std::string filter;
    bool show_hidden = false;
    // Bumped when names learned for types change what the Type column shows.
    std::uint64_t types_version = 0;
    // A menu is open over it and has the keys.
    bool passive = false;
    friend bool operator==(const props_t &, const props_t &) = default;
  };
  props_t props;
  ui::event_source_t<long> selected;
  ui::event_source_t<long> activated;
  ui::event_source_t<table_context_t> context;
  // Extensions the visible rows need a name for, which the parent should look up.
  std::function<void(const std::string &)> want_type;
  const std::unordered_map<std::string, std::string> *types = nullptr;

  [[nodiscard]] const std::vector<shell::entry_t> &entries() const noexcept
  {
    return _entries;
  }
  [[nodiscard]] long selection() const noexcept
  {
    return _selected;
  }
  [[nodiscard]] const shell::entry_t *selected_entry() const noexcept
  {
    return _selected >= 0 && std::size_t(_selected) < _entries.size() ? &_entries[std::size_t(_selected)] : nullptr;
  }
  [[nodiscard]] std::size_t shown() const
  {
    return order().size();
  }
  [[nodiscard]] std::chrono::nanoseconds sort_cost() const noexcept
  {
    return _sort_cost;
  }
  [[nodiscard]] ui::pressable_t &body() noexcept
  {
    return _body;
  }
  [[nodiscard]] ui::rect_t body_bounds() const noexcept
  {
    return _body.bounds();
  }

  // A new folder: what is known of it so far, nothing selected, at the top. `select` names what to
  // select once it appears: the folder just left, when going up.
  void reset(ui::scope_t &scope, std::vector<shell::entry_t> entries, std::string select = {})
  {
    _entries = std::move(entries);
    refold();
    _selected = -1;
    _scroll = 0;
    _pending_select = std::move(select);
    take_pending_select(scope);
    dirty();
    ui::invalidate(scope, *this);
  }
  void append(ui::scope_t &scope, std::vector<shell::entry_t> &&batch)
  {
    _entries.insert(_entries.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
    for (std::size_t i = _folded.size(); i < _entries.size(); ++i)
      _folded.push_back(fold(_entries[i].name));
    take_pending_select(scope);
    // Not dirty: order() merges the new rows into what it has.
    ui::invalidate(scope, *this);
  }
  // A row that has gone -- moved to the Recycle Bin -- leaves the selection on the row that took its
  // place, as Explorer does.
  void remove(ui::scope_t &scope, const std::string &name)
  {
    long at = -1;
    for (std::size_t i = 0; i < _entries.size(); ++i)
    {
      if (_entries[i].name == name)
        at = long(i);
    }
    if (at < 0)
      return;
    long position = 0;
    const auto &o = order();
    for (std::size_t i = 0; i < o.size(); ++i)
    {
      if (long(o[i]) == at)
        position = long(i);
    }
    _entries.erase(_entries.begin() + at);
    _folded.erase(_folded.begin() + at);
    dirty();
    const auto &after = order();
    _selected = after.empty() ? -1 : long(after[std::size_t(std::min(position, long(after.size()) - 1))]);
    ui::emit(selected, _selected, scope);
    ui::invalidate(scope, *this);
  }

  // A fresh listing of the folder already shown: keep the selection by name, and the scroll.
  void replace(ui::scope_t &scope, std::vector<shell::entry_t> entries)
  {
    const std::string keep = selected_entry() ? selected_entry()->name : std::string();
    _entries = std::move(entries);
    refold();
    _selected = -1;
    if (!keep.empty())
    {
      for (std::size_t i = 0; i < _entries.size(); ++i)
      {
        if (_entries[i].name == keep)
          _selected = long(i);
      }
    }
    dirty();
    ui::invalidate(scope, *this);
  }

  ui::node_t view() const
  {
    const auto &t = ui::theme();
    const auto &o = order();
    ui::row_t header{.align = ui::align_t::center};
    for (std::size_t c = 0; c < columns.size(); ++c)
    {
      ui::row_t label{.gap = 4, .align = ui::align_t::center, .children = {text(columns[c].title, 12, int(c) == _sort ? t.text : t.text_muted)}};
      if (int(c) == _sort)
        label.children.push_back(ui::icon_t{.path = _descending ? icons().sort_down : icons().sort_up, .size = 10, .color = t.text});
      header.children.push_back(ui::box_t{.size = {columns[c].width, 28},
                                          .color = _headers[c].hovered() ? raised : clear,
                                          .radius = 4,
                                          .events = _headers[c],
                                          .grow = columns[c].width == 0 ? 1.0f : 0.0f,
                                          .padding = {10, 0},
                                          .align_x = columns[c].right ? ui::align_t::end : ui::align_t::start,
                                          .children = {std::move(label)}});
    }

    const auto bounds = _body.bounds();
    const float height = std::max(float(bounds.height), row_height);
    const float width = float(std::max(bounds.width, 200));
    const float top = clamped(height);
    const auto first = std::size_t(top / row_height);
    const auto last = std::min(o.size(), first + std::size_t(height / row_height) + 2);
    ui::box_t body{.size = {300, 200},
                   .color = well,
                   .radius = 6,
                   .events = _body,
                   .grow = 1,
                   .align_x = ui::align_t::start,
                   .align_y = ui::align_t::start,
                   .clip = true};
    for (std::size_t i = first; i < last; ++i)
    {
      const auto index = o[i];
      const auto &e = _entries[index];
      const bool chosen = long(index) == _selected;
      ui::row_t cells{.align = ui::align_t::center};
      for (std::size_t c = 0; c < columns.size(); ++c)
        cells.children.push_back(cell(c, e));
      body.children.push_back(ui::at_t{.x = 0,
                                       .y = float(i) * row_height - top,
                                       .children = {ui::box_t{.size = {int(width), int(row_height)},
                                                              .color = chosen ? faded(t.accent, _body.focused() ? 0.42f : 0.25f) : (int(i) == _hovered ? ui::color_t{1, 1, 1, 0.045f} : clear),
                                                              .align_x = ui::align_t::stretch,
                                                              .children = {std::move(cells)}}}});
    }
    if (o.empty())
      body.children.push_back(ui::at_t{.x = 16, .y = 14, .children = {muted(_entries.empty() ? "This folder is empty." : (props.filter.empty() ? "Only hidden items here: tick Hidden to see them." : "Nothing here matches the filter."), 13)}});
    const float content = float(o.size()) * row_height;
    if (content > height && bounds.height > 0)
    {
      const float thumb = std::max(24.0f, height * height / content);
      const float y = top / std::max(limit(height), 1.0f) * (height - thumb);
      body.children.push_back(ui::at_t{.x = width - 9, .y = y, .children = {ui::box_t{.size = {6, int(thumb)}, .color = _thumb.held() || _thumb.hovered() ? t.border_hovered : t.scrollbar, .radius = 3, .events = _thumb}}});
    }
    return ui::column_t{.gap = 4, .align = ui::align_t::stretch, .children = {std::move(header), std::move(body)}};
  }

  ui::behaviour_t behave(ui::scope_t &scope)
  {
    auto heads = ui::subscribe<ui::press_event_t>(_headers);
    auto keys = ui::subscribe<ui::typing_event_t>(_body);
    auto drags = ui::subscribe<ui::drag_event_t>(_body, _thumb);
    auto wheels = ui::subscribe<ui::wheel_event_t>(_body, _thumb);
    auto hovers = ui::subscribe<ui::hover_event_t>(_body);
    auto menus = ui::subscribe<ui::context_event_t>(_body);
    auto events = ui::merge(scope, heads, keys, drags, wheels, hovers, menus);
    while (auto event = co_await events)
    {
      const float height = std::max(float(_body.bounds().height), row_height);
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
          _descending = c == 1 || c == 3;
        }
        dirty();
        break;
      }
      case 1:
        typed(std::get<1>(*event).input, scope, height);
        break;
      case 2:
      {
        const auto &drag = std::get<2>(*event);
        if (drag.source == &_thumb)
        {
          const float content = float(order().size()) * row_height;
          const float thumb = std::max(24.0f, height * height / std::max(content, 1.0f));
          if (drag.phase == ui::drag_phase_t::begin)
            _grab = float(drag.position.y) - float(_body.bounds().y) - clamped(height) / std::max(limit(height), 1.0f) * (height - thumb);
          else if (drag.phase == ui::drag_phase_t::move && height > thumb)
            _scroll = (float(drag.position.y) - float(_body.bounds().y) - _grab) / (height - thumb) * limit(height);
          break;
        }
        if (drag.phase != ui::drag_phase_t::begin)
          break;
        const long at = row_at(drag.position.y);
        if (at >= 0 && std::size_t(at) < order().size())
        {
          select_at(std::size_t(at), scope);
          if (drag.clicks == 2)
            ui::emit(activated, _selected, scope);
        }
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
      case 5:
      {
        const auto &menu = std::get<5>(*event);
        const long at = row_at(menu.position.y);
        long index = -1;
        if (at >= 0 && std::size_t(at) < order().size())
        {
          select_at(std::size_t(at), scope);
          index = _selected;
        }
        ui::emit(context, table_context_t{index, menu.position}, scope);
        break;
      }
      }
      _scroll = clamped(height);
    }
  }

  // Where the selected row is on screen, for a menu opened from the keyboard.
  [[nodiscard]] vui::point_t selected_point() const
  {
    const auto b = _body.bounds();
    const auto &o = order();
    for (std::size_t i = 0; i < o.size(); ++i)
    {
      if (long(o[i]) == _selected)
        return {double(b.x + 40), double(b.y) + double(float(i) * row_height - clamped(float(b.height)) + row_height)};
    }
    return {double(b.x + 40), double(b.y + 20)};
  }

private:
  struct column_t
  {
    const char *title;
    int width;
    bool right;
  };
  static constexpr float row_height = 26;
  inline static const std::array<column_t, 4> columns{{{"Name", 0, false}, {"Date modified", 136, false}, {"Type", 140, false}, {"Size", 84, true}}};

  void dirty()
  {
    _order_valid = false;
  }
  void refold()
  {
    _folded.clear();
    _folded.reserve(_entries.size());
    for (const auto &e : _entries)
      _folded.push_back(fold(e.name));
  }
  [[nodiscard]] float limit(float height) const
  {
    return std::max(0.0f, float(order().size()) * row_height - height);
  }
  [[nodiscard]] float clamped(float height) const
  {
    return std::clamp(_scroll, 0.0f, limit(height));
  }

  [[nodiscard]] std::string type_of(const shell::entry_t &e) const
  {
    if (e.directory)
      return "File folder";
    const auto ext = extension_of(e.name);
    if (types)
    {
      if (auto it = types->find(ext); it != types->end())
        return it->second;
    }
    if (want_type)
      want_type(ext);
    return ext.empty() ? "File" : ext.substr(1) + " file";
  }

  ui::node_t cell(std::size_t c, const shell::entry_t &e) const
  {
    const auto &t = ui::theme();
    const auto &column = columns[c];
    ui::box_t box{.size = {column.width, int(row_height)}, .color = clear, .grow = column.width == 0 ? 1.0f : 0.0f, .padding = {10, 0}, .align_x = column.right ? ui::align_t::end : ui::align_t::start};
    const auto ink = e.hidden ? t.text_muted : t.text;
    switch (c)
    {
    case 0:
      box.clip = true;
      box.size.width = 140;
      box.children.push_back(ui::row_t{.gap = 8,
                                       .align = ui::align_t::center,
                                       .children = {ui::icon_t{.path = e.directory ? icons().folder : icons().file, .size = 16, .color = faded(e.directory ? folder_color : file_color, e.hidden ? 0.5f : 1.0f)},
                                                    text(e.name, 13, ink)}});
      break;
    case 1:
      box.children.push_back(text(when(e.modified), 12, t.text_muted));
      break;
    case 2:
      box.clip = true;
      box.children.push_back(text(type_of(e), 12, t.text_muted));
      break;
    default:
      if (!e.directory)
        box.children.push_back(text(bytes(e.size), 12, t.text_muted, true));
      break;
    }
    return box;
  }

  // Filtered, then sorted with folders first, and kept. Rows that arrive later are filtered, sorted
  // among themselves and merged in, so a folder read in nine batches is not sorted nine times.
  const std::vector<std::size_t> &order() const
  {
    const bool same = _order_valid && _cached_filter == props.filter && _cached_hidden == props.show_hidden && (_sort != 2 || _cached_types == props.types_version);
    if (same && _ordered == _entries.size())
      return _order;
    const auto started = clock_t_::now();
    if (!same)
    {
      _cached_filter = props.filter;
      _cached_hidden = props.show_hidden;
      _cached_types = props.types_version;
      _order_valid = true;
      _order.clear();
      _ordered = 0;
    }
    const auto needle = fold(props.filter);
    // Type names, looked up once per extension rather than once per comparison.
    std::unordered_map<std::string, std::string> type_names;
    if (_sort == 2)
    {
      for (std::size_t i = 0; i < _entries.size(); ++i)
      {
        if (!_entries[i].directory)
          type_names.try_emplace(extension_of(_entries[i].name), std::string());
      }
      for (auto &[ext, name] : type_names)
        name = fold(types && types->contains(ext) ? types->at(ext) : ext);
    }
    const auto less = [&](std::size_t a, std::size_t b)
    {
      const auto &x = _entries[a];
      const auto &y = _entries[b];
      if (x.directory != y.directory)
        return x.directory;
      int c = 0;
      switch (_sort)
      {
      case 0:
        c = natural_folded(_folded[a], _folded[b]);
        return _descending ? c > 0 : c < 0;
      case 1:
        c = x.modified < y.modified ? -1 : (x.modified > y.modified ? 1 : 0);
        break;
      case 2:
        if (!x.directory)
          c = natural_folded(type_names[extension_of(x.name)], type_names[extension_of(y.name)]);
        break;
      default:
        c = x.size < y.size ? -1 : (x.size > y.size ? 1 : 0);
        break;
      }
      if (c == 0)
        return natural_folded(_folded[a], _folded[b]) < 0;
      return _descending ? c > 0 : c < 0;
    };
    const auto middle = _order.size();
    for (std::size_t i = _ordered; i < _entries.size(); ++i)
    {
      if (_entries[i].hidden && !props.show_hidden)
        continue;
      if (!needle.empty() && _folded[i].find(needle) == std::string::npos)
        continue;
      _order.push_back(i);
    }
    std::sort(_order.begin() + std::ptrdiff_t(middle), _order.end(), less);
    std::inplace_merge(_order.begin(), _order.begin() + std::ptrdiff_t(middle), _order.end(), less);
    _ordered = _entries.size();
    _sort_cost = clock_t_::now() - started;
    return _order;
  }

  [[nodiscard]] long position_of_selected() const
  {
    const auto &o = order();
    for (std::size_t i = 0; i < o.size(); ++i)
    {
      if (long(o[i]) == _selected)
        return long(i);
    }
    return -1;
  }

  void select_at(std::size_t position, ui::scope_t &scope)
  {
    const long index = long(order()[position]);
    if (index == _selected)
      return;
    _selected = index;
    ui::emit(selected, _selected, scope);
  }

  void take_pending_select(ui::scope_t &scope)
  {
    if (_pending_select.empty())
      return;
    for (std::size_t i = 0; i < _entries.size(); ++i)
    {
      if (_entries[i].name == _pending_select)
      {
        _selected = long(i);
        _pending_select.clear();
        ui::emit(selected, _selected, scope);
        return;
      }
    }
  }

  void keep_in_view(long position, float height)
  {
    const float y = float(position) * row_height;
    if (y < _scroll)
      _scroll = y;
    if (y + row_height > _scroll + height)
      _scroll = y + row_height - height;
  }

  void typed(const std::variant<vui::key_event_t, vui::text_event_t> &input, ui::scope_t &scope, float height)
  {
    if (props.passive)
      return;
    const auto &o = order();
    if (input.index() == 1)
    {
      // Typing jumps to the next name that starts with what was typed lately.
      const auto now = clock_t_::now();
      if (now - _typed_at > 900ms)
        _typed.clear();
      _typed_at = now;
      _typed += fold(std::get<1>(input).text);
      const long from = std::max(position_of_selected(), 0L);
      for (std::size_t k = 0; k < o.size(); ++k)
      {
        const auto position = (std::size_t(from) + (_typed.size() == 1 ? k + 1 : k)) % o.size();
        if (_folded[o[position]].rfind(_typed, 0) == 0)
        {
          select_at(position, scope);
          keep_in_view(long(position), height);
          break;
        }
      }
      return;
    }
    const auto &key = std::get<0>(input);
    if (!key.pressed || o.empty())
      return;
    const long n = long(o.size());
    long at = position_of_selected();
    const long page = std::max(1L, long(height / row_height) - 1);
    switch (key.key)
    {
    case vui::key_t::down:
      at = at < 0 ? 0 : std::min(at + 1, n - 1);
      break;
    case vui::key_t::up:
      at = std::max(at - 1, 0L);
      break;
    case vui::key_t::page_down:
      at = std::min(std::max(at, 0L) + page, n - 1);
      break;
    case vui::key_t::page_up:
      at = std::max(at - page, 0L);
      break;
    case vui::key_t::home:
      at = 0;
      break;
    case vui::key_t::end:
      at = n - 1;
      break;
    case vui::key_t::enter:
    case vui::key_t::keypad_enter:
      if (_selected >= 0)
        ui::emit(activated, _selected, scope);
      return;
    default:
      return;
    }
    select_at(std::size_t(at), scope);
    keep_in_view(at, height);
  }

  std::vector<shell::entry_t> _entries;
  std::array<ui::pressable_t, 4> _headers;
  ui::pressable_t _body{ui::text_input};
  ui::pressable_t _thumb;
  int _sort = 0;
  bool _descending = false;
  float _scroll = 0;
  float _grab = 0;
  int _hovered = -1;
  long _selected = -1;
  std::string _pending_select;
  std::string _typed;
  clock_t_::time_point _typed_at;

  std::vector<std::string> _folded;
  mutable std::vector<std::size_t> _order;
  mutable std::size_t _ordered = 0;
  mutable bool _order_valid = false;
  mutable std::string _cached_filter;
  mutable bool _cached_hidden = false;
  mutable std::uint64_t _cached_types = 0;
  mutable std::chrono::nanoseconds _sort_cost{0};
};

// ---- "are you sure": a small dialog over everything, answered with a button, Enter or Escape

class confirm_t
{
public:
  struct props_t
  {
    std::string title;
    std::string message;
    std::string confirm;
    // What cannot be undone says so in red.
    bool danger = false;
    friend bool operator==(const props_t &, const props_t &) = default;
  };
  props_t props;
  ui::event_source_t<bool> answered;
  // Everywhere outside the dialog: a press there is No.
  ui::pressable_t dismiss;

  ui::node_t view() const
  {
    const auto &t = ui::theme();
    return ui::box_t{.size = {400, 0},
                     .color = t.menu,
                     .radius = 10,
                     .border_width = 1,
                     .border_color = t.border,
                     .shadow = {.x = 0, .y = 8, .blur = 24, .color = {0, 0, 0, 0.6f}},
                     .padding = 20,
                     .align_x = ui::align_t::stretch,
                     .children = {ui::column_t{.gap = 12,
                                               .align = ui::align_t::stretch,
                                               .children = {text(props.title, 16, t.text), ui::label({.text = props.message, .muted = !props.danger, .size = 13, .color = props.danger ? error_color : ui::color_t{0, 0, 0, 0}}),
                                                            ui::row_t{.gap = 10,
                                                                      .children = {ui::spacer_t{}, ui::mount(_no, {.label = "Cancel"}), ui::mount(_yes, {.label = props.confirm, .primary = true})}}}}}};
  }

  ui::behaviour_t behave(ui::scope_t &scope)
  {
    auto presses = ui::subscribe<ui::press_event_t>(_yes.press, _no.press);
    while (auto press = co_await presses)
      ui::emit(answered, press->source == &_yes.press, scope);
  }

private:
  ui::button_t _yes, _no;
};

// ---- the window

enum class action_t
{
  open,
  open_with,
  copy_path,
  copy_path_native,
  edit_path,
  extract_here,
  extract_to,
  open_in_archiver,
  compress,
  add_place,
  remove_place,
  place_up,
  place_down,
  reset_places,
  reveal,
  terminal,
  properties,
  trash,
  remove,
  refresh,
};

struct explorer_t
{
  explicit explorer_t(std::string start)
    : start_at(std::move(start))
    , places(load_places())
    , drives(shell::drives())
  {
  }

  std::string start_at;

  // Where we are.
  std::string path;
  std::vector<std::string> back_stack, forward_stack;
  std::string filter;
  bool show_hidden = false;

  // What we know: recent listings, names for types, the applications for each extension.
  std::unordered_map<std::string, std::vector<shell::entry_t>> listings;
  std::deque<std::string> listing_order;
  std::unordered_map<std::string, std::string> types;
  std::unordered_set<std::string> types_asked;
  std::uint64_t types_version = 0;
  std::unordered_map<std::string, std::vector<shell::handler_t>> handlers;
  std::unordered_set<std::string> handlers_asked;

  // The listing in flight.
  std::shared_ptr<std::atomic<std::uint64_t>> generation = std::make_shared<std::atomic<std::uint64_t>>(0);
  bool loading = false;
  clock_t_::time_point asked_at;
  std::chrono::nanoseconds first_rows{0};
  std::chrono::nanoseconds listed_in{0};
  bool from_cache = false;
  // The first folder is asked for while the window and its renderer are still starting, so its time
  // to first rows measures start-up rather than the folder.
  std::size_t navigations = 0;
  std::string status;
  bool status_error = false;

  // The preview of the selection.
  std::uint64_t preview_generation = 0;
  std::string preview_note;
  // The selection's thumbnail, when the system has one (G57), and the last few kept by path and
  // modification time, so going back to a file shows it at once.
  std::shared_ptr<const ui::image_data_t> preview_image;
  std::unordered_map<std::string, std::shared_ptr<const ui::image_data_t>> thumbnails;
  std::deque<std::string> thumbnail_order;
  // A PDF's pages (G61): which file the preview shows, how many pages it has once that is known, and
  // which one is drawn. Its pages share the thumbnail cache, keyed by page.
  std::string preview_file;
  std::string preview_key;
  int pdf_pages = 0;
  int pdf_page = 0;
  ui::pressable_t pdf_previous, pdf_next;

  // The path bar while it is a field: what is typed in it, and whether it has had focus yet -- losing
  // focus after that puts the breadcrumbs back. Mutable because the view is where focus is seen.
  mutable bool editing_path = false;
  mutable bool path_had_focus = false;
  std::string typing;
  // The menu is for the path bar rather than a row or the empty list; or for a place in the sidebar,
  // by its index among the places and then the drives.
  bool menu_for_path = false;
  std::optional<std::size_t> menu_place;

  // What is waiting for "are you sure" before it goes to the Recycle Bin, or for good.
  std::optional<shell::entry_t> confirming;
  bool confirming_permanent = false;

  // The context menu, while open: where, for what, and what each item does.
  std::optional<vui::point_t> menu_at;
  std::optional<shell::entry_t> menu_entry;
  std::vector<std::pair<action_t, std::size_t>> menu_actions;

  std::vector<shell::place_t> places;
  std::vector<shell::place_t> drives;

  // Widgets.
  ui::pressable_t back_button, forward_button, up_button, home_button, refresh_button;
  std::array<ui::pressable_t, 32> crumb_buttons;
  std::array<ui::pressable_t, 32> place_buttons;
  ui::line_edit_t filter_edit;
  ui::checkbox_t hidden_box;
  file_table_t table;
  ui::text_edit_t preview;
  ui::context_menu_t menu;
  confirm_t confirm;
  ui::pressable_t path_bar;
  // The whole window, for the mouse's back and forward buttons wherever the pointer is.
  ui::pressable_t window_area;
  ui::line_edit_t path_edit;

  pipe_t *pipe = nullptr;
  worker_t *lister = nullptr;
  worker_t *helper = nullptr;
  // Archive work, which can take minutes, on a thread of its own so previews and thumbnails go on.
  worker_t *jobs = nullptr;
  // What the archive items of the open menu do: which tool, and the archive suffix to write.
  struct archive_offer_t
  {
    std::size_t tool = 0;
    std::string suffix;
  };
  std::vector<archive_offer_t> archive_offers;

  // ---- navigation

  void remember(const std::string &folder, std::vector<shell::entry_t> entries)
  {
    if (!listings.contains(folder))
    {
      listing_order.push_back(folder);
      while (listing_order.size() > 48)
      {
        listings.erase(listing_order.front());
        listing_order.pop_front();
      }
    }
    listings[folder] = std::move(entries);
  }

  void say(std::string message, bool error = false)
  {
    status = std::move(message);
    status_error = error;
  }

  // Show a folder now -- from memory if it was seen lately -- and read it on the lister, whose rows
  // stream in as they are read when there was nothing to show.
  void go(ui::scope_t &scope, std::string folder, bool record = true, std::string select = {})
  {
    if (record && !path.empty() && folder != path)
    {
      back_stack.push_back(path);
      forward_stack.clear();
    }
    path = std::move(folder);
    editing_path = false;
    ++navigations;
    menu_at.reset();
    asked_at = clock_t_::now();
    first_rows = {};
    const auto mine = ++*generation;
    auto cached = listings.find(path);
    from_cache = cached != listings.end();
    table.reset(scope, from_cache ? cached->second : std::vector<shell::entry_t>{}, std::move(select));
    preview_for(scope, -1);
    loading = true;
    read(scope, mine, from_cache);
  }

  void read(ui::scope_t &, std::uint64_t mine, bool quietly)
  {
    auto gen = generation;
    auto *p = pipe;
    const auto folder = path;
    lister->replace(
      [this, gen, p, folder, mine, quietly]
      {
        // Shown already from memory: gather the fresh listing and swap it in whole, so the rows do
        // not empty and refill. Otherwise every batch goes up as it is read.
        auto gathered = std::make_shared<std::vector<shell::entry_t>>();
        const auto result = shell::list(folder,
                                        [&](std::vector<shell::entry_t> &&batch)
                                        {
                                          if (gen->load() != mine)
                                            return false;
                                          if (quietly)
                                          {
                                            gathered->insert(gathered->end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
                                            return true;
                                          }
                                          p->post_event(
                                            [this, mine, rows = std::move(batch)](ui::scope_t &scope) mutable
                                            {
                                              if (generation->load() != mine)
                                                return;
                                              if (first_rows == std::chrono::nanoseconds{0})
                                                first_rows = clock_t_::now() - asked_at;
                                              table.append(scope, std::move(rows));
                                            });
                                          return true;
                                        });
        if (gen->load() != mine)
          return;
        p->post_event(
          [this, mine, quietly, result, gathered](ui::scope_t &scope)
          {
            if (generation->load() != mine)
              return;
            loading = false;
            if (!result)
            {
              say("Cannot open " + path + ": " + result.error(), true);
              return;
            }
            listed_in = *result;
            if (quietly)
              table.replace(scope, *gathered);
            if (first_rows == std::chrono::nanoseconds{0})
              first_rows = clock_t_::now() - asked_at;
            remember(path, table.entries());
            say("");
            scope.invalidate();
          });
      });
  }

  // Extract, open or compress with an archive tool, on the jobs thread; the folder is read again when
  // it is done, with what was made selected.
  void archive_job(ui::scope_t &, action_t action, const archive_offer_t &offer, const shell::entry_t &entry)
  {
    const auto &tool = shell::archivers()[offer.tool];
    const auto item = shell::join(path, entry.name);
    const auto folder = path;
    if (action == action_t::open_in_archiver)
    {
      launch([tool, item] { return shell::open_in_archiver(tool, item); }, "Opened " + entry.name + " in " + tool.name);
      return;
    }
    std::string made;
    std::string doing;
    std::function<std::expected<void, std::string>()> work;
    if (action == action_t::compress)
    {
      made = free_name(folder, archive_stem(entry.name), "." + offer.suffix);
      const auto archive = shell::join(folder, made);
      doing = "Compressing " + entry.name + " to " + made + " with " + tool.name;
      work = [tool, item, archive] { return shell::compress(tool, item, archive); };
    }
    else if (action == action_t::extract_to)
    {
      made = free_name(folder, archive_stem(entry.name), "");
      const auto into = shell::join(folder, made);
      doing = "Extracting " + entry.name + " to " + made + " with " + tool.name;
      work = [tool, item, into] { return shell::extract(tool, item, into); };
    }
    else
    {
      made = entry.name;
      doing = "Extracting " + entry.name + " here with " + tool.name;
      work = [tool, item, folder] { return shell::extract(tool, item, folder); };
    }
    say(doing + "...");
    auto *p = pipe;
    const auto started = clock_t_::now();
    jobs->post(
      [this, p, work, made, folder, doing, started]
      {
        auto r = work();
        const auto took = clock_t_::now() - started;
        p->post_event(
          [this, r, made, folder, doing, took](ui::scope_t &scope)
          {
            if (!r)
            {
              say(r.error(), true);
              scope.invalidate();
              return;
            }
            say(doing.substr(0, doing.find(" with ")) + ": done in " + ms(std::chrono::duration_cast<std::chrono::nanoseconds>(took)));
            if (path == folder)
              go(scope, folder, false, made);
            scope.invalidate();
          });
      });
  }

  void go_back(ui::scope_t &scope)
  {
    if (back_stack.empty())
      return;
    forward_stack.push_back(path);
    auto to = back_stack.back();
    back_stack.pop_back();
    go(scope, to, false);
  }

  void go_forward(ui::scope_t &scope)
  {
    if (forward_stack.empty())
      return;
    back_stack.push_back(path);
    auto to = forward_stack.back();
    forward_stack.pop_back();
    go(scope, to, false);
  }

  [[nodiscard]] bool is_in_places(const std::string &folder) const
  {
    return std::any_of(places.begin(), places.end(), [&](const shell::place_t &p) { return same_path(p.path, folder); });
  }

  void keep_places()
  {
    if (auto saved = save_places(places); !saved)
      say(saved.error(), true);
  }

  void add_place(const std::string &folder)
  {
    if (is_in_places(folder) || places.size() >= place_buttons.size() - 4)
      return;
    auto name = folder.substr(std::min(folder.size(), folder.find_last_of("\\/") + 1));
    if (name.empty())
      name = folder;
    places.push_back({name, folder, shell::place_t::kind_t::folder});
    keep_places();
    say("Added " + name + " to Places");
  }

  void act_on_place(ui::scope_t &scope, action_t action, std::size_t i)
  {
    const bool is_place = i < places.size();
    const auto chosen = is_place ? places[i] : drives[i - places.size()];
    switch (action)
    {
    case action_t::open:
      go(scope, chosen.path);
      break;
    case action_t::copy_path:
      scope.window().set_clipboard_text(portable(chosen.path));
      say("Copied " + portable(chosen.path));
      break;
    case action_t::add_place:
      add_place(chosen.path);
      break;
    case action_t::remove_place:
      if (is_place)
      {
        places.erase(places.begin() + std::ptrdiff_t(i));
        keep_places();
        say("Removed " + chosen.name + " from Places");
      }
      break;
    case action_t::place_up:
      if (is_place && i > 0)
      {
        std::swap(places[i], places[i - 1]);
        keep_places();
      }
      break;
    case action_t::place_down:
      if (is_place && i + 1 < places.size())
      {
        std::swap(places[i], places[i + 1]);
        keep_places();
      }
      break;
    case action_t::reset_places:
      places = shell::places();
      keep_places();
      say("Places reset to the standard folders");
      break;
    default:
      break;
    }
  }

  // The path bar becomes a field holding the path, all of it selected and focused, so typing or
  // pasting replaces it.
  void edit_path(ui::scope_t &scope)
  {
    menu_at.reset();
    editing_path = true;
    path_had_focus = false;
    typing = path;
    path_edit.select_all();
    ui::request_focus(scope, path_edit.field());
  }

  // Enter in the path bar: a folder is opened, a file opens its folder with it selected.
  void go_to_typed(ui::scope_t &scope)
  {
    editing_path = false;
    const auto target = typed_path(typing);
    if (target.empty())
      return;
    std::error_code error;
    const auto native = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(target.data()), target.size()));
    if (std::filesystem::is_regular_file(native, error))
    {
      const auto folder = shell::parent(target);
      go(scope, folder, true, target.substr(std::min(target.size(), target.find_last_of("\\/") + 1)));
      return;
    }
    go(scope, target);
  }

  void up(ui::scope_t &scope)
  {
    const auto above = shell::parent(path);
    if (above == path)
      return;
    // Selecting the folder we came out of, as Explorer does.
    auto name = path.substr(std::min(path.size(), path.find_last_of("\\/") + 1));
    go(scope, above, true, name);
  }

  // ---- what the helper thread answers

  void want_type(const std::string &ext)
  {
    if (!types_asked.insert(ext).second)
      return;
    auto *p = pipe;
    helper->post(
      [this, p, ext]
      {
        auto name = shell::type_name("x" + ext);
        p->post_event(
          [this, ext, name = std::move(name)](ui::scope_t &scope)
          {
            types[ext] = name;
            ++types_version;
            scope.invalidate();
          });
      });
  }

  void want_handlers(const std::string &file)
  {
    const auto ext = extension_of(file);
    if (ext.empty() || !handlers_asked.insert(ext).second)
      return;
    auto *p = pipe;
    helper->post(
      [this, p, file, ext]
      {
        auto found = shell::handlers(file);
        p->post_event(
          [this, ext, found = std::move(found)](ui::scope_t &scope)
          {
            handlers[ext] = found;
            // An open menu picks the applications up as they arrive.
            if (menu_at && menu_entry && extension_of(menu_entry->name) == ext)
              build_menu(scope);
            scope.invalidate();
          });
      });
  }

  static std::shared_ptr<const ui::image_data_t> image_of(shell::picture_t picture)
  {
    auto data = std::make_shared<ui::image_data_t>();
    data->width = picture.width;
    data->height = picture.height;
    data->rgba = std::move(picture.rgba);
    return data;
  }

  void keep_thumbnail(const std::string &key, const std::shared_ptr<const ui::image_data_t> &data)
  {
    if (!thumbnails.contains(key))
      thumbnail_order.push_back(key);
    thumbnails[key] = data;
    while (thumbnail_order.size() > 64)
    {
      thumbnails.erase(thumbnail_order.front());
      thumbnail_order.pop_front();
    }
  }

  // A page of the PDF the preview shows: from the cache when it was drawn before, else drawn on the
  // helper at about twice the pane's size, so it stays sharp on a high-density screen. When the
  // system has no PDF renderer, its thumbnail is what is left.
  void show_pdf_page(ui::scope_t &scope, int page)
  {
    const auto mine = preview_generation;
    const auto file = preview_file;
    const auto key = preview_key;
    const auto pages_key = key + "|pages";
    pdf_page = page;
    if (auto cached = thumbnails.find(key + "|page " + std::to_string(page)), counted = thumbnails.find(pages_key); cached != thumbnails.end() && counted != thumbnails.end())
    {
      preview_image = cached->second;
      pdf_pages = counted->second->width;
      scope.invalidate();
      return;
    }
    auto *p = pipe;
    helper->post(
      [this, p, file, key, pages_key, page, mine]
      {
        auto drawn = shell::pdf_page(file, page, 720);
        if (!drawn)
        {
          auto picture = shell::thumbnail(file, 512);
          p->post_event(
            [this, mine, key, note = drawn.error(), data = picture ? image_of(std::move(*picture)) : nullptr](ui::scope_t &scope)
            {
              if (data)
                keep_thumbnail(key, data);
              if (preview_generation != mine)
                return;
              preview_image = data;
              if (!data)
                preview_note = note;
              scope.invalidate();
            });
          return;
        }
        auto data = image_of(std::move(drawn->picture));
        // The page count rides in the cache as an empty picture as wide as the count.
        auto count = std::make_shared<ui::image_data_t>();
        count->width = drawn->pages;
        p->post_event(
          [this, mine, page_key = key + "|page " + std::to_string(drawn->page), pages_key, data, count = std::shared_ptr<const ui::image_data_t>(std::move(count)), at = drawn->page](ui::scope_t &scope)
          {
            keep_thumbnail(page_key, data);
            keep_thumbnail(pages_key, count);
            if (preview_generation != mine)
              return;
            preview_image = data;
            pdf_pages = count->width;
            pdf_page = at;
            scope.invalidate();
          });
      });
  }

  void turn_pdf_page(ui::scope_t &scope, int by)
  {
    if (pdf_pages <= 1)
      return;
    const int to = std::clamp(pdf_page + by, 0, pdf_pages - 1);
    if (to != pdf_page)
      show_pdf_page(scope, to);
  }

  void preview_for(ui::scope_t &scope, long index)
  {
    const auto mine = ++preview_generation;
    preview_note.clear();
    preview_image.reset();
    if (index < 0)
    {
      preview.clear(scope);
      return;
    }
    const auto &e = table.entries()[std::size_t(index)];
    if (e.directory)
    {
      preview.clear(scope);
      return;
    }
    want_handlers(shell::join(path, e.name));
    auto *p = pipe;
    const auto file = shell::join(path, e.name);
    // The picture, from the cache or the system's thumbnailer; it wins over the text when there is one.
    // A PDF is drawn by the system's PDF renderer instead, a page at a time.
    const auto key = file + "|" + std::to_string(e.modified);
    preview_file = file;
    preview_key = key;
    pdf_pages = 0;
    pdf_page = 0;
    if (extension_of(e.name) == ".pdf")
      show_pdf_page(scope, 0);
    else if (auto cached = thumbnails.find(key); cached != thumbnails.end())
      preview_image = cached->second;
    else
    {
      helper->post(
        [this, p, file, key, mine]
        {
          auto picture = shell::thumbnail(file, 512);
          if (!picture)
            return;
          auto data = image_of(std::move(*picture));
          p->post_event(
            [this, key, mine, data](ui::scope_t &scope)
            {
              keep_thumbnail(key, data);
              if (preview_generation == mine)
                preview_image = data;
              scope.invalidate();
            });
        });
    }
    helper->post(
      [this, p, file, mine]
      {
        // The start of the file; text if it has no NUL in it and is not obviously binary.
        constexpr std::size_t most = 64 * 1024;
        std::ifstream in(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(file.data()), file.size())), std::ios::binary);
        std::string data(most, '\0');
        in.read(data.data(), std::streamsize(most));
        data.resize(std::size_t(in.gcount()));
        const bool more = !in.eof();
        const bool binary = data.find('\0') != std::string::npos;
        p->post_event(
          [this, mine, data = std::move(data), more, binary](ui::scope_t &scope)
          {
            if (preview_generation != mine)
              return;
            if (binary)
            {
              preview.clear(scope);
              preview_note = "No preview for a binary file.";
            }
            else
            {
              preview.set_text(scope, data);
              preview_note = more ? "The first 64 KB." : "";
            }
            scope.invalidate();
          });
      });
  }

  // ---- launching, on the helper, which has COM on Windows

  void launch(std::function<std::expected<void, std::string>()> what, std::string done)
  {
    auto *p = pipe;
    helper->post(
      [this, p, what = std::move(what), done = std::move(done)]
      {
        auto r = what();
        p->post_event(
          [this, r, done](ui::scope_t &scope)
          {
            if (r)
              say(done);
            else
              say(r.error(), true);
            scope.invalidate();
          });
      });
  }

  void ask_to_trash(const shell::entry_t &entry, bool permanent)
  {
    menu_at.reset();
    confirming = entry;
    confirming_permanent = permanent;
  }

  // On the helper: a big folder takes a while to move, and the window should not wait for it.
  void trash_confirmed(ui::scope_t &)
  {
    if (!confirming)
      return;
    const auto entry = *confirming;
    const bool permanent = confirming_permanent;
    confirming.reset();
    const auto target = shell::join(path, entry.name);
    const auto folder = path;
    say(permanent ? "Deleting " + entry.name + "..." : "Moving " + entry.name + " to the " + shell::trash_name() + "...");
    auto *p = pipe;
    helper->post(
      [this, p, target, folder, entry, permanent]
      {
        auto r = permanent ? shell::remove_permanently(target) : shell::trash(target);
        p->post_event(
          [this, r, folder, entry, permanent](ui::scope_t &scope)
          {
            if (!r)
            {
              say((permanent ? "Could not delete " : "Could not move ") + entry.name + ": " + r.error(), true);
              scope.invalidate();
              return;
            }
            say(permanent ? "Deleted " + entry.name + " permanently" : "Moved " + entry.name + " to the " + shell::trash_name());
            if (auto it = listings.find(folder); it != listings.end())
              std::erase_if(it->second, [&](const shell::entry_t &e) { return e.name == entry.name; });
            if (path == folder)
              table.remove(scope, entry.name);
            scope.invalidate();
          });
      });
  }

  void activate(ui::scope_t &scope, long index)
  {
    if (index < 0)
      return;
    const auto e = table.entries()[std::size_t(index)];
    if (e.directory)
    {
      go(scope, shell::join(path, e.name));
      return;
    }
    const auto file = shell::join(path, e.name);
    launch([file] { return shell::open(file); }, "Opened " + e.name);
  }

  // ---- the context menu

  void open_menu(ui::scope_t &scope, std::optional<shell::entry_t> entry, vui::point_t at, bool for_path = false, std::optional<std::size_t> place = std::nullopt)
  {
    menu_for_path = for_path;
    menu_place = place;
    menu_entry = std::move(entry);
    if (menu_entry && !menu_entry->directory)
      want_handlers(shell::join(path, menu_entry->name));
    menu_at = at;
    build_menu(scope);
  }

  void build_menu(ui::scope_t &)
  {
    menu_actions.clear();
    std::vector<ui::menu_item_t> items;
    const auto add = [&](ui::menu_item_t item, action_t action, std::size_t extra = 0)
    {
      items.push_back(std::move(item));
      menu_actions.emplace_back(action, extra);
    };
    const auto rule = [&]
    {
      items.push_back({.kind = ui::menu_item_t::kind_t::separator});
      menu_actions.emplace_back(action_t::refresh, 0);
    };
    const auto header = [&](std::string label)
    {
      items.push_back({.label = std::move(label), .kind = ui::menu_item_t::kind_t::header});
      menu_actions.emplace_back(action_t::refresh, 0);
    };
    if (menu_place)
    {
      const auto i = *menu_place;
      const bool is_place = i < places.size();
      add({.label = "Open"}, action_t::open);
      rule();
      if (is_place)
      {
        add({.label = "Move up", .enabled = i > 0}, action_t::place_up);
        add({.label = "Move down", .enabled = i + 1 < places.size()}, action_t::place_down);
        add({.label = "Remove from Places"}, action_t::remove_place);
      }
      else
        add({.label = "Add to Places", .enabled = !is_in_places(drives[i - places.size()].path)}, action_t::add_place);
      rule();
      add({.label = "Copy path"}, action_t::copy_path);
      add({.label = "Reset Places to the standard folders"}, action_t::reset_places);
    }
    else if (menu_entry)
    {
      const bool folder = menu_entry->directory;
      const auto ext = extension_of(menu_entry->name);
      const auto found = handlers.find(ext);
      std::string by;
      if (!folder && found != handlers.end() && !found->second.empty() && found->second.front().is_default)
        by = found->second.front().name;
      add({.label = "Open", .detail = folder ? "Enter" : by}, action_t::open);
      if (folder)
        add({.label = "Add to Places", .enabled = !is_in_places(shell::join(path, menu_entry->name))}, action_t::add_place);
      if (!folder && !ext.empty())
      {
        rule();
        header("Open with");
        if (found == handlers.end())
          add({.label = "Looking for applications...", .enabled = false}, action_t::refresh);
        else if (found->second.empty())
          add({.label = "No application is registered for " + ext, .enabled = false}, action_t::refresh);
        else
        {
          for (std::size_t i = 0; i < found->second.size() && items.size() < ui::context_menu_t::max_items - 16; ++i)
            add({.label = found->second[i].name, .detail = found->second[i].is_default ? "default" : ""}, action_t::open_with, i);
        }
      }
      // What the installed archive tools offer (G59): extracting an archive, opening it in a tool's
      // own window, and compressing anything into each format some tool writes.
      archive_offers.clear();
      const auto &tools = shell::archivers();
      bool archive_header = false;
      const auto archive_items = [&] {
        if (!archive_header)
        {
          rule();
          header("Archive");
          archive_header = true;
        }
      };
      if (!folder && shell::is_archive(menu_entry->name))
      {
        for (std::size_t t = 0; t < tools.size(); ++t)
        {
          if (!shell::can_extract(tools[t], menu_entry->name))
            continue;
          archive_items();
          archive_offers.push_back({t, {}});
          add({.label = "Extract here", .detail = tools[t].name}, action_t::extract_here, archive_offers.size() - 1);
          add({.label = "Extract to \"" + archive_stem(menu_entry->name) + "\"", .detail = tools[t].name}, action_t::extract_to, archive_offers.size() - 1);
          break;
        }
        for (std::size_t t = 0; t < tools.size(); ++t)
        {
          if (tools[t].gui.empty())
            continue;
          archive_items();
          archive_offers.push_back({t, {}});
          add({.label = "Open in " + tools[t].name}, action_t::open_in_archiver, archive_offers.size() - 1);
        }
      }
      for (const char *suffix : {"zip", "7z", "rar", "tar.gz"})
      {
        for (std::size_t t = 0; t < tools.size(); ++t)
        {
          if (std::find(tools[t].creates.begin(), tools[t].creates.end(), suffix) == tools[t].creates.end())
            continue;
          archive_items();
          archive_offers.push_back({t, suffix});
          add({.label = "Compress to \"" + archive_stem(menu_entry->name) + "." + suffix + "\"", .detail = tools[t].name}, action_t::compress, archive_offers.size() - 1);
          break;
        }
      }
      rule();
      add({.label = "Copy path", .detail = "Ctrl+Shift+C"}, action_t::copy_path);
#if defined(_WIN32)
      add({.label = "Copy path with backslashes"}, action_t::copy_path_native);
#endif
      add({.label = std::string("Show in ") + shell::file_manager_name()}, action_t::reveal);
      add({.label = "Open terminal here"}, action_t::terminal);
      rule();
      add({.label = "Delete", .detail = "Del"}, action_t::trash);
      add({.label = "Delete permanently", .detail = "Shift+Del"}, action_t::remove);
      if (shell::has_properties())
        add({.label = "Properties", .detail = "Alt+Enter"}, action_t::properties);
    }
    else if (menu_for_path)
    {
      add({.label = "Copy path", .detail = "Ctrl+Shift+C"}, action_t::copy_path);
#if defined(_WIN32)
      add({.label = "Copy path with backslashes"}, action_t::copy_path_native);
#endif
      add({.label = "Edit path", .detail = "double-click"}, action_t::edit_path);
      add({.label = "Add to Places", .enabled = !is_in_places(path)}, action_t::add_place);
      rule();
      add({.label = "Open terminal here"}, action_t::terminal);
      add({.label = std::string("Show in ") + shell::file_manager_name()}, action_t::reveal);
      if (shell::has_properties())
        add({.label = "Properties"}, action_t::properties);
    }
    else
    {
      add({.label = "Refresh", .detail = "F5"}, action_t::refresh);
      rule();
      add({.label = "Copy path"}, action_t::copy_path);
#if defined(_WIN32)
      add({.label = "Copy path with backslashes"}, action_t::copy_path_native);
#endif
      add({.label = std::string("Show in ") + shell::file_manager_name()}, action_t::reveal);
      add({.label = "Open terminal here"}, action_t::terminal);
      if (shell::has_properties())
        add({.label = "Properties"}, action_t::properties);
    }
    menu_items = std::move(items);
  }
  std::vector<ui::menu_item_t> menu_items;

  void act(ui::scope_t &scope, std::size_t index)
  {
    if (index >= menu_actions.size())
      return;
    const auto [action, extra] = menu_actions[index];
    const auto entry = menu_entry;
    const auto place = menu_place;
    menu_at.reset();
    if (place)
    {
      act_on_place(scope, action, *place);
      return;
    }
    const auto target = entry ? shell::join(path, entry->name) : path;
    const auto folder_of = entry && !entry->directory ? path : target;
    switch (action)
    {
    case action_t::open:
      if (entry && entry->directory)
        go(scope, target);
      else if (entry)
        launch([target] { return shell::open(target); }, "Opened " + entry->name);
      break;
    case action_t::open_with:
    {
      const auto &list = handlers[extension_of(entry->name)];
      if (extra < list.size())
      {
        const auto h = list[extra];
        launch([target, h] { return shell::open_with(target, h); }, "Opened " + entry->name + " in " + h.name);
      }
      break;
    }
    case action_t::copy_path:
      scope.window().set_clipboard_text(portable(target));
      say("Copied " + portable(target));
      break;
    case action_t::copy_path_native:
      scope.window().set_clipboard_text(target);
      say("Copied " + target);
      break;
    case action_t::edit_path:
      edit_path(scope);
      break;
    case action_t::add_place:
      add_place(target);
      break;
    case action_t::extract_here:
    case action_t::extract_to:
    case action_t::open_in_archiver:
    case action_t::compress:
      if (entry && extra < archive_offers.size())
        archive_job(scope, action, archive_offers[extra], *entry);
      break;
    case action_t::reveal:
      launch([target] { return shell::reveal(target); }, std::string("Shown in ") + shell::file_manager_name());
      break;
    case action_t::terminal:
      launch([folder_of] { return shell::open_terminal(folder_of); }, "Terminal opened in " + folder_of);
      break;
    case action_t::properties:
      launch([target] { return shell::properties(target); }, "");
      break;
    case action_t::trash:
      if (entry)
        ask_to_trash(*entry, false);
      break;
    case action_t::remove:
      if (entry)
        ask_to_trash(*entry, true);
      break;
    case action_t::refresh:
      go(scope, path, false);
      break;
    }
  }

  // ---- views

  static ui::node_t icon_button(const ui::pressable_t &source, const std::shared_ptr<const ui::path_t> &icon, bool enabled)
  {
    const auto &t = ui::theme();
    return ui::box_t{.size = {34, 32},
                     .color = !enabled ? clear : (source.held() ? t.control_held : (source.hovered() ? t.control_hovered : clear)),
                     .radius = 6,
                     .events = enabled ? ui::event_ref_t(source) : ui::event_ref_t{},
                     .children = {ui::icon_t{.path = icon, .size = 18, .color = enabled ? t.text : t.text_disabled}}};
  }

  // The breadcrumbs, or while editing a field with the path in it. Losing focus once it has had it
  // ends the editing, as a click elsewhere should.
  ui::node_t path_area(ui::row_t trail) const
  {
    if (editing_path)
    {
      if (path_edit.field().focused())
        path_had_focus = true;
      else if (path_had_focus)
        editing_path = false;
    }
    if (editing_path)
      return ui::box_t{.size = {200, 36},
                       .color = clear,
                       .grow = 1,
                       .align_x = ui::align_t::stretch,
                       .align_y = ui::align_t::stretch,
                       .children = {ui::mount(path_edit, {.text = typing, .placeholder = "Type or paste a path, then Enter", .columns = 20})}};
    return ui::box_t{.size = {200, 36},
                     .color = well,
                     .radius = 8,
                     .events = path_bar,
                     .grow = 1,
                     .padding = {6, 0},
                     .align_x = ui::align_t::start,
                     .clip = true,
                     .children = {std::move(trail)}};
  }

  ui::node_t toolbar() const
  {
    const auto &t = ui::theme();
    ui::row_t trail{.gap = 0, .align = ui::align_t::center};
    const auto parts = shell::crumbs(path);
    // The deepest crumbs, when a path is deeper than there are buttons.
    const std::size_t from = parts.size() > crumb_buttons.size() ? parts.size() - crumb_buttons.size() : 0;
    for (std::size_t i = from; i < parts.size(); ++i)
    {
      const auto &b = crumb_buttons[i - from];
      if (i > from)
        trail.children.push_back(ui::icon_t{.path = icons().chevron, .size = 10, .color = t.text_muted});
      const bool last = i + 1 == parts.size();
      trail.children.push_back(ui::box_t{.color = b.hovered() ? t.control_hovered : clear,
                                         .radius = 5,
                                         .events = b,
                                         .padding = {7, 4},
                                         .children = {text(parts[i].name, 13, last ? t.text : t.text_muted)}});
    }
    return ui::row_t{.gap = 4,
                     .align = ui::align_t::center,
                     .children = {icon_button(back_button, icons().back, !back_stack.empty()), icon_button(forward_button, icons().forward, !forward_stack.empty()),
                                  icon_button(up_button, icons().up, shell::parent(path) != path), icon_button(home_button, icons().home, true), icon_button(refresh_button, icons().refresh, true),
                                  ui::box_t{.size = {8, 0}, .color = clear},
                                  path_area(std::move(trail)),
                                  ui::box_t{.size = {8, 0}, .color = clear}, ui::mount(filter_edit, {.text = filter, .placeholder = "Filter this folder", .columns = 18}),
                                  ui::mount(hidden_box, {.label = "Hidden", .checked = show_hidden})}};
  }

  ui::node_t sidebar() const
  {
    const auto &t = ui::theme();
    ui::column_t list{.gap = 2, .align = ui::align_t::stretch};
    std::size_t b = 0;
    const auto entry = [&](const shell::place_t &place)
    {
      if (b >= place_buttons.size())
        return;
      const auto &button = place_buttons[b++];
      const bool here = place.path == path;
      const auto &icon = place.kind == shell::place_t::kind_t::home ? icons().home : (place.kind == shell::place_t::kind_t::drive ? icons().drive : icons().folder);
      const auto color = place.kind == shell::place_t::kind_t::drive ? drive_color : (place.kind == shell::place_t::kind_t::home ? t.text : folder_color);
      ui::column_t inside{.gap = 4, .align = ui::align_t::stretch, .children = {ui::row_t{.gap = 10, .align = ui::align_t::center, .children = {ui::icon_t{.path = icon, .size = 16, .color = color}, text(place.name, 13, t.text)}}}};
      if (place.total > 0)
      {
        // How full the drive is.
        const float used = float(double(place.total - place.free) / double(place.total));
        inside.children.push_back(ui::row_t{.align = ui::align_t::stretch,
                                            .children = {ui::box_t{.size = {0, 4}, .color = used > 0.9f ? error_color : drive_color, .radius = 2, .grow = std::max(used, 0.001f)},
                                                         ui::box_t{.size = {0, 4}, .color = faded(t.text, 0.12f), .radius = 2, .grow = std::max(1 - used, 0.001f)}}});
        inside.children.push_back(muted(bytes(place.free) + " free of " + bytes(place.total), 11));
      }
      list.children.push_back(ui::box_t{.color = here ? faded(t.accent, 0.3f) : (button.hovered() ? t.control_hovered : clear),
                                        .radius = 6,
                                        .events = button,
                                        .padding = {10, 6},
                                        .align_x = ui::align_t::stretch,
                                        .children = {std::move(inside)}});
    };
    list.children.push_back(muted("Places", 11));
    for (const auto &p : places)
      entry(p);
    list.children.push_back(ui::box_t{.size = {0, 10}, .color = clear});
    list.children.push_back(muted("Drives", 11));
    for (const auto &d : drives)
      entry(d);
    return ui::box_t{.size = {200, 0}, .color = panel, .radius = 10, .padding = 10, .align_x = ui::align_t::stretch, .align_y = ui::align_t::start, .children = {std::move(list)}};
  }

  ui::node_t details() const
  {
    const auto &t = ui::theme();
    const auto *e = table.selected_entry();
    ui::column_t col{.gap = 8, .align = ui::align_t::stretch};
    if (!e)
    {
      col.children.push_back(text("Nothing selected", 15, t.text));
      col.children.push_back(muted(grouped(table.shown()) + " items in this folder"));
      return ui::box_t{.size = {270, 0}, .color = panel, .radius = 10, .padding = 14, .align_x = ui::align_t::stretch, .align_y = ui::align_t::start, .children = {std::move(col)}};
    }
    col.children.push_back(ui::row_t{.gap = 10,
                                     .align = ui::align_t::center,
                                     .children = {ui::icon_t{.path = e->directory ? icons().folder : icons().file, .size = 28, .color = e->directory ? folder_color : file_color},
                                                  ui::box_t{.size = {200, 0}, .color = clear, .align_x = ui::align_t::start, .clip = true, .children = {text(e->name, 15, t.text)}}}});
    const auto line = [&](const char *name, std::string value) { col.children.push_back(ui::row_t{.gap = 8, .children = {ui::box_t{.size = {70, 0}, .color = clear, .align_x = ui::align_t::start, .children = {muted(name)}}, text(std::move(value), 12, t.text)}}); };
    line("Type", e->directory ? "File folder" : (types.contains(extension_of(e->name)) ? types.at(extension_of(e->name)) : extension_of(e->name)));
    if (!e->directory)
      line("Size", bytes(e->size) + (e->size >= 1024 ? "  (" + grouped(e->size) + " bytes)" : ""));
    line("Modified", when(e->modified));
    if (!e->directory)
    {
      const auto found = handlers.find(extension_of(e->name));
      if (found != handlers.end() && !found->second.empty())
        line("Opens with", found->second.front().name);
    }
    ui::column_t whole{.gap = 10, .align = ui::align_t::stretch, .children = {ui::box_t{.color = panel, .radius = 10, .padding = 14, .align_x = ui::align_t::stretch, .children = {std::move(col)}}}};
    if (!e->directory)
    {
      ui::column_t p{.gap = 6, .align = ui::align_t::stretch, .children = {ui::row_t{.children = {muted("Preview"), ui::spacer_t{}, muted(preview_image ? std::string() : preview_note, 11)}}}};
      if (preview_image && preview_image->width > 0 && preview_image->height > 0)
      {
        // Fitted to the pane, its shape kept, never enlarged past twice its pixels.
        constexpr float room_w = 246, room_h = 360;
        const float w = float(preview_image->width), h = float(preview_image->height);
        const float fit = std::min({room_w / w, room_h / h, 2.0f});
        p.children.push_back(ui::box_t{.color = clear,
                                       .align_x = ui::align_t::center,
                                       .children = {ui::image_t{.size = {int(w * fit), int(h * fit)}, .data = preview_image, .radius = 6}}});
        if (pdf_pages > 0)
          p.children.push_back(ui::row_t{.gap = 8,
                                         .align = ui::align_t::center,
                                         .children = {icon_button(pdf_previous, icons().back, pdf_page > 0), ui::spacer_t{},
                                                      muted("Page " + grouped(std::uint64_t(pdf_page + 1)) + " of " + grouped(std::uint64_t(pdf_pages)), 11), ui::spacer_t{},
                                                      icon_button(pdf_next, icons().forward, pdf_page + 1 < pdf_pages)}});
        else
          p.children.push_back(muted(grouped(std::uint64_t(preview_image->width)) + " x " + grouped(std::uint64_t(preview_image->height)) + " thumbnail", 11));
      }
      else
        p.children.push_back(ui::box_t{.color = clear, .grow = 1, .align_x = ui::align_t::stretch, .align_y = ui::align_t::stretch, .children = {ui::mount(preview, {.read_only = true, .columns = 30, .rows = 8})}});
      whole.children.push_back(ui::box_t{.color = panel, .radius = 10, .grow = 1, .padding = 12, .align_x = ui::align_t::stretch, .align_y = ui::align_t::stretch, .children = {std::move(p)}});
    }
    return ui::box_t{.size = {270, 0}, .color = clear, .align_x = ui::align_t::stretch, .align_y = ui::align_t::stretch, .children = {std::move(whole)}};
  }

  ui::node_t status_bar() const
  {
    const auto &t = ui::theme();
    std::string facts = grouped(table.shown()) + (table.shown() == 1 ? " item" : " items");
    if (table.shown() != table.entries().size())
      facts += " (of " + grouped(table.entries().size()) + ")";
    if (loading)
      facts += from_cache ? "  ·  shown from memory, checking for changes..." : "  ·  reading...";
    else
      facts += (navigations > 1 ? "  ·  first rows in " + ms(first_rows) : std::string("  ·  opened at start-up")) + "  ·  read in " + ms(listed_in) + (from_cache ? " (shown from memory at once)" : "") + "  ·  sorted in " + ms(table.sort_cost());
    return ui::row_t{.gap = 16,
                     .align = ui::align_t::center,
                     .children = {muted(facts, 12), ui::spacer_t{}, text(status, 12, status_error ? error_color : ok_color)}};
  }

  ui::node_t view() const
  {
    ui::column_t page{.gap = 12,
                      .align = ui::align_t::stretch,
                      .padding = 14,
                      .children = {toolbar(),
                                   ui::row_t{.gap = 12,
                                             .align = ui::align_t::stretch,
                                             .grow = 1,
                                             .children = {sidebar(), ui::box_t{.color = clear, .grow = 1, .align_x = ui::align_t::stretch, .align_y = ui::align_t::stretch, .children = {ui::mount(table, {.filter = filter, .show_hidden = show_hidden, .types_version = types_version, .passive = menu_at.has_value() || confirming.has_value()})}},
                                                          details()}},
                                   status_bar()}};
    // The menu, over everything, where it was opened.
    ui::box_t whole{.color = clear, .events = window_area, .align_x = ui::align_t::stretch, .align_y = ui::align_t::stretch, .children = {std::move(page)}};
    if (menu_at)
      whole.children.push_back(ui::at_t{.x = float(menu_at->x), .y = float(menu_at->y), .children = {ui::mount(menu, {.items = menu_items}).on_top(menu.dismiss)}});
    if (confirming)
    {
      // Centred over the list, near its top.
      const auto b = table.body_bounds();
      const std::string where = shell::trash_name();
      confirm_t::props_t ask;
      if (confirming_permanent)
      {
        ask.title = "Permanently delete \"" + confirming->name + "\"?";
        ask.message = confirming->directory ? "The folder and everything in it will be deleted.\nThis cannot be undone." : "It will not go to the " + where + ".\nThis cannot be undone.";
        ask.confirm = "Delete permanently";
        ask.danger = true;
      }
      else
      {
        ask.title = "Move \"" + confirming->name + "\" to the " + where + "?";
        ask.message = confirming->directory ? "The folder and everything in it can be restored from the " + where + "." : "It can be restored from the " + where + ".";
        ask.confirm = "Move to " + where;
      }
      whole.children.push_back(ui::at_t{.x = float(b.x + b.width / 2 - 200), .y = float(b.y + 60), .children = {ui::mount(confirm, std::move(ask)).on_top(confirm.dismiss)}});
    }
    return whole;
  }

  ui::behaviour_t behave(ui::scope_t &scope)
  {
    pipe_t events_from_workers(scope.loop(), [&scope](apply_t &&apply) { apply(scope); });
    // Declared after the pipe, so their threads are joined before it goes.
    worker_t lister_thread(false);
    worker_t helper_thread(true);
    worker_t jobs_thread(true);
    pipe = &events_from_workers;
    lister = &lister_thread;
    helper = &helper_thread;
    jobs = &jobs_thread;
    table.types = &types;
    table.want_type = [this](const std::string &ext) { want_type(ext); };

    // A file to start at opens its folder with it selected, as the path bar does.
    {
      std::error_code error;
      const auto start = typed_path(start_at);
      if (!start.empty() && std::filesystem::is_regular_file(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(start.data()), start.size())), error))
        go(scope, shell::parent(start), false, start.substr(std::min(start.size(), start.find_last_of("\\/") + 1)));
      else
        go(scope, start.empty() ? shell::home() : start, false);
    }

    auto presses = ui::subscribe<ui::press_event_t>(back_button, forward_button, up_button, home_button, refresh_button, pdf_previous, pdf_next);
    auto crumbs = ui::subscribe<ui::press_event_t>(crumb_buttons);
    auto places_pressed = ui::subscribe<ui::press_event_t>(place_buttons);
    auto filters = ui::subscribe<ui::line_edit_event_t>(filter_edit.changed);
    auto toggles = ui::subscribe<ui::toggle_event_t>(hidden_box.changed);
    auto selections = ui::subscribe<long>(table.selected);
    auto activations = ui::subscribe<long>(table.activated);
    auto contexts = ui::subscribe<table_context_t>(table.context);
    auto choices = ui::subscribe<ui::menu_choice_t>(menu.chosen);
    auto dismissals = ui::subscribe<ui::press_event_t>(menu.dismiss);
    auto keys = ui::subscribe<vui::key_event_t>(table.body());
    auto answers = ui::subscribe<bool>(confirm.answered);
    auto declined = ui::subscribe<ui::press_event_t>(confirm.dismiss);
    auto bar_drags = ui::subscribe<ui::drag_event_t>(path_bar);
    auto bar_menus = ui::subscribe<ui::context_event_t>(path_bar);
    auto typed = ui::subscribe<ui::line_edit_event_t>(path_edit.changed);
    auto entered = ui::subscribe<ui::line_edit_event_t>(path_edit.submitted);
    auto path_keys = ui::subscribe<vui::key_event_t>(path_edit.field());
    auto place_menus = ui::subscribe<ui::context_event_t>(place_buttons);
    auto navigations = ui::subscribe<ui::navigate_event_t>(window_area);
    auto events = ui::merge(scope, presses, crumbs, places_pressed, filters, toggles, selections, activations, contexts, choices, dismissals, keys, answers, declined, bar_drags, bar_menus, typed,
                            entered, path_keys, place_menus, navigations);
    while (auto event = co_await events)
    {
      switch (event->index())
      {
      case 0:
      {
        const auto *source = std::get<0>(*event).source;
        if (source == &back_button)
          go_back(scope);
        else if (source == &forward_button)
          go_forward(scope);
        else if (source == &up_button)
          up(scope);
        else if (source == &home_button)
          go(scope, shell::home());
        else if (source == &refresh_button)
          go(scope, path, false);
        else if (source == &pdf_previous)
          turn_pdf_page(scope, -1);
        else if (source == &pdf_next)
          turn_pdf_page(scope, 1);
        break;
      }
      case 1:
      {
        const auto parts = shell::crumbs(path);
        const std::size_t from = parts.size() > crumb_buttons.size() ? parts.size() - crumb_buttons.size() : 0;
        const auto i = std::size_t(std::get<1>(*event).source - &crumb_buttons[0]) + from;
        if (i < parts.size() && parts[i].path != path)
          go(scope, parts[i].path);
        break;
      }
      case 2:
      {
        const auto i = std::size_t(std::get<2>(*event).source - &place_buttons[0]);
        const auto &p = i < places.size() ? places[i] : drives[i - places.size()];
        go(scope, p.path);
        break;
      }
      case 3:
        filter = std::get<3>(*event).text;
        break;
      case 4:
        show_hidden = std::get<4>(*event).checked;
        break;
      case 5:
        preview_for(scope, std::get<5>(*event));
        break;
      case 6:
        activate(scope, std::get<6>(*event));
        break;
      case 7:
      {
        const auto &c = std::get<7>(*event);
        std::optional<shell::entry_t> entry;
        if (c.index >= 0)
          entry = table.entries()[std::size_t(c.index)];
        open_menu(scope, entry, c.position);
        break;
      }
      case 8:
        act(scope, std::get<8>(*event).index);
        break;
      case 9:
        menu_at.reset();
        break;
      case 18:
      {
        const auto &c = std::get<18>(*event);
        open_menu(scope, std::nullopt, c.position, false, std::size_t(c.source - &place_buttons[0]));
        break;
      }
      case 13:
        if (std::get<13>(*event).phase == ui::drag_phase_t::begin && std::get<13>(*event).clicks == 2)
          edit_path(scope);
        break;
      case 14:
        open_menu(scope, std::nullopt, std::get<14>(*event).position, true);
        break;
      case 15:
        typing = std::get<15>(*event).text;
        break;
      case 16:
        go_to_typed(scope);
        break;
      case 19:
        // The mouse's back and forward buttons; a menu open closes, a question asked stays.
        if (confirming)
          break;
        menu_at.reset();
        if (std::get<19>(*event).forward)
          go_forward(scope);
        else
          go_back(scope);
        break;
      case 17:
        if (std::get<17>(*event).pressed && std::get<17>(*event).key == vui::key_t::escape)
          editing_path = false;
        break;
      case 11:
        if (std::get<11>(*event))
          trash_confirmed(scope);
        else
          confirming.reset();
        break;
      case 12:
        confirming.reset();
        break;
      case 10:
      {
        const auto &key = std::get<10>(*event);
        if (!key.pressed)
          break;
        const bool alt = vui::has(key.modifiers, vui::modifier_t::alt);
        const bool ctrl = vui::has(key.modifiers, vui::modifier_t::control) || vui::has(key.modifiers, vui::modifier_t::super);
        const bool shift = vui::has(key.modifiers, vui::modifier_t::shift);
        // While asking, Enter is yes and Escape is no; nothing else happens.
        if (confirming)
        {
          if (key.key == vui::key_t::enter || key.key == vui::key_t::keypad_enter)
            trash_confirmed(scope);
          else if (key.key == vui::key_t::escape)
            confirming.reset();
          break;
        }
        // While the menu is open it has the keys it wants, and Escape closes it.
        if (menu_at)
        {
          if (key.key == vui::key_t::escape)
            menu_at.reset();
          else
            menu.key(key, scope);
          break;
        }
        if (key.key == vui::key_t::backspace || (alt && key.key == vui::key_t::up))
          up(scope);
        else if (alt && key.key == vui::key_t::left)
          go_back(scope);
        else if (alt && key.key == vui::key_t::right)
          go_forward(scope);
        else if (key.key == vui::key_t::f5)
          go(scope, path, false);
        else if (key.key == vui::key_t::delete_key)
        {
          if (const auto *e = table.selected_entry())
            ask_to_trash(*e, shift);
        }
        else if ((shift && key.key == vui::key_t::f10) || key.key == vui::key_t::menu)
        {
          std::optional<shell::entry_t> entry;
          if (const auto *e = table.selected_entry())
            entry = *e;
          open_menu(scope, entry, table.selected_point());
        }
        else if (ctrl && shift && key.key == vui::key_t::c)
        {
          const auto *e = table.selected_entry();
          const auto target = portable(e ? shell::join(path, e->name) : path);
          scope.window().set_clipboard_text(target);
          say("Copied " + target);
        }
        else if (alt && key.key == vui::key_t::enter && shell::has_properties())
        {
          const auto *e = table.selected_entry();
          const auto target = e ? shell::join(path, e->name) : path;
          launch([target] { return shell::properties(target); }, "");
        }
        break;
      }
      }
    }
    pipe = nullptr;
    lister = nullptr;
    helper = nullptr;
    jobs = nullptr;
  }
};

// ---- --probe: the shell layer on its own, for CI

int probe_and_check()
{
  // Unbuffered, so what was checked before a crash is still printed.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  shell::initialize();
  int failures = 0;
  const auto check = [&failures](bool ok, const std::string &what)
  {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
  };
  const auto h = shell::home();
  std::printf("home: %s\n", h.c_str());
  check(!h.empty(), "a home folder");
  std::size_t batches = 0, entries = 0, first = 0;
  const auto took = shell::list(h,
                                [&](std::vector<shell::entry_t> &&batch)
                                {
                                  if (batches++ == 0)
                                    first = batch.size();
                                  entries += batch.size();
                                  return true;
                                });
  check(bool(took), "listing it" + (took ? " (" + std::to_string(entries) + " entries in " + std::to_string(batches) + " batches, " + ms(*took) + ")" : ": " + took.error()));
  check(first <= shell::first_batch, "the first batch is small");
  const auto ps = shell::places();
  check(!ps.empty() && ps.front().path == h, "places, home first (" + std::to_string(ps.size()) + ")");
  for (const auto &p : ps)
    std::printf("    %s  %s\n", p.name.c_str(), p.path.c_str());
  const auto ds = shell::drives();
  check(!ds.empty(), "drives (" + std::to_string(ds.size()) + ")");
  for (const auto &d : ds)
    std::printf("    %s  %s  %s free of %s\n", d.name.c_str(), d.path.c_str(), bytes(d.free).c_str(), bytes(d.total).c_str());
  const auto trail = shell::crumbs(h);
  check(!trail.empty() && trail.back().path == h, "breadcrumbs end at home (" + std::to_string(trail.size()) + ")");
  check(shell::parent(shell::parent(h)) != h, "parent climbs");
  check(shell::join(h, "x").size() == h.size() + 2, "join adds one separator");
  std::printf("type of notes.txt: %s\n", shell::type_name("notes.txt").c_str());
  // A file of our own, to ask what opens it.
  const auto file = shell::join(h, ".vui_explorer_probe.txt");
  {
    std::ofstream out(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(file.data()), file.size())));
    out << "probe\n";
  }
  const auto hs = shell::handlers(file);
  std::printf("applications for a .txt file: %zu\n", hs.size());
  for (const auto &x : hs)
    std::printf("    %s%s  (%s)\n", x.name.c_str(), x.is_default ? " [default]" : "", x.id.c_str());
  std::error_code ec;
  std::filesystem::remove(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(file.data()), file.size())), ec);
  // Cancelling: a batch callback that says stop ends the read.
  std::size_t seen = 0;
  (void)shell::list(h,
                    [&](std::vector<shell::entry_t> &&)
                    {
                      ++seen;
                      return false;
                    });
  check(seen <= 1, "a listing stops when asked");

  // Deleting, in a folder of the probe's own under home.
  const auto scratch = shell::join(h, ".vui_explorer_probe");
  const auto native = [](const std::string &p) { return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(p.data()), p.size())); };
  std::filesystem::create_directories(native(shell::join(shell::join(scratch, "folder"), "deep")), ec);
  {
    std::ofstream(native(shell::join(shell::join(shell::join(scratch, "folder"), "deep"), "x.txt"))) << "x\n";
    std::ofstream(native(shell::join(scratch, "file.txt"))) << "y\n";
  }
  // A thumbnail of a picture the probe draws itself: a 64x32 gradient as an uncompressed BMP, a
  // format every platform's thumbnailer reads. Required where the system has thumbnails built in;
  // a bare Linux container has no thumbnailer, so there it is only reported.
  {
    const int w = 64, h = 32;
    std::string bmp(54 + std::size_t(w) * h * 3, '\0');
    const auto put = [&bmp](std::size_t at, std::uint32_t v, int bytes)
    {
      for (int i = 0; i < bytes; ++i)
        bmp[at + std::size_t(i)] = char((v >> (8 * i)) & 0xff);
    };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put(2, std::uint32_t(bmp.size()), 4);
    put(10, 54, 4);
    put(14, 40, 4);
    put(18, std::uint32_t(w), 4);
    put(22, std::uint32_t(h), 4);
    put(26, 1, 2);
    put(28, 24, 2);
    put(34, std::uint32_t(w * h * 3), 4);
    for (int y = 0; y < h; ++y)
    {
      for (int x = 0; x < w; ++x)
      {
        const auto at = 54 + (std::size_t(y) * w + std::size_t(x)) * 3;
        bmp[at] = char(255 - x * 4);
        bmp[at + 1] = char(y * 8);
        bmp[at + 2] = char(x * 4);
      }
    }
    std::ofstream(native(shell::join(scratch, "picture.bmp")), std::ios::binary) << bmp;
    const auto picture = shell::thumbnail(shell::join(scratch, "picture.bmp"), 128);
#if defined(__linux__)
    std::printf("  thumbnail of a BMP: %s\n", picture ? (std::to_string(picture->width) + " x " + std::to_string(picture->height)).c_str() : picture.error().c_str());
#else
    check(picture && picture->width > 0 && picture->height > 0 && picture->width >= picture->height && picture->rgba.size() == std::size_t(picture->width) * picture->height * 4,
          "a thumbnail of a picture" + (picture ? " (" + std::to_string(picture->width) + " x " + std::to_string(picture->height) + ")" : ": " + picture.error()));
#endif
  }
  // PDF pages (G61), from a PDF the probe writes itself: a wide page with a red block and a tall one
  // with a blue block. Required on Windows and macOS, which carry a renderer; on Linux only when
  // poppler's pdftoppm is installed.
  {
    const std::vector<std::string> objects{
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 5 0 R >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 200] /Contents 6 0 R >>",
      "<< /Length 25 >>\nstream\n1 0 0 rg 0 0 200 100 re f\nendstream",
      "<< /Length 25 >>\nstream\n0 0 1 rg 0 0 100 200 re f\nendstream",
    };
    std::string pdf = "%PDF-1.4\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i)
    {
      offsets.push_back(pdf.size());
      pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const auto xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const auto offset : offsets)
    {
      char line[24];
      std::snprintf(line, sizeof line, "%010zu 00000 n \n", offset);
      pdf += line;
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    const auto file = shell::join(scratch, "two pages.pdf");
    std::ofstream(native(file), std::ios::binary) << pdf;
    const auto drawing = clock_t_::now();
    const auto first = shell::pdf_page(file, 0, 200);
    const auto drawn_in = clock_t_::now() - drawing;
    // Past the end is clamped to the last page.
    const auto drawing_last = clock_t_::now();
    const auto last = shell::pdf_page(file, 7, 200);
    const auto last_in = clock_t_::now() - drawing_last;
    const auto middle = [](const shell::picture_t &picture)
    {
      const auto at = (std::size_t(picture.height / 2) * std::size_t(picture.width) + std::size_t(picture.width / 2)) * 4;
      return std::array<int, 3>{picture.rgba[at], picture.rgba[at + 1], picture.rgba[at + 2]};
    };
    const bool first_ok = first && first->pages == 2 && first->page == 0 && first->picture.width == 200 && first->picture.height >= 95 && first->picture.height <= 105 &&
                          first->picture.rgba.size() == std::size_t(first->picture.width) * first->picture.height * 4 && middle(first->picture)[0] > 200 && middle(first->picture)[2] < 60;
    const bool last_ok = last && last->page == 1 && last->picture.height == 200 && last->picture.width >= 95 && last->picture.width <= 105 && middle(last->picture)[2] > 200 &&
                         middle(last->picture)[0] < 60;
    const auto said = [](const std::expected<shell::pdf_page_t, std::string> &r)
    { return r ? std::to_string(r->picture.width) + " x " + std::to_string(r->picture.height) + ", page " + std::to_string(r->page + 1) + " of " + std::to_string(r->pages) : r.error(); };
#if defined(__linux__)
    if (!first && first.error() == "pdftoppm is not installed")
      std::printf("  (PDF pages not tried: pdftoppm is not installed)\n");
    else
#endif
    {
      check(first_ok, "the first page of a PDF (" + said(first) + ", " + ms(std::chrono::duration_cast<std::chrono::nanoseconds>(drawn_in)) + ")");
      check(last_ok, "a page past the end is the last one (" + said(last) + ", " + ms(std::chrono::duration_cast<std::chrono::nanoseconds>(last_in)) + ")");
    }
  }
  // Archives: every format some installed tool writes, packed and unpacked again, the contents
  // compared; and extracting again keeps a file that is there.
  {
    const auto &tools = shell::archivers();
    std::printf("archive tools:");
    for (const auto &t : tools)
      std::printf(" %s", t.name.c_str());
    std::printf("\n");
    const auto source = shell::join(scratch, "packed");
    std::filesystem::create_directories(native(shell::join(source, "inner")), ec);
    std::ofstream(native(shell::join(source, "a.txt")), std::ios::binary) << "alpha\n";
    std::ofstream(native(shell::join(shell::join(source, "inner"), "b.txt")), std::ios::binary) << "beta\n";
    const auto read = [&](const std::string &p)
    {
      std::ifstream in(native(p), std::ios::binary);
      return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    for (const char *suffix : {"zip", "7z", "rar", "tar.gz"})
    {
      const auto writer = std::find_if(tools.begin(), tools.end(), [&](const shell::archiver_t &t) { return std::find(t.creates.begin(), t.creates.end(), suffix) != t.creates.end(); });
      if (writer == tools.end())
        continue;
      const auto archive = shell::join(scratch, std::string("packed.") + suffix);
      const auto packed = shell::compress(*writer, source, archive);
      const auto reader = std::find_if(tools.begin(), tools.end(), [&](const shell::archiver_t &t) { return shell::can_extract(t, archive); });
      const auto into = shell::join(scratch, std::string("unpacked-") + suffix);
      const auto unpacked = packed && reader != tools.end() ? shell::extract(*reader, archive, into) : std::expected<void, std::string>(std::unexpected(std::string("nothing reads it")));
      const auto back = shell::join(into, "packed");
      const bool same = unpacked && read(shell::join(back, "a.txt")) == "alpha\n" && read(shell::join(shell::join(back, "inner"), "b.txt")) == "beta\n";
      check(packed && same, std::string(suffix) + " written by " + writer->name + (reader != tools.end() ? " and read back by " + reader->name : "") +
                              (packed ? (unpacked ? std::string() : ": " + unpacked.error()) : ": " + packed.error()));
      if (same)
      {
        // Again, over a file changed since: it must stay as it is.
        std::ofstream(native(shell::join(back, "a.txt")), std::ios::binary) << "changed\n";
        const auto again = shell::extract(*reader, archive, into);
        check(again && read(shell::join(back, "a.txt")) == "changed\n", std::string(suffix) + ": extracting again keeps a file that is there");
      }
    }
  }
  // The places, saved and read back, in a settings folder of the probe's own.
  {
    config_override = shell::join(scratch, "settings");
    auto list = shell::places();
    list.push_back({"probe place", shell::join(scratch, "folder"), shell::place_t::kind_t::folder});
    const auto saved = save_places(list);
    const auto loaded = load_places();
    check(saved && loaded.size() == list.size() && loaded.back().name == "probe place" && loaded.back().path == list.back().path && loaded.front().kind == shell::place_t::kind_t::home,
          "places are saved and read back" + (saved ? std::string() : ": " + saved.error()));
    config_override.clear();
  }
  const auto gone = shell::remove_permanently(shell::join(scratch, "folder"));
  check(gone && !std::filesystem::exists(native(shell::join(scratch, "folder"))), "a folder is deleted permanently, with what is in it" + (gone ? std::string() : ": " + gone.error()));
  // Only where asked: anywhere else it would leave a file in the user's own Trash.
  if (const char *t = std::getenv("VUI_PROBE_TRASH"); t && std::strcmp(t, "1") == 0)
  {
    const auto moved = shell::trash(shell::join(scratch, "file.txt"));
    check(moved && !std::filesystem::exists(native(shell::join(scratch, "file.txt"))), std::string("a file moves to the ") + shell::trash_name() + (moved ? std::string() : ": " + moved.error()));
  }
  else
    std::printf("  (moving to the %s not tried: set VUI_PROBE_TRASH=1)\n", shell::trash_name());
  std::filesystem::remove_all(native(scratch), ec);
  return failures == 0 ? 0 : 1;
}
} // namespace

int main(int argc, char **argv)
{
  if (argc > 1 && std::strcmp(argv[1], "--probe") == 0)
    return probe_and_check();
  shell::initialize();
  // `vui_explorer [milliseconds] [--at folder]`: a time limit for CI, a folder to start in.
  long milliseconds = 0;
  std::string start;
  for (int i = 1; i < argc; ++i)
  {
    if (std::strcmp(argv[i], "--at") == 0 && i + 1 < argc)
      start = argv[++i];
    else
      milliseconds = std::atol(argv[i]);
  }
  const auto limit = std::chrono::milliseconds(milliseconds);
  return vio::run(
    [limit, start](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto display = vui::display_t::open(loop);
      if (!display)
      {
        std::fprintf(stderr, "no display: %s\n", display.error().msg.c_str());
        co_return 1;
      }
      auto window = vui::window_t::create(*display, {.title = "vui explorer", .size = {1680, 1050}});
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
      co_await ui::run(*window, explorer_t{start});
      co_await std::move(closing);
      co_return 0;
    });
}
