// Archives (G59): what each archive tool reads and writes, and the command lines that extract and
// compress with it -- the same on every platform. Finding the tools and running a command is each
// platform's, in shell_<platform>.cpp.

#include "shell.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace shell
{
namespace
{
std::string lower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

// The archive suffix of a name, compressed tars as one: "tar.gz", "zip", or empty.
std::string suffix_of(const std::string &name)
{
  const auto n = lower(name);
  for (const char *two : {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lz", ".tar.lzma"})
  {
    const std::string s = two;
    if (n.size() > s.size() && n.compare(n.size() - s.size(), s.size(), s) == 0)
      return s.substr(1);
  }
  const auto dot = n.rfind('.');
  return dot == std::string::npos || dot == 0 ? std::string() : n.substr(dot + 1);
}

bool is_tar(const std::string &suffix)
{
  return suffix == "tar" || suffix.rfind("tar.", 0) == 0 || suffix == "tgz" || suffix == "tbz" || suffix == "tbz2" || suffix == "txz" || suffix == "tzst";
}

std::filesystem::path native(const std::string &s)
{
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(s.data()), s.size()));
}

std::string base_name(const std::string &path)
{
  return path.substr(std::min(path.size(), path.find_last_of("\\/") + 1));
}

#if defined(_WIN32)
constexpr char separator = '\\';
#else
constexpr char separator = '/';
#endif

std::expected<void, std::string> finished(const std::expected<int, std::string> &run, const archiver_t &tool, int worst_ok)
{
  if (!run)
    return std::unexpected(run.error());
  if (*run > worst_ok)
    return std::unexpected(tool.name + " stopped with code " + std::to_string(*run));
  return {};
}
} // namespace

const std::vector<archiver_t> &archivers()
{
  static const std::vector<archiver_t> found = detail::find_archivers();
  return found;
}

bool is_archive(const std::string &name)
{
  const auto s = suffix_of(name);
  return is_tar(s) || s == "zip" || s == "7z" || s == "rar" || s == "gz" || s == "bz2" || s == "xz" || s == "zst" || s == "cab" || s == "iso" || s == "jar" || s == "lzh" || s == "arj";
}

bool can_extract(const archiver_t &tool, const std::string &name)
{
  const auto s = suffix_of(name);
  switch (tool.kind)
  {
  case archiver_t::kind_t::seven_zip:
  case archiver_t::kind_t::winrar:
    // A compressed tar takes these two passes, the compression and then the tar, and one pass
    // leaves a .tar where the folder should be; tar does both at once.
    return is_archive(name) && !(is_tar(s) && s != "tar");
  case archiver_t::kind_t::bsdtar:
    // libarchive reads all of these, rar and 7z included.
    return is_archive(name) && s != "lzh" && s != "arj";
  case archiver_t::kind_t::gnu_tar:
    return is_tar(s);
  case archiver_t::kind_t::info_zip:
    return s == "zip" || s == "jar";
  default:
    return false;
  }
}

std::expected<void, std::string> extract(const archiver_t &tool, const std::string &archive, const std::string &into)
{
  std::error_code error;
  std::filesystem::create_directories(native(into), error);
  if (error)
    return std::unexpected("cannot make " + into + ": " + error.message());
  switch (tool.kind)
  {
  case archiver_t::kind_t::seven_zip:
    // -aos skips files that exist; 1 is 7-Zip's code for warnings, which skipping one is.
    return finished(detail::run_tool({tool.program, "x", archive, "-o" + into, "-aos", "-y", "-bso0", "-bsp0"}, into), tool, 1);
  case archiver_t::kind_t::winrar:
    return finished(detail::run_tool({tool.program, "x", "-ibck", "-o-", "-y", archive, into + separator}, into), tool, 1);
  case archiver_t::kind_t::bsdtar:
    // -k keeps files that exist.
    return finished(detail::run_tool({tool.program, "-x", "-k", "-f", archive, "-C", into}, into), tool, 0);
  case archiver_t::kind_t::gnu_tar:
    // GNU tar's -k keeps them too, but then exits with an error; --skip-old-files keeps them quietly.
    return finished(detail::run_tool({tool.program, "-x", "--skip-old-files", "-f", archive, "-C", into}, into), tool, 0);
  case archiver_t::kind_t::info_zip:
    return finished(detail::run_tool({tool.program, "-n", "-q", archive, "-d", into}, into), tool, 0);
  default:
    return std::unexpected(tool.name + " has no command line to extract with");
  }
}

std::expected<void, std::string> compress(const archiver_t &tool, const std::string &item, const std::string &archive)
{
  if (std::filesystem::exists(native(archive)))
    return std::unexpected(archive + " is there already");
  const auto folder = parent(item);
  const auto name = base_name(item);
  const auto s = suffix_of(archive);
  switch (tool.kind)
  {
  case archiver_t::kind_t::seven_zip:
    return finished(detail::run_tool({tool.program, "a", s == "7z" ? "-t7z" : "-tzip", archive, name, "-y", "-bso0", "-bsp0"}, folder), tool, 1);
  case archiver_t::kind_t::winrar:
  {
    std::vector<std::string> argv{tool.program, "a", "-ibck", "-ep1", "-r"};
    if (s == "zip")
      argv.push_back("-afzip");
    argv.push_back(archive);
    argv.push_back(item);
    return finished(detail::run_tool(argv, folder), tool, 1);
  }
  case archiver_t::kind_t::bsdtar:
  case archiver_t::kind_t::gnu_tar:
    // -a picks the format from the archive's suffix: zip with libarchive, the compressed tars with both.
    return finished(detail::run_tool({tool.program, "-a", "-c", "-f", archive, name}, folder), tool, 0);
  case archiver_t::kind_t::info_zip:
  {
    auto zip = tool.program;
    const auto slash = zip.find_last_of("\\/");
    zip = (slash == std::string::npos ? std::string() : zip.substr(0, slash + 1)) + "zip";
    return finished(detail::run_tool({zip, "-r", "-q", archive, name}, folder), tool, 0);
  }
  default:
    return std::unexpected(tool.name + " has no command line to compress with");
  }
}
} // namespace shell
