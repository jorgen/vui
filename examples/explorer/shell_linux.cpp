#if defined(__linux__)

// Linux: readdir and fstatat read a folder; the XDG user-dirs file names the user's folders; and
// the applications that open a file are the .desktop entries that list its MIME type, with
// mimeapps.list deciding the default -- the same rules every desktop follows.

#include "shell.h"

#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace shell
{
namespace
{
std::string env(const char *name, const std::string &fallback = {})
{
  const char *v = std::getenv(name);
  return v && *v ? std::string(v) : fallback;
}

bool is_directory(const std::string &path)
{
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string lower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

std::string extension_of(const std::string &name)
{
  const auto slash = name.rfind('/');
  const auto dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0 || (slash != std::string::npos && dot < slash + 2))
    return {};
  return lower(name.substr(dot + 1));
}

// Extension to MIME type, from the system's table.
const std::unordered_map<std::string, std::string> &mime_table()
{
  static const auto table = []
  {
    std::unordered_map<std::string, std::string> t;
    std::ifstream in("/etc/mime.types");
    for (std::string line; std::getline(in, line);)
    {
      if (line.empty() || line[0] == '#')
        continue;
      std::istringstream words(line);
      std::string mime, ext;
      words >> mime;
      while (words >> ext)
        t.emplace(lower(ext), mime);
    }
    // A few the table often lacks.
    for (const auto &[e, m] : {std::pair{"md", "text/markdown"}, {"cpp", "text/x-c++src"}, {"h", "text/x-chdr"}, {"hpp", "text/x-c++hdr"}, {"txt", "text/plain"}, {"json", "application/json"}})
      t.emplace(e, m);
    return t;
  }();
  return table;
}

std::string mime_of(const std::string &path)
{
  if (is_directory(path))
    return "inode/directory";
  const auto &t = mime_table();
  if (auto it = t.find(extension_of(path)); it != t.end())
    return it->second;
  return "application/octet-stream";
}

struct desktop_entry_t
{
  std::string id;
  std::string path;
  std::string name;
  std::string exec;
  std::vector<std::string> mimes;
};

std::vector<std::string> split(const std::string &s, char by)
{
  std::vector<std::string> out;
  std::string part;
  std::istringstream in(s);
  while (std::getline(in, part, by))
  {
    if (!part.empty())
      out.push_back(part);
  }
  return out;
}

std::vector<std::string> data_dirs()
{
  std::vector<std::string> dirs{env("XDG_DATA_HOME", env("HOME") + "/.local/share")};
  for (auto &d : split(env("XDG_DATA_DIRS", "/usr/local/share:/usr/share"), ':'))
    dirs.push_back(d);
  return dirs;
}

// Every application entry, by id, read once: the first found for an id wins, user entries first.
const std::vector<desktop_entry_t> &applications()
{
  static const auto entries = []
  {
    std::vector<desktop_entry_t> out;
    std::unordered_map<std::string, bool> seen;
    for (const auto &dir : data_dirs())
    {
      const auto apps = dir + "/applications";
      DIR *d = opendir(apps.c_str());
      if (!d)
        continue;
      while (auto *e = readdir(d))
      {
        const std::string file = e->d_name;
        if (file.size() < 9 || file.compare(file.size() - 8, 8, ".desktop") != 0 || seen.count(file))
          continue;
        seen[file] = true;
        desktop_entry_t entry;
        entry.id = file;
        entry.path = apps + "/" + file;
        std::ifstream in(entry.path);
        bool main = false, hidden = false, application = false;
        for (std::string line; std::getline(in, line);)
        {
          if (!line.empty() && line[0] == '[')
          {
            main = line == "[Desktop Entry]";
            continue;
          }
          if (!main)
            continue;
          if (line.rfind("Name=", 0) == 0 && entry.name.empty())
            entry.name = line.substr(5);
          else if (line.rfind("Exec=", 0) == 0)
            entry.exec = line.substr(5);
          else if (line.rfind("MimeType=", 0) == 0)
            entry.mimes = split(line.substr(9), ';');
          else if (line == "NoDisplay=true" || line == "Hidden=true")
            hidden = true;
          else if (line == "Type=Application")
            application = true;
        }
        if (application && !hidden && !entry.exec.empty() && !entry.name.empty())
          out.push_back(std::move(entry));
      }
      closedir(d);
    }
    return out;
  }();
  return entries;
}

// The default for a MIME type from mimeapps.list, user configuration first.
std::string default_for(const std::string &mime)
{
  const auto config = env("XDG_CONFIG_HOME", env("HOME") + "/.config");
  for (const auto &file : {config + "/mimeapps.list", std::string("/etc/xdg/mimeapps.list"), std::string("/usr/share/applications/mimeapps.list"), env("HOME") + "/.local/share/applications/mimeapps.list"})
  {
    std::ifstream in(file);
    bool defaults = false;
    for (std::string line; std::getline(in, line);)
    {
      if (!line.empty() && line[0] == '[')
      {
        defaults = line == "[Default Applications]";
        continue;
      }
      if (defaults && line.rfind(mime + "=", 0) == 0)
      {
        const auto ids = split(line.substr(mime.size() + 1), ';');
        if (!ids.empty())
          return ids.front();
      }
    }
  }
  return {};
}

bool on_path(const std::string &program)
{
  if (program.find('/') != std::string::npos)
    return access(program.c_str(), X_OK) == 0;
  for (const auto &dir : split(env("PATH", "/usr/bin:/bin"), ':'))
  {
    if (access((dir + "/" + program).c_str(), X_OK) == 0)
      return true;
  }
  return false;
}

// Start a program detached from this process -- a grandchild, so nothing is left to reap -- and
// report whether exec succeeded, through a close-on-exec pipe that only an exec failure writes to.
std::expected<void, std::string> spawn(const std::vector<std::string> &argv, const std::string &directory = {})
{
  if (argv.empty() || !on_path(argv[0]))
    return std::unexpected((argv.empty() ? std::string("nothing") : argv[0]) + " is not installed");
  int report[2];
  if (pipe2(report, O_CLOEXEC) != 0)
    return std::unexpected(std::string(std::strerror(errno)));
  const pid_t child = fork();
  if (child < 0)
    return std::unexpected(std::string(std::strerror(errno)));
  if (child == 0)
  {
    setsid();
    if (fork() != 0)
      _exit(0);
    if (!directory.empty() && chdir(directory.c_str()) != 0)
      _exit(127);
    std::vector<char *> args;
    for (const auto &a : argv)
      args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    execvp(args[0], args.data());
    const int e = errno;
    (void)!write(report[1], &e, sizeof e);
    _exit(127);
  }
  close(report[1]);
  int status = 0;
  waitpid(child, &status, 0);
  int e = 0;
  const auto n = read(report[0], &e, sizeof e);
  close(report[0]);
  if (n == sizeof e)
    return std::unexpected(argv[0] + ": " + std::strerror(e));
  return {};
}

// A desktop entry's Exec line as arguments, the file in place of its field codes.
std::vector<std::string> command_for(const std::string &exec, const std::string &path)
{
  std::vector<std::string> args;
  std::string word;
  bool quoted = false, any = false, placed = false;
  for (std::size_t i = 0; i < exec.size(); ++i)
  {
    const char c = exec[i];
    if (c == '"')
    {
      quoted = !quoted;
      any = true;
      continue;
    }
    if (c == '\\' && quoted && i + 1 < exec.size())
    {
      word += exec[++i];
      continue;
    }
    if (c == ' ' && !quoted)
    {
      if (any || !word.empty())
        args.push_back(word);
      word.clear();
      any = false;
      continue;
    }
    if (c == '%' && i + 1 < exec.size())
    {
      const char code = exec[++i];
      if (code == '%')
        word += '%';
      else if (code == 'f' || code == 'F' || code == 'u' || code == 'U')
      {
        word += path;
        placed = true;
      }
      continue;
    }
    word += c;
  }
  if (any || !word.empty())
    args.push_back(word);
  if (!placed)
    args.push_back(path);
  return args;
}
} // namespace

void initialize()
{
}

std::expected<std::chrono::nanoseconds, std::string> list(const std::string &path, const batch_t &batch)
{
  const auto started = std::chrono::steady_clock::now();
  DIR *d = opendir(path.c_str());
  if (!d)
    return std::unexpected(std::string(std::strerror(errno)));
  std::vector<entry_t> out;
  std::size_t limit = first_batch;
  out.reserve(limit);
  const int fd = dirfd(d);
  while (auto *e = readdir(d))
  {
    const char *name = e->d_name;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
      continue;
    entry_t entry;
    entry.name = name;
    entry.hidden = name[0] == '.';
    struct stat st{};
    if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0)
    {
      if (S_ISLNK(st.st_mode))
      {
        entry.link = true;
        struct stat target{};
        if (fstatat(fd, name, &target, 0) == 0)
          st = target;
      }
      entry.directory = S_ISDIR(st.st_mode);
      entry.size = entry.directory ? 0 : std::uint64_t(st.st_size);
      entry.modified = std::int64_t(st.st_mtime);
    }
    out.push_back(std::move(entry));
    if (out.size() >= limit)
    {
      if (!batch(std::move(out)))
      {
        closedir(d);
        return std::chrono::steady_clock::now() - started;
      }
      out = {};
      limit = later_batch;
      out.reserve(limit);
    }
  }
  closedir(d);
  if (!out.empty())
    batch(std::move(out));
  return std::chrono::steady_clock::now() - started;
}

std::string home()
{
  auto h = env("HOME");
  if (h.empty())
  {
    if (auto *pw = getpwuid(getuid()))
      h = pw->pw_dir;
  }
  return h.empty() ? std::string("/") : h;
}

std::vector<place_t> places()
{
  const auto h = home();
  std::vector<place_t> out{{"Home", h, place_t::kind_t::home}};
  // XDG_DESKTOP_DIR="$HOME/Desktop" and the like, in the user's language.
  std::unordered_map<std::string, std::string> dirs;
  std::ifstream in(env("XDG_CONFIG_HOME", h + "/.config") + "/user-dirs.dirs");
  for (std::string line; std::getline(in, line);)
  {
    const auto eq = line.find('=');
    if (line.rfind("XDG_", 0) != 0 || eq == std::string::npos)
      continue;
    auto value = line.substr(eq + 1);
    if (value.size() >= 2 && value.front() == '"')
      value = value.substr(1, value.size() - 2);
    if (value.rfind("$HOME", 0) == 0)
      value = h + value.substr(5);
    dirs[line.substr(0, eq)] = value;
  }
  const struct
  {
    const char *name;
    const char *key;
    const char *fallback;
  } wanted[] = {{"Desktop", "XDG_DESKTOP_DIR", "Desktop"},  {"Documents", "XDG_DOCUMENTS_DIR", "Documents"}, {"Downloads", "XDG_DOWNLOAD_DIR", "Downloads"},
                {"Pictures", "XDG_PICTURES_DIR", "Pictures"}, {"Music", "XDG_MUSIC_DIR", "Music"},              {"Videos", "XDG_VIDEOS_DIR", "Videos"}};
  for (const auto &w : wanted)
  {
    auto it = dirs.find(w.key);
    const auto path = it != dirs.end() ? it->second : h + "/" + w.fallback;
    if (path != h && is_directory(path))
      out.push_back({w.name, path, place_t::kind_t::folder});
  }
  return out;
}

std::vector<place_t> drives()
{
  std::vector<place_t> out;
  const auto add = [&out](const std::string &name, const std::string &path)
  {
    place_t p{name, path, place_t::kind_t::drive};
    struct statvfs vfs{};
    if (statvfs(path.c_str(), &vfs) == 0)
    {
      p.total = std::uint64_t(vfs.f_blocks) * vfs.f_frsize;
      p.free = std::uint64_t(vfs.f_bavail) * vfs.f_frsize;
    }
    out.push_back(std::move(p));
  };
  add("File system", "/");
  std::ifstream mounts("/proc/mounts");
  for (std::string line; std::getline(mounts, line);)
  {
    std::istringstream words(line);
    std::string device, point;
    words >> device >> point;
    if (point.rfind("/media/", 0) == 0 || point.rfind("/mnt/", 0) == 0 || point.rfind("/run/media/", 0) == 0)
      add(point.substr(point.rfind('/') + 1), point);
  }
  return out;
}

std::string parent(const std::string &path)
{
  auto p = path;
  while (p.size() > 1 && p.back() == '/')
    p.pop_back();
  const auto slash = p.rfind('/');
  if (slash == std::string::npos)
    return path;
  return slash == 0 ? std::string("/") : p.substr(0, slash);
}

std::string join(const std::string &folder, const std::string &name)
{
  if (folder.empty() || folder.back() == '/')
    return folder + name;
  return folder + "/" + name;
}

std::vector<crumb_t> crumbs(const std::string &path)
{
  std::vector<crumb_t> out{{"/", "/"}};
  std::string at = "/";
  for (const auto &part : split(path, '/'))
  {
    at = join(at, part);
    out.push_back({part, at});
  }
  return out;
}

std::string type_name(const std::string &name)
{
  const auto ext = extension_of(name);
  if (ext.empty())
    return "File";
  const auto &t = mime_table();
  auto it = t.find(ext);
  std::string kind = "file";
  if (it != t.end())
  {
    const auto category = it->second.substr(0, it->second.find('/'));
    if (category == "text" || category == "image" || category == "audio" || category == "video")
      kind = category;
  }
  std::string upper = ext;
  std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return char(std::toupper(c)); });
  return upper + " " + kind;
}

std::vector<handler_t> handlers(const std::string &path)
{
  const auto mime = mime_of(path);
  // text/plain opens what any text/* does not claim specifically.
  const auto category = mime.substr(0, mime.find('/')) + "/*";
  std::vector<handler_t> out;
  const auto def = default_for(mime);
  for (const auto &app : applications())
  {
    const bool fits = std::any_of(app.mimes.begin(), app.mimes.end(), [&](const std::string &m) { return m == mime || m == category || (mime.rfind("text/", 0) == 0 && m == "text/plain"); });
    if (!fits)
      continue;
    handler_t h{app.name, app.path, app.id == def};
    if (h.is_default)
      out.insert(out.begin(), std::move(h));
    else
      out.push_back(std::move(h));
  }
  return out;
}

std::expected<void, std::string> open(const std::string &path)
{
  return spawn({"xdg-open", path});
}

std::expected<void, std::string> open_with(const std::string &path, const handler_t &handler)
{
  for (const auto &app : applications())
  {
    if (app.path == handler.id)
      return spawn(command_for(app.exec, path));
  }
  return std::unexpected(handler.name + " is no longer installed");
}

std::expected<void, std::string> open_terminal(const std::string &folder)
{
  std::vector<std::string> candidates;
  if (const auto t = env("TERMINAL"); !t.empty())
    candidates.push_back(t);
  for (const char *t : {"x-terminal-emulator", "gnome-terminal", "konsole", "xfce4-terminal", "alacritty", "kitty", "foot", "xterm"})
    candidates.emplace_back(t);
  for (const auto &t : candidates)
  {
    if (on_path(t))
      return spawn({t}, folder);
  }
  return std::unexpected(std::string("no terminal found"));
}

std::expected<void, std::string> reveal(const std::string &path)
{
  // The freedesktop file manager interface selects the item; without it, open the folder.
  if (on_path("dbus-send"))
  {
    auto r = spawn({"dbus-send", "--session", "--dest=org.freedesktop.FileManager1", "--type=method_call", "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1.ShowItems",
                    "array:string:file://" + path, "string:"});
    if (r)
      return r;
  }
  return spawn({"xdg-open", parent(path)});
}

bool has_properties()
{
  return false;
}

std::expected<void, std::string> properties(const std::string &)
{
  return std::unexpected(std::string("no properties window on this system"));
}

const char *file_manager_name()
{
  return "file manager";
}
} // namespace shell

#endif
