// Copying, moving and renaming on Linux and macOS, and the clipboard of files there (G69). Windows
// has the shell's own, in shell_windows.cpp.
#if !defined(_WIN32)

#include "shell.h"

#include <sys/stat.h>

#include <filesystem>
#include <mutex>
#include <system_error>

namespace shell
{
namespace
{
namespace fs = std::filesystem;

fs::path native(const std::string &s)
{
  return fs::path(std::u8string(reinterpret_cast<const char8_t *>(s.data()), s.size()));
}

std::string utf8(const fs::path &p)
{
  const auto s = p.u8string();
  return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}

bool exists(const fs::path &p)
{
  std::error_code error;
  return fs::exists(fs::symlink_status(p, error));
}

// "name (2).txt", "name (3).txt" ... beside a name that is taken. A folder has no extension.
fs::path free_name(const fs::path &folder, const fs::path &name, bool directory)
{
  const auto stem = directory ? utf8(name) : utf8(name.stem());
  const auto ext = directory ? std::string() : utf8(name.extension());
  for (int n = 2;; ++n)
  {
    auto candidate = folder / native(stem + " (" + std::to_string(n) + ")" + ext);
    if (!exists(candidate))
      return candidate;
  }
}

// Whether `inner` is `outer` or somewhere inside it.
bool within(const fs::path &inner, const fs::path &outer)
{
  std::error_code error;
  const auto a = fs::weakly_canonical(inner, error);
  const auto b = fs::weakly_canonical(outer, error);
  auto i = a.begin();
  for (auto o = b.begin(); o != b.end(); ++o, ++i)
  {
    if (*o == fs::path())
      continue;
    if (i == a.end() || *i != *o)
      return false;
  }
  return true;
}

std::mutex clipboard_mutex;
std::optional<clipboard_files_t> clipboard;
} // namespace

std::expected<void, std::string> transfer(const std::vector<std::string> &items, const std::string &folder, bool move)
{
  const auto into = native(folder);
  for (const auto &item : items)
  {
    const auto from = native(item);
    std::error_code error;
    const auto status = fs::symlink_status(from, error);
    if (error)
      return std::unexpected(item + ": " + error.message());
    const bool directory = fs::is_directory(status);
    const bool here = fs::equivalent(from.parent_path(), into, error);
    if (here && move)
      continue;
    if (directory && within(into, from))
      return std::unexpected("cannot put " + utf8(from.filename()) + " inside itself");
    auto to = into / from.filename();
    if (exists(to))
      to = free_name(into, from.filename(), directory);
    if (move)
    {
      fs::rename(from, to, error);
      if (!error)
        continue;
      // Another file system: copy, then remove what was copied.
      if (error != std::errc::cross_device_link)
        return std::unexpected("could not move " + utf8(from.filename()) + ": " + error.message());
      error.clear();
    }
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, error);
    if (error)
      return std::unexpected("could not copy " + utf8(from.filename()) + ": " + error.message());
    if (move)
    {
      fs::remove_all(from, error);
      if (error)
        return std::unexpected("copied " + utf8(from.filename()) + " but could not remove it: " + error.message());
    }
  }
  return {};
}

bool same_volume(const std::string &a, const std::string &b)
{
  struct stat x{}, y{};
  return ::stat(a.c_str(), &x) == 0 && ::stat(b.c_str(), &y) == 0 && x.st_dev == y.st_dev;
}

std::expected<void, std::string> rename_item(const std::string &path, const std::string &name)
{
  if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos)
    return std::unexpected(std::string("a name cannot be empty or hold a /"));
  const auto from = native(path);
  const auto to = from.parent_path() / native(name);
  std::error_code error;
  // Taken, unless it is the same file under another case on a file system that ignores case.
  if (exists(to) && !fs::equivalent(from, to, error))
    return std::unexpected("there is already something called \"" + name + "\" here");
  fs::rename(from, to, error);
  if (error)
    return std::unexpected(error.message());
  return {};
}

std::expected<void, std::string> put_on_clipboard(const std::vector<std::string> &items, bool cut, void *)
{
  std::scoped_lock lock(clipboard_mutex);
  clipboard = clipboard_files_t{items, cut};
  return {};
}

std::optional<clipboard_files_t> files_on_clipboard(void *)
{
  std::scoped_lock lock(clipboard_mutex);
  return clipboard;
}

void clear_clipboard(void *)
{
  std::scoped_lock lock(clipboard_mutex);
  clipboard.reset();
}
} // namespace shell

#endif
