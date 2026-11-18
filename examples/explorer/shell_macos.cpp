#if defined(__APPLE__)

// macOS: readdir and fstatat read a folder, the user's folders are fixed names under the home
// folder, volumes are /Volumes, and LaunchServices says which applications open a file. Starting
// things goes through /usr/bin/open, which is what LaunchServices does for a shell.

#include "shell.h"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreServices/CoreServices.h>
#include <QuickLook/QuickLook.h>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <cerrno>
#include <cstdlib>
#include <cstring>

extern char **environ;

namespace shell
{
namespace
{
bool is_directory(const std::string &path)
{
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string cf_string(CFStringRef s)
{
  if (!s)
    return {};
  const auto length = CFStringGetLength(s);
  std::string out(std::size_t(CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8)) + 1, '\0');
  if (!CFStringGetCString(s, out.data(), CFIndex(out.size()), kCFStringEncodingUTF8))
    return {};
  out.resize(std::strlen(out.c_str()));
  return out;
}

std::string path_of(CFURLRef url)
{
  char buffer[PATH_MAX];
  if (!url || !CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8 *>(buffer), sizeof buffer))
    return {};
  return buffer;
}

CFURLRef url_for(const std::string &path)
{
  return CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8 *>(path.c_str()), CFIndex(path.size()), is_directory(path));
}

// An application's name: its bundle's file name without ".app".
std::string app_name(const std::string &path)
{
  auto name = path.substr(path.rfind('/') + 1);
  if (name.size() > 4 && name.compare(name.size() - 4, 4, ".app") == 0)
    name.resize(name.size() - 4);
  return name;
}

std::expected<void, std::string> run(std::vector<std::string> argv)
{
  std::vector<char *> args;
  for (auto &a : argv)
    args.push_back(a.data());
  args.push_back(nullptr);
  pid_t pid = 0;
  if (const int e = posix_spawn(&pid, args[0], nullptr, nullptr, args.data(), environ); e != 0)
    return std::unexpected(argv[0] + ": " + std::strerror(e));
  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return std::unexpected(std::string("open could not start it"));
  return {};
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
      entry.hidden = entry.hidden || (st.st_flags & UF_HIDDEN) != 0;
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
  const char *h = std::getenv("HOME");
  if (h && *h)
    return h;
  if (auto *pw = getpwuid(getuid()))
    return pw->pw_dir;
  return "/";
}

std::string config_dir()
{
  return home() + "/Library/Application Support/vui/explorer";
}

std::vector<place_t> places()
{
  const auto h = home();
  std::vector<place_t> out{{"Home", h, place_t::kind_t::home}};
  for (const auto &[name, folder] : {std::pair{"Desktop", "Desktop"}, {"Documents", "Documents"}, {"Downloads", "Downloads"}, {"Pictures", "Pictures"}, {"Music", "Music"}, {"Movies", "Movies"}})
  {
    const auto path = h + "/" + folder;
    if (is_directory(path))
      out.push_back({name, path, place_t::kind_t::folder});
  }
  out.push_back({"Applications", "/Applications", place_t::kind_t::folder});
  return out;
}

std::vector<place_t> drives()
{
  std::vector<place_t> out;
  const auto add = [&out](const std::string &name, const std::string &path)
  {
    place_t p{name, path, place_t::kind_t::drive};
    struct statfs fs{};
    if (statfs(path.c_str(), &fs) == 0)
    {
      p.total = std::uint64_t(fs.f_blocks) * fs.f_bsize;
      p.free = std::uint64_t(fs.f_bavail) * fs.f_bsize;
    }
    out.push_back(std::move(p));
  };
  add("Macintosh HD", "/");
  if (DIR *d = opendir("/Volumes"))
  {
    while (auto *e = readdir(d))
    {
      if (e->d_name[0] == '.')
        continue;
      const std::string path = std::string("/Volumes/") + e->d_name;
      // The boot volume appears here too, as a link to /.
      struct stat st{};
      if (lstat(path.c_str(), &st) == 0 && !S_ISLNK(st.st_mode))
        add(e->d_name, path);
    }
    closedir(d);
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
  std::size_t i = 1;
  while (i < path.size())
  {
    const auto next = path.find('/', i);
    const auto part = path.substr(i, next == std::string::npos ? std::string::npos : next - i);
    if (!part.empty())
    {
      at = join(at, part);
      out.push_back({part, at});
    }
    if (next == std::string::npos)
      break;
    i = next + 1;
  }
  return out;
}

std::string type_name(const std::string &name)
{
  const auto dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0)
    return "File";
  auto ext = name.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::toupper(c)); });
  return ext + " file";
}

std::vector<handler_t> handlers(const std::string &path)
{
  std::vector<handler_t> out;
  CFURLRef url = url_for(path);
  if (!url)
    return out;
  std::string def;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  if (CFURLRef d = LSCopyDefaultApplicationURLForURL(url, kLSRolesAll, nullptr))
  {
    def = path_of(d);
    CFRelease(d);
  }
  if (CFArrayRef apps = LSCopyApplicationURLsForURL(url, kLSRolesAll))
  {
    for (CFIndex i = 0; i < CFArrayGetCount(apps); ++i)
    {
      const auto app = path_of(static_cast<CFURLRef>(CFArrayGetValueAtIndex(apps, i)));
      if (app.empty() || std::any_of(out.begin(), out.end(), [&](const handler_t &h) { return h.id == app; }))
        continue;
      handler_t h{app_name(app), app, app == def};
      if (h.is_default)
        out.insert(out.begin(), std::move(h));
      else
        out.push_back(std::move(h));
    }
    CFRelease(apps);
  }
#pragma clang diagnostic pop
  CFRelease(url);
  return out;
}

std::expected<void, std::string> open(const std::string &path)
{
  return run({"/usr/bin/open", path});
}

std::expected<void, std::string> open_with(const std::string &path, const handler_t &handler)
{
  return run({"/usr/bin/open", "-a", handler.id, path});
}

namespace
{
// WezTerm's app bundle, wherever the app was put.
std::string wezterm()
{
  for (const std::string app : {std::string("/Applications/WezTerm.app"), std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/Applications/WezTerm.app"})
  {
    if (access((app + "/Contents/MacOS/wezterm").c_str(), X_OK) == 0)
      return app;
  }
  return {};
}
} // namespace

std::string terminal_name()
{
  return wezterm().empty() ? "Terminal" : "WezTerm";
}

std::expected<void, std::string> open_terminal(const std::string &folder, bool elevated)
{
  if (elevated)
    return std::unexpected(std::string("running as administrator is a Windows feature"));
  // WezTerm first, which opens the shell its configuration names.
  // Through open, which returns at once; -n makes a new instance take the arguments.
  if (const auto w = wezterm(); !w.empty())
    return run({"/usr/bin/open", "-na", w, "--args", "start", "--cwd", folder});
  return run({"/usr/bin/open", "-a", "Terminal", folder});
}

bool can_elevate()
{
  return false;
}

bool is_program(const std::string &)
{
  return false;
}

std::expected<void, std::string> run_elevated(const std::string &)
{
  return std::unexpected(std::string("running as administrator is a Windows feature"));
}

bool can_be_default()
{
  return false;
}

bool is_default()
{
  return false;
}

std::expected<void, std::string> make_default()
{
  return std::unexpected(std::string("only on Windows"));
}

std::expected<void, std::string> restore_default()
{
  return std::unexpected(std::string("only on Windows"));
}

std::expected<void, std::string> reveal(const std::string &path)
{
  return run({"/usr/bin/open", "-R", path});
}

bool has_properties()
{
  return false;
}

std::expected<void, std::string> properties(const std::string &)
{
  return std::unexpected(std::string("no properties window on this system"));
}

std::expected<picture_t, std::string> thumbnail(const std::string &path, int pixels)
{
  // QuickLook's thumbnail, what Finder shows; the C call is deprecated in favour of
  // QLThumbnailGenerator, which would need Objective-C, and still works.
  CFURLRef url = url_for(path);
  if (!url)
    return std::unexpected(std::string("cannot reach it"));
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  CGImageRef image = QLThumbnailImageCreate(kCFAllocatorDefault, url, CGSizeMake(pixels, pixels), nullptr);
#pragma clang diagnostic pop
  CFRelease(url);
  if (!image)
    return std::unexpected(std::string("no thumbnail"));
  picture_t out;
  out.width = int(CGImageGetWidth(image));
  out.height = int(CGImageGetHeight(image));
  out.rgba.assign(std::size_t(out.width) * out.height * 4, 0);
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef context = CGBitmapContextCreate(out.rgba.data(), std::size_t(out.width), std::size_t(out.height), 8, std::size_t(out.width) * 4, srgb, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
  if (context)
  {
    CGContextDrawImage(context, CGRectMake(0, 0, out.width, out.height), image);
    CGContextRelease(context);
  }
  CGColorSpaceRelease(srgb);
  CGImageRelease(image);
  if (!context)
    return std::unexpected(std::string("could not draw the thumbnail"));
  // Premultiplied out of Core Graphics, straight for the caller.
  for (std::size_t i = 0; i < out.rgba.size(); i += 4)
  {
    const int a = out.rgba[i + 3];
    if (a == 0 || a == 255)
      continue;
    for (int c = 0; c < 3; ++c)
      out.rgba[i + std::size_t(c)] = std::uint8_t(std::min(255, out.rgba[i + std::size_t(c)] * 255 / a));
  }
  return out;
}

std::expected<pdf_page_t, std::string> pdf_page(const std::string &path, int page, int pixels)
{
  CFURLRef url = url_for(path);
  if (!url)
    return std::unexpected(std::string("cannot reach it"));
  CGPDFDocumentRef document = CGPDFDocumentCreateWithURL(url);
  CFRelease(url);
  if (!document)
    return std::unexpected(std::string("not a PDF Core Graphics can read"));
  if (!CGPDFDocumentIsUnlocked(document))
  {
    CGPDFDocumentRelease(document);
    return std::unexpected(std::string("the PDF is locked with a password"));
  }
  pdf_page_t out;
  out.pages = int(CGPDFDocumentGetNumberOfPages(document));
  if (out.pages <= 0)
  {
    CGPDFDocumentRelease(document);
    return std::unexpected(std::string("the PDF has no pages"));
  }
  out.page = std::clamp(page, 0, out.pages - 1);
  CGPDFPageRef pdf_page = CGPDFDocumentGetPage(document, std::size_t(out.page) + 1);
  // The crop box, turned as the page says it is shown.
  const CGRect box = CGPDFPageGetBoxRect(pdf_page, kCGPDFCropBox);
  const int rotation = ((CGPDFPageGetRotationAngle(pdf_page) % 360) + 360) % 360;
  const bool sideways = rotation == 90 || rotation == 270;
  const double w = sideways ? box.size.height : box.size.width;
  const double h = sideways ? box.size.width : box.size.height;
  const double scale = double(pixels) / std::max(1.0, std::max(w, h));
  out.picture.width = std::max(1, int(w * scale));
  out.picture.height = std::max(1, int(h * scale));
  out.picture.rgba.assign(std::size_t(out.picture.width) * out.picture.height * 4, 255);
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef context = CGBitmapContextCreate(out.picture.rgba.data(), std::size_t(out.picture.width), std::size_t(out.picture.height), 8, std::size_t(out.picture.width) * 4, srgb,
                                               kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
  CGColorSpaceRelease(srgb);
  if (!context)
  {
    CGPDFDocumentRelease(document);
    return std::unexpected(std::string("could not draw the page"));
  }
  // White under it, as a viewer shows paper; the page's own transform fits it, rotation and all.
  CGContextSetRGBFillColor(context, 1, 1, 1, 1);
  CGContextFillRect(context, CGRectMake(0, 0, out.picture.width, out.picture.height));
  CGContextConcatCTM(context, CGPDFPageGetDrawingTransform(pdf_page, kCGPDFCropBox, CGRectMake(0, 0, out.picture.width, out.picture.height), 0, true));
  CGContextDrawPDFPage(context, pdf_page);
  CGContextRelease(context);
  CGPDFDocumentRelease(document);
  return out;
}

std::expected<void, std::string> trash(const std::string &path)
{
  // The File Manager's move to the Trash: deprecated in favour of NSFileManager, which would need
  // Objective-C, and still what Finder's own Trash is.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  char *moved = nullptr;
  const OSStatus status = FSPathMoveObjectToTrashSync(path.c_str(), &moved, kFSFileOperationDefaultOptions);
#pragma clang diagnostic pop
  std::free(moved);
  if (status != noErr)
    return std::unexpected("could not move it to the Trash (error " + std::to_string(int(status)) + ")");
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
std::vector<archiver_t> find_archivers()
{
  std::vector<archiver_t> out;
  const auto exists = [](const std::string &p) { return access(p.c_str(), F_OK) == 0; };
  for (const char *p : {"/opt/homebrew/bin/7zz", "/usr/local/bin/7zz", "/opt/homebrew/bin/7z", "/usr/local/bin/7z"})
  {
    if (exists(p))
    {
      out.push_back({"7-Zip", archiver_t::kind_t::seven_zip, p, {}, {"zip", "7z"}});
      break;
    }
  }
  // macOS's own tar is bsdtar, which reads most archives and writes zip.
  if (exists("/usr/bin/tar"))
    out.push_back({"tar", archiver_t::kind_t::bsdtar, "/usr/bin/tar", {}, {"zip", "tar.gz"}});
  for (const auto &[name, app] : {std::pair{"Archive Utility", "/System/Library/CoreServices/Applications/Archive Utility.app"}, {"Keka", "/Applications/Keka.app"}, {"The Unarchiver", "/Applications/The Unarchiver.app"}})
  {
    if (exists(app))
      out.push_back({name, archiver_t::kind_t::gui_only, {}, app, {}});
  }
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
  const int e = posix_spawn(&pid, args[0], &actions, nullptr, args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (e != 0)
    return std::unexpected(argv.front() + ": " + std::strerror(e));
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}
} // namespace detail

std::expected<void, std::string> open_in_archiver(const archiver_t &tool, const std::string &archive)
{
  return run({"/usr/bin/open", "-a", tool.gui, archive});
}

const char *file_manager_name()
{
  return "Finder";
}
} // namespace shell

#endif
