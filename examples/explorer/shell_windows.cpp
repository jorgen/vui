#if defined(_WIN32)

// Windows: FindFirstFileExW with a large fetch reads a folder with names, sizes, times and
// attributes in one pass; the known-folder API names the user's folders; the shell's association
// handlers are the "Open with" list, and invoking one is how Explorer itself opens a file with it.

#include "shell.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <knownfolders.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <unordered_map>

namespace shell
{
namespace
{
std::wstring wide(const std::string &s)
{
  if (s.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring out(std::size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
  return out;
}

std::string narrow(const wchar_t *s, int length = -1)
{
  if (!s)
    return {};
  if (length < 0)
    length = int(wcslen(s));
  if (length == 0)
    return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, length, nullptr, 0, nullptr, nullptr);
  std::string out(std::size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, length, out.data(), n, nullptr, nullptr);
  return out;
}

std::string error_text(DWORD code)
{
  wchar_t *buffer = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::string text = buffer ? narrow(buffer) : "error " + std::to_string(code);
  LocalFree(buffer);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.'))
    text.pop_back();
  return text;
}

// 100 ns ticks since 1601 to seconds since 1970.
std::int64_t unix_seconds(const FILETIME &t)
{
  const auto ticks = (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
  return std::int64_t(ticks / 10000000ull) - 11644473600ll;
}

std::string known_folder(REFKNOWNFOLDERID id)
{
  PWSTR path = nullptr;
  std::string out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)))
    out = narrow(path);
  CoTaskMemFree(path);
  return out;
}

std::string extension_of(const std::string &name)
{
  const auto dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0)
    return {};
  auto ext = name.substr(dot);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return ext;
}

template <typename T>
struct com_ptr_t
{
  T *p = nullptr;
  com_ptr_t() = default;
  com_ptr_t(const com_ptr_t &) = delete;
  com_ptr_t &operator=(const com_ptr_t &) = delete;
  ~com_ptr_t()
  {
    if (p)
      p->Release();
  }
  T **put() noexcept
  {
    return &p;
  }
  T *operator->() const noexcept
  {
    return p;
  }
  explicit operator bool() const noexcept
  {
    return p != nullptr;
  }
};
} // namespace

void initialize()
{
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
}

std::expected<std::chrono::nanoseconds, std::string> list(const std::string &path, const batch_t &batch)
{
  const auto started = std::chrono::steady_clock::now();
  std::vector<entry_t> out;
  out.reserve(first_batch);
  std::size_t limit = first_batch;
  auto pattern = wide(path);
  if (!pattern.empty() && pattern.back() != L'\\')
    pattern += L'\\';
  pattern += L'*';
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
  if (find == INVALID_HANDLE_VALUE)
  {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND)
      return std::chrono::steady_clock::now() - started;
    return std::unexpected(error_text(code));
  }
  do
  {
    const wchar_t *name = data.cFileName;
    if ((name[0] == L'.' && name[1] == 0) || (name[0] == L'.' && name[1] == L'.' && name[2] == 0))
      continue;
    entry_t e;
    e.name = narrow(name);
    e.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    e.hidden = (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
    e.link = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    e.size = e.directory ? 0 : (std::uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    e.modified = unix_seconds(data.ftLastWriteTime);
    out.push_back(std::move(e));
    if (out.size() >= limit)
    {
      if (!batch(std::move(out)))
      {
        FindClose(find);
        return std::chrono::steady_clock::now() - started;
      }
      out = {};
      limit = later_batch;
      out.reserve(limit);
    }
  } while (FindNextFileW(find, &data));
  FindClose(find);
  if (!out.empty())
    batch(std::move(out));
  return std::chrono::steady_clock::now() - started;
}

std::string home()
{
  return known_folder(FOLDERID_Profile);
}

std::vector<place_t> places()
{
  std::vector<place_t> out;
  out.push_back({"Home", home(), place_t::kind_t::home});
  const struct
  {
    const char *name;
    const KNOWNFOLDERID *id;
  } folders[] = {{"Desktop", &FOLDERID_Desktop}, {"Documents", &FOLDERID_Documents}, {"Downloads", &FOLDERID_Downloads},
                 {"Pictures", &FOLDERID_Pictures}, {"Music", &FOLDERID_Music},         {"Videos", &FOLDERID_Videos}};
  for (const auto &f : folders)
  {
    auto path = known_folder(*f.id);
    if (!path.empty())
      out.push_back({f.name, std::move(path), place_t::kind_t::folder});
  }
  return out;
}

std::vector<place_t> drives()
{
  std::vector<place_t> out;
  const DWORD mask = GetLogicalDrives();
  for (int i = 0; i < 26; ++i)
  {
    if (!(mask & (1u << i)))
      continue;
    const wchar_t root[] = {wchar_t(L'A' + i), L':', L'\\', 0};
    const UINT type = GetDriveTypeW(root);
    if (type == DRIVE_NO_ROOT_DIR)
      continue;
    place_t p;
    p.kind = place_t::kind_t::drive;
    p.path = narrow(root);
    wchar_t label[MAX_PATH + 1] = {};
    // Asking a removable or network drive for its label can stall; those keep a plain name.
    if (type == DRIVE_FIXED)
    {
      GetVolumeInformationW(root, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
      ULARGE_INTEGER free{}, total{};
      if (GetDiskFreeSpaceExW(root, &free, &total, nullptr))
      {
        p.total = total.QuadPart;
        p.free = free.QuadPart;
      }
    }
    const std::string letter = narrow(root, 2);
    p.name = label[0] ? narrow(label) + " (" + letter + ")" : (type == DRIVE_REMOTE ? "Network (" + letter + ")" : "Drive (" + letter + ")");
    out.push_back(std::move(p));
  }
  return out;
}

std::string parent(const std::string &path)
{
  auto p = path;
  while (p.size() > 3 && (p.back() == '\\' || p.back() == '/'))
    p.pop_back();
  const auto slash = p.find_last_of("\\/");
  if (slash == std::string::npos)
    return path;
  if (slash <= 2)
    return p.substr(0, 3);
  return p.substr(0, slash);
}

std::string join(const std::string &folder, const std::string &name)
{
  if (folder.empty())
    return name;
  if (folder.back() == '\\' || folder.back() == '/')
    return folder + name;
  return folder + "\\" + name;
}

std::vector<crumb_t> crumbs(const std::string &path)
{
  std::vector<crumb_t> out;
  if (path.size() < 2)
    return out;
  std::string at = path.substr(0, 2) + "\\";
  out.push_back({path.substr(0, 2), at});
  std::size_t i = 3;
  while (i < path.size())
  {
    const auto next = path.find_first_of("\\/", i);
    const auto name = path.substr(i, next == std::string::npos ? std::string::npos : next - i);
    if (!name.empty())
    {
      at = join(at, name);
      out.push_back({name, at});
    }
    if (next == std::string::npos)
      break;
    i = next + 1;
  }
  return out;
}

std::string type_name(const std::string &name)
{
  // The shell's name for a type costs a registry walk, so each extension is asked once.
  static std::mutex mutex;
  static std::unordered_map<std::string, std::string> cache;
  const auto ext = extension_of(name);
  std::lock_guard lock(mutex);
  if (auto it = cache.find(ext); it != cache.end())
    return it->second;
  SHFILEINFOW info{};
  std::string type;
  if (SHGetFileInfoW(wide(ext.empty() ? std::string("file") : ext).c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof info, SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES))
    type = narrow(info.szTypeName);
  if (type.empty())
    type = ext.empty() ? "File" : ext.substr(1) + " file";
  return cache.emplace(ext, type).first->second;
}

std::vector<handler_t> handlers(const std::string &path)
{
  std::vector<handler_t> out;
  const auto ext = extension_of(path);
  if (ext.empty())
    return out;
  com_ptr_t<IEnumAssocHandlers> list;
  if (FAILED(SHAssocEnumHandlers(wide(ext).c_str(), ASSOC_FILTER_RECOMMENDED, list.put())) || !list)
    return out;
  for (;;)
  {
    com_ptr_t<IAssocHandler> handler;
    ULONG got = 0;
    if (list->Next(1, handler.put(), &got) != S_OK || got == 0)
      break;
    LPWSTR name = nullptr;
    LPWSTR ui = nullptr;
    handler_t h;
    if (SUCCEEDED(handler->GetName(&name)))
      h.id = narrow(name);
    if (SUCCEEDED(handler->GetUIName(&ui)))
      h.name = narrow(ui);
    CoTaskMemFree(name);
    CoTaskMemFree(ui);
    if (h.name.empty() || std::any_of(out.begin(), out.end(), [&](const handler_t &o) { return o.name == h.name; }))
      continue;
    out.push_back(std::move(h));
  }
  // The default is whatever the association names for "open".
  wchar_t exe[MAX_PATH * 2] = {};
  DWORD size = DWORD(std::size(exe));
  if (SUCCEEDED(AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_EXECUTABLE, wide(ext).c_str(), L"open", exe, &size)))
  {
    const auto def = narrow(exe);
    for (auto &h : out)
    {
      if (_stricmp(h.id.c_str(), def.c_str()) == 0)
      {
        h.is_default = true;
        std::rotate(out.begin(), out.begin() + (&h - out.data()), out.begin() + (&h - out.data()) + 1);
        break;
      }
    }
  }
  return out;
}

std::expected<void, std::string> open(const std::string &path)
{
  const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (r <= 32)
    return std::unexpected(error_text(GetLastError()));
  return {};
}

std::expected<void, std::string> open_with(const std::string &path, const handler_t &handler)
{
  // Found again by its id: a handler is a COM object of the enumeration that made it.
  const auto ext = extension_of(path);
  com_ptr_t<IEnumAssocHandlers> list;
  if (FAILED(SHAssocEnumHandlers(wide(ext).c_str(), ASSOC_FILTER_RECOMMENDED, list.put())) || !list)
    return std::unexpected(std::string("no handlers for ") + ext);
  com_ptr_t<IShellItem> item;
  if (FAILED(SHCreateItemFromParsingName(wide(path).c_str(), nullptr, IID_PPV_ARGS(item.put()))))
    return std::unexpected(std::string("cannot reach ") + path);
  com_ptr_t<IDataObject> data;
  if (FAILED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(data.put()))))
    return std::unexpected(std::string("cannot hand over ") + path);
  for (;;)
  {
    com_ptr_t<IAssocHandler> h;
    ULONG got = 0;
    if (list->Next(1, h.put(), &got) != S_OK || got == 0)
      break;
    LPWSTR name = nullptr;
    const bool match = SUCCEEDED(h->GetName(&name)) && narrow(name) == handler.id;
    CoTaskMemFree(name);
    if (match)
    {
      if (FAILED(h->Invoke(data.p)))
        return std::unexpected("could not start " + handler.name);
      return {};
    }
  }
  return std::unexpected(handler.name + " no longer opens " + ext);
}

std::expected<void, std::string> open_terminal(const std::string &folder)
{
  // Windows Terminal when it is there, the console host otherwise.
  const auto dir = wide(folder);
  const std::wstring args = L"-d \"" + dir + L"\"";
  if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"wt.exe", args.c_str(), nullptr, SW_SHOWNORMAL)) > 32)
    return {};
  if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"cmd.exe", nullptr, dir.c_str(), SW_SHOWNORMAL)) > 32)
    return {};
  return std::unexpected(error_text(GetLastError()));
}

std::expected<void, std::string> reveal(const std::string &path)
{
  const std::wstring args = L"/select,\"" + wide(path) + L"\"";
  if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL)) <= 32)
    return std::unexpected(error_text(GetLastError()));
  return {};
}

bool has_properties()
{
  return true;
}

std::expected<void, std::string> properties(const std::string &path)
{
  if (!SHObjectProperties(nullptr, SHOP_FILEPATH, wide(path).c_str(), nullptr))
    return std::unexpected(std::string("no properties for ") + path);
  return {};
}

const char *file_manager_name()
{
  return "Explorer";
}
} // namespace shell

#endif
