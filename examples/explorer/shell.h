#pragma once

// What the explorer asks of the system, the same on every platform: listing folders, the folders a
// user has, the applications that open a file, and starting them. shell_windows.cpp,
// shell_linux.cpp and shell_macos.cpp each implement it; only one compiles on a given platform.
// Paths are UTF-8 throughout.
//
// Everything here may block -- a network drive, a slow registry -- so the explorer calls it on its
// worker thread, never on the loop's, except the launching calls, which return at once.

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <vector>

namespace shell
{
struct entry_t
{
  std::string name;
  std::uint64_t size = 0;
  // Last written, as seconds since the epoch.
  std::int64_t modified = 0;
  bool directory = false;
  bool hidden = false;
  bool link = false;
};

// A folder is read in batches and each is handed over as soon as it is read, so the first rows show
// before a big folder has been read to the end. The first batch is small for that reason; later ones
// are bigger, because each costs a hand-over. `batch` returns false to stop reading: the user has
// gone somewhere else. Returns how long the whole read took.
using batch_t = std::function<bool(std::vector<entry_t> &&entries)>;
std::expected<std::chrono::nanoseconds, std::string> list(const std::string &path, const batch_t &batch);

// How big each batch is: the first, then every later one.
inline constexpr std::size_t first_batch = 256;
inline constexpr std::size_t later_batch = 4096;

// A place in the sidebar.
struct place_t
{
  std::string name;
  std::string path;
  enum class kind_t : std::uint8_t
  {
    home,
    folder,
    drive,
  } kind = kind_t::folder;
  // For a drive, when known.
  std::uint64_t total = 0;
  std::uint64_t free = 0;
};
[[nodiscard]] std::string home();
[[nodiscard]] std::vector<place_t> places();
[[nodiscard]] std::vector<place_t> drives();

// The parent of a path, or the path itself at a root; and joining a name onto a folder.
[[nodiscard]] std::string parent(const std::string &path);
[[nodiscard]] std::string join(const std::string &folder, const std::string &name);
// The path from its root down, each with the full path it stands for: a breadcrumb trail.
struct crumb_t
{
  std::string name;
  std::string path;
};
[[nodiscard]] std::vector<crumb_t> crumbs(const std::string &path);

// What kind of file a name is, in words: "Text Document", "PNG image". From the extension only.
[[nodiscard]] std::string type_name(const std::string &name);

// An application that can open a file, as the system registers it.
struct handler_t
{
  std::string name;
  // How to start it again: an executable, a .desktop file, an application bundle.
  std::string id;
  bool is_default = false;
};
// For a file, by its type; the default first.
[[nodiscard]] std::vector<handler_t> handlers(const std::string &path);

// These start something and return; their error is that it could not be started.
std::expected<void, std::string> open(const std::string &path);
std::expected<void, std::string> open_with(const std::string &path, const handler_t &handler);
std::expected<void, std::string> open_terminal(const std::string &folder);
// Shows it in the system's own file manager, selected.
std::expected<void, std::string> reveal(const std::string &path);
// The system's properties window, where there is one.
[[nodiscard]] bool has_properties();
std::expected<void, std::string> properties(const std::string &path);

// A picture of a file, as the system's own file manager would show it: an image scaled down, a frame
// of a video, the first page of a PDF -- whatever the installed thumbnail handlers make. RGBA, sRGB,
// straight alpha, rows from the top, no larger than `pixels` on its longer side. Empty when the
// system has no thumbnail for the file; an icon is not a thumbnail. Slow: call it on a worker.
struct picture_t
{
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba;
};
[[nodiscard]] std::expected<picture_t, std::string> thumbnail(const std::string &path, int pixels);

// Moves a file or a folder, with everything in it, to the Recycle Bin or Trash, from where it can be
// restored. May take a while for a big folder.
std::expected<void, std::string> trash(const std::string &path);
// Deletes a file or a folder with everything in it, for good. A link inside is deleted itself, never
// followed into what it points at. The explorer asks twice as hard before it calls this.
std::expected<void, std::string> remove_permanently(const std::string &path);
// "Recycle Bin" or "Trash".
[[nodiscard]] const char *trash_name();

// What the system file manager is called, for "Show in ...".
[[nodiscard]] const char *file_manager_name();

// Call once, on the thread that launches things, before any of the above (COM on Windows).
void initialize();
} // namespace shell
