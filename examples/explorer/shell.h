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
#include <optional>
#include <string>
#include <utility>
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
// Where a user's settings for the explorer belong: under the roaming AppData folder on Windows,
// ~/Library/Application Support on macOS, $XDG_CONFIG_HOME on Linux. Not created here.
[[nodiscard]] std::string config_dir();
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
// The terminal opened in a folder (G67): WezTerm wherever it is installed, running the shell its
// configuration names, else the platform's own. Elevated asks through the system first.
[[nodiscard]] std::string terminal_name();
std::expected<void, std::string> open_terminal(const std::string &folder, bool elevated = false);
// Starting a program as administrator (G67): Windows' runas verb, which asks through UAC. Programs
// are what Windows runs by name: .exe, .com, .bat, .cmd, .msi.
[[nodiscard]] bool can_elevate();
[[nodiscard]] bool is_program(const std::string &name);
std::expected<void, std::string> run_elevated(const std::string &path);
// This explorer as the file manager folders open in (G67): double-clicking a folder or a drive,
// and Win+E, for the current user only and undone without a trace. Windows only.
[[nodiscard]] bool can_be_default();
[[nodiscard]] bool is_default();
std::expected<void, std::string> make_default();
std::expected<void, std::string> restore_default();
#if defined(_WIN32)
namespace detail
{
// The per-user classes key, under HKEY_CURRENT_USER; --probe points it at a scratch key.
extern std::wstring classes;
// --probe's check of make_default and restore_default, run against a scratch key and removed after:
// what was checked, and whether it held.
std::vector<std::pair<bool, std::string>> probe_default();
} // namespace detail
#endif
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

// One page of a PDF drawn by the system's own renderer (G61), on white, no larger than `pixels` on its
// longer side, and how many pages there are: Windows.Data.Pdf on Windows, Core Graphics on macOS,
// poppler's pdftoppm on Linux when it is installed. `page` counts from 0 and is clamped. Slow: call
// it on a worker.
struct pdf_page_t
{
  picture_t picture;
  int page = 0;
  int pages = 0;
};
[[nodiscard]] std::expected<pdf_page_t, std::string> pdf_page(const std::string &path, int page, int pixels);

// An archive tool the system has (G59): found once, best first. `program` is its command line, used
// to extract and compress; `gui` is its own window, to open an archive in, when it has one.
struct archiver_t
{
  enum class kind_t : std::uint8_t
  {
    seven_zip,
    winrar,
    bsdtar,
    gnu_tar,
    info_zip,
    gui_only,
  };
  std::string name;
  kind_t kind = kind_t::gui_only;
  std::string program;
  std::string gui;
  // What it writes, by suffix: "zip", "7z", "rar", "tar.gz".
  std::vector<std::string> creates;
};
[[nodiscard]] const std::vector<archiver_t> &archivers();
// An archive by its name: zip, 7z, rar, tar and the compressed tars, and the rest the tools read.
[[nodiscard]] bool is_archive(const std::string &name);
// Whether this tool's command line reads that archive.
[[nodiscard]] bool can_extract(const archiver_t &tool, const std::string &name);
// Unpacks `archive` into the folder `into`, which is made if needed. Never overwrites a file that is
// there already. Waits until it is done: call it on a worker.
std::expected<void, std::string> extract(const archiver_t &tool, const std::string &archive, const std::string &into);
// Packs files and folders, all in one folder, into `archive`, which must not exist yet, its format
// chosen by its suffix. Names inside are relative to that folder. Waits until it is done.
std::expected<void, std::string> compress(const archiver_t &tool, const std::vector<std::string> &items, const std::string &archive);
std::expected<void, std::string> open_in_archiver(const archiver_t &tool, const std::string &archive);

// Moves a file or a folder, with everything in it, to the Recycle Bin or Trash, from where it can be
// restored. May take a while for a big folder.
std::expected<void, std::string> trash(const std::string &path);
// Deletes a file or a folder with everything in it, for good. A link inside is deleted itself, never
// followed into what it points at. The explorer asks twice as hard before it calls this.
std::expected<void, std::string> remove_permanently(const std::string &path);
// "Recycle Bin" or "Trash".
[[nodiscard]] const char *trash_name();

// ---- copying, moving and renaming (G69)

// Copies files and folders, with everything in them, into `folder`, or moves them there. On Windows
// it is the shell's own operation: its progress, its questions when a name is taken, and undo in
// Explorer. Elsewhere a name that is taken gets a free one beside it, "name (2).txt", so nothing is
// overwritten. Copying into the folder something is in makes a second one beside it; moving it there
// does nothing. A folder is never put inside itself. Waits until it is done: call it on a worker.
std::expected<void, std::string> transfer(const std::vector<std::string> &items, const std::string &folder, bool move);
// Gives a file or folder a new name in the folder it is in. A name that is taken, or one that holds
// a path separator, is refused; a change of case alone is allowed.
std::expected<void, std::string> rename_item(const std::string &path, const std::string &name);

// Files cut or copied, waiting to be pasted.
struct clipboard_files_t
{
  std::vector<std::string> items;
  bool cut = false;
};
// On Windows the system's clipboard, in the form Explorer and other programs read and write, so
// files copied here paste there and the other way round; `owner` is the window putting them there.
// Elsewhere it is this program's own, until the toolkit's clipboard carries files.
std::expected<void, std::string> put_on_clipboard(const std::vector<std::string> &items, bool cut, void *owner);
// What is on it, or nothing when it holds no files.
[[nodiscard]] std::optional<clipboard_files_t> files_on_clipboard(void *owner);
// After cut files have been pasted: they are where they were put, and are not to be moved again.
void clear_clipboard(void *owner);

// What the system file manager is called, for "Show in ...".
[[nodiscard]] const char *file_manager_name();

// Call once, on the thread that launches things, before any of the above (COM on Windows).
void initialize();

namespace detail
{
// Each platform's part of the archive support: the tools it finds, and running one and waiting for
// it, in `directory`, to its exit code.
std::vector<archiver_t> find_archivers();
std::expected<int, std::string> run_tool(const std::vector<std::string> &argv, const std::string &directory);
} // namespace detail
} // namespace shell
