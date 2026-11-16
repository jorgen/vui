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
#include <array>
#include <filesystem>
#include <cerrno>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include <signal.h>
#include <spawn.h>
#include <sys/types.h>

// PNG only: what is decoded here is a thumbnail the system's own thumbnailer made, never the user's
// file itself (G57).
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

extern char **environ;

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

// MD5, for the freedesktop thumbnail cache, which names a thumbnail by the hash of its file's URI.
std::string md5_hex(const std::string &message)
{
  static constexpr std::array<std::uint32_t, 64> k = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  static constexpr std::array<int, 64> r = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  std::uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  std::vector<std::uint8_t> data(message.begin(), message.end());
  const std::uint64_t bits = std::uint64_t(data.size()) * 8;
  data.push_back(0x80);
  while (data.size() % 64 != 56)
    data.push_back(0);
  for (int i = 0; i < 8; ++i)
    data.push_back(std::uint8_t(bits >> (8 * i)));
  for (std::size_t chunk = 0; chunk < data.size(); chunk += 64)
  {
    std::uint32_t w[16];
    for (int i = 0; i < 16; ++i)
      w[i] = std::uint32_t(data[chunk + i * 4]) | std::uint32_t(data[chunk + i * 4 + 1]) << 8 | std::uint32_t(data[chunk + i * 4 + 2]) << 16 | std::uint32_t(data[chunk + i * 4 + 3]) << 24;
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; ++i)
    {
      std::uint32_t f;
      int g;
      if (i < 16)
      {
        f = (b & c) | (~b & d);
        g = i;
      }
      else if (i < 32)
      {
        f = (d & b) | (~d & c);
        g = (5 * i + 1) % 16;
      }
      else if (i < 48)
      {
        f = b ^ c ^ d;
        g = (3 * i + 5) % 16;
      }
      else
      {
        f = c ^ (b | ~d);
        g = (7 * i) % 16;
      }
      const std::uint32_t t = d;
      d = c;
      c = b;
      const std::uint32_t x = a + f + k[std::size_t(i)] + w[g];
      b = b + ((x << r[std::size_t(i)]) | (x >> (32 - r[std::size_t(i)])));
      a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
  }
  std::string out;
  char hex[3];
  for (auto word : h)
  {
    for (int i = 0; i < 4; ++i)
    {
      std::snprintf(hex, sizeof hex, "%02x", (word >> (8 * i)) & 0xff);
      out += hex;
    }
  }
  return out;
}

std::string file_uri(const std::string &path)
{
  std::string out = "file://";
  for (const unsigned char c : path)
  {
    if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
      out += char(c);
    else
    {
      char b[4];
      std::snprintf(b, sizeof b, "%%%02X", c);
      out += b;
    }
  }
  return out;
}

std::expected<picture_t, std::string> load_png(const std::string &file)
{
  const auto bytes = [&]
  {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }();
  if (bytes.empty())
    return std::unexpected(std::string("no thumbnail"));
  int w = 0, h = 0, channels = 0;
  stbi_uc *pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(bytes.data()), int(bytes.size()), &w, &h, &channels, 4);
  if (!pixels)
    return std::unexpected(std::string("the thumbnail could not be read"));
  picture_t out;
  out.width = w;
  out.height = h;
  out.rgba.assign(pixels, pixels + std::size_t(w) * h * 4);
  stbi_image_free(pixels);
  return out;
}

// Runs a thumbnailer and waits for it, five seconds at most: it is a program the system installed,
// but it parses a file nobody vouched for, and a hung one must not hold the explorer's helper.
bool run_and_wait(const std::vector<std::string> &argv)
{
  std::vector<char *> args;
  for (const auto &a : argv)
    args.push_back(const_cast<char *>(a.c_str()));
  args.push_back(nullptr);
  pid_t pid = 0;
  if (posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), environ) != 0)
    return false;
  for (int waited = 0; waited < 500; ++waited)
  {
    int status = 0;
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid)
      return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (r < 0)
      return false;
    usleep(10000);
  }
  kill(pid, SIGKILL);
  waitpid(pid, nullptr, 0);
  return false;
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

std::string config_dir()
{
  return env("XDG_CONFIG_HOME", home() + "/.config") + "/vui/explorer";
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

// The freedesktop.org Trash specification: the item moves into Trash/files under a name not yet
// taken there, and Trash/info gets a .trashinfo saying where it came from and when, which is what
// lets a file manager restore it. Only the home trash: something on another file system cannot be
// renamed into it, and that is reported rather than copied.
std::expected<picture_t, std::string> thumbnail(const std::string &path, int pixels)
{
  // The freedesktop.org thumbnail cache first: a file manager has usually made one already, named by
  // the MD5 of the file's URI, and it counts only if it is newer than the file.
  const auto uri = file_uri(path);
  const auto name = md5_hex(uri) + ".png";
  const auto cache = env("XDG_CACHE_HOME", home() + "/.cache") + "/thumbnails/";
  struct stat source{};
  if (::stat(path.c_str(), &source) != 0)
    return std::unexpected(std::string("cannot reach it"));
  const std::vector<std::string> sizes = pixels > 256 ? std::vector<std::string>{"xx-large", "x-large", "large", "normal"} : std::vector<std::string>{"large", "x-large", "normal"};
  for (const auto &size : sizes)
  {
    const auto file = cache + size + "/" + name;
    struct stat cached{};
    if (::stat(file.c_str(), &cached) == 0 && cached.st_mtime >= source.st_mtime)
    {
      if (auto picture = load_png(file))
        return picture;
    }
  }
  // Else a thumbnailer for its type, as the desktop installed them, into a file of our own.
  const auto mime = mime_of(path);
  std::vector<std::string> dirs{env("XDG_DATA_HOME", home() + "/.local/share")};
  for (auto &d : split(env("XDG_DATA_DIRS", "/usr/local/share:/usr/share"), ':'))
    dirs.push_back(d);
  for (const auto &dir : dirs)
  {
    DIR *d = opendir((dir + "/thumbnailers").c_str());
    if (!d)
      continue;
    std::vector<std::string> entries;
    while (auto *e = readdir(d))
      entries.emplace_back(e->d_name);
    closedir(d);
    for (const auto &entry : entries)
    {
      if (entry.size() < 13 || entry.compare(entry.size() - 12, 12, ".thumbnailer") != 0)
        continue;
      std::ifstream in(dir + "/thumbnailers/" + entry);
      std::string exec, try_exec;
      bool fits = false;
      for (std::string line; std::getline(in, line);)
      {
        if (line.rfind("Exec=", 0) == 0)
          exec = line.substr(5);
        else if (line.rfind("TryExec=", 0) == 0)
          try_exec = line.substr(8);
        else if (line.rfind("MimeType=", 0) == 0)
        {
          for (const auto &m : split(line.substr(9), ';'))
            fits = fits || m == mime;
        }
      }
      if (!fits || exec.empty() || (!try_exec.empty() && !on_path(try_exec)))
        continue;
      char out_name[] = "/tmp/vui-thumbnail-XXXXXX.png";
      const int fd = mkstemps(out_name, 4);
      if (fd < 0)
        continue;
      ::close(fd);
      // Field codes: %i the file, %u its URI, %o where to write, %s the size.
      std::vector<std::string> argv;
      for (const auto &word : split(exec, ' '))
      {
        std::string arg;
        for (std::size_t i = 0; i < word.size(); ++i)
        {
          if (word[i] == '%' && i + 1 < word.size())
          {
            const char code = word[++i];
            if (code == 'i')
              arg += path;
            else if (code == 'u')
              arg += uri;
            else if (code == 'o')
              arg += out_name;
            else if (code == 's')
              arg += std::to_string(pixels);
            else if (code == '%')
              arg += '%';
            continue;
          }
          arg += word[i];
        }
        argv.push_back(arg);
      }
      std::expected<picture_t, std::string> picture = std::unexpected(std::string("no thumbnail"));
      if (!argv.empty() && run_and_wait(argv))
        picture = load_png(out_name);
      ::unlink(out_name);
      if (picture)
        return picture;
    }
  }
  return std::unexpected(std::string("no thumbnail"));
}

std::expected<void, std::string> trash(const std::string &path)
{
  const auto base = env("XDG_DATA_HOME", home() + "/.local/share") + "/Trash";
  const auto files = base + "/files";
  const auto info = base + "/info";
  // The whole path: a fresh home has no ~/.local yet, and the trash is made on first use.
  std::error_code made;
  std::filesystem::create_directories(files, made);
  std::filesystem::create_directories(info, made);
  if (made)
    return std::unexpected("cannot make the Trash: " + made.message());
  ::chmod(base.c_str(), 0700);
  auto name = path.substr(path.rfind('/') + 1);
  std::string taken = name;
  int fd = -1;
  for (int n = 1; n < 10000; ++n)
  {
    fd = ::open((info + "/" + taken + ".trashinfo").c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd >= 0)
      break;
    if (errno != EEXIST)
      return std::unexpected("cannot write to the Trash: " + std::string(std::strerror(errno)));
    taken = name + "." + std::to_string(n);
  }
  if (fd < 0)
    return std::unexpected(std::string("the Trash has too many items of that name"));
  // Path= is percent-encoded, all but the unreserved characters and the slash.
  std::string encoded;
  for (const unsigned char c : path)
  {
    if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
      encoded += char(c);
    else
    {
      char b[4];
      std::snprintf(b, sizeof b, "%%%02X", c);
      encoded += b;
    }
  }
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  char date[32];
  std::strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &local);
  const auto record = "[Trash Info]\nPath=" + encoded + "\nDeletionDate=" + date + "\n";
  const bool written = ::write(fd, record.data(), record.size()) == ssize_t(record.size());
  ::close(fd);
  if (!written || ::rename(path.c_str(), (files + "/" + taken).c_str()) != 0)
  {
    const int e = written ? errno : EIO;
    ::unlink((info + "/" + taken + ".trashinfo").c_str());
    if (e == EXDEV)
      return std::unexpected(std::string("it is on another file system than the Trash"));
    return std::unexpected("cannot move it to the Trash: " + std::string(std::strerror(e)));
  }
  return {};
}

std::expected<void, std::string> remove_permanently(const std::string &path)
{
  std::error_code error;
  std::filesystem::remove_all(std::filesystem::path(path), error);
  if (error)
    return std::unexpected("could not delete it: " + error.message());
  return {};
}

const char *trash_name()
{
  return "Trash";
}

namespace detail
{
std::string which(const std::string &program)
{
  for (const auto &dir : split(env("PATH", "/usr/bin:/bin"), ':'))
  {
    const auto full = dir + "/" + program;
    if (access(full.c_str(), X_OK) == 0)
      return full;
  }
  return {};
}

std::vector<archiver_t> find_archivers()
{
  std::vector<archiver_t> out;
  for (const char *seven : {"7zz", "7z", "7za"})
  {
    if (const auto p = which(seven); !p.empty())
    {
      out.push_back({"7-Zip", archiver_t::kind_t::seven_zip, p, {}, {"zip", "7z"}});
      break;
    }
  }
  if (const auto p = which("bsdtar"); !p.empty())
    out.push_back({"bsdtar", archiver_t::kind_t::bsdtar, p, {}, {"zip", "tar.gz"}});
  if (const auto p = which("tar"); !p.empty())
    out.push_back({"tar", archiver_t::kind_t::gnu_tar, p, {}, {"tar.gz"}});
  if (const auto p = which("unzip"); !p.empty())
    out.push_back({"Info-ZIP", archiver_t::kind_t::info_zip, p, {}, which("zip").empty() ? std::vector<std::string>{} : std::vector<std::string>{"zip"}});
  if (const auto p = which("file-roller"); !p.empty())
    out.push_back({"File Roller", archiver_t::kind_t::gui_only, {}, p, {}});
  if (const auto p = which("ark"); !p.empty())
    out.push_back({"Ark", archiver_t::kind_t::gui_only, {}, p, {}});
  return out;
}

std::expected<int, std::string> run_tool(const std::vector<std::string> &argv, const std::string &directory)
{
  std::vector<char *> args;
  for (const auto &a : argv)
    args.push_back(const_cast<char *>(a.c_str()));
  args.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (!directory.empty())
    posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
  pid_t pid = 0;
  const int e = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (e != 0)
    return std::unexpected("cannot start " + argv.front() + ": " + std::strerror(e));
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}
} // namespace detail

std::expected<void, std::string> open_in_archiver(const archiver_t &tool, const std::string &archive)
{
  return spawn({tool.gui, archive});
}

const char *file_manager_name()
{
  return "file manager";
}
} // namespace shell

#endif
