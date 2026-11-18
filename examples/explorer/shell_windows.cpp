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
#include <oleidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <optional>
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

std::string config_dir()
{
  const auto base = known_folder(FOLDERID_RoamingAppData);
  return base.empty() ? std::string() : base + "\\vui\\explorer";
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

// As a double click in Explorer does: the file's default verb, started in the file's folder. Left
// unset, a program would start in whatever folder the explorer itself was started in -- System32,
// when Windows starts it for Win+E -- and one that finds its files beside it would not start.
std::expected<void, std::string> open(const std::string &path)
{
  const auto file = wide(path);
  const auto folder = wide(parent(path));
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof info;
  info.fMask = SEE_MASK_NOASYNC;
  info.lpFile = file.c_str();
  info.lpDirectory = folder.empty() ? nullptr : folder.c_str();
  info.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&info))
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

namespace
{
bool file_there(const std::wstring &path)
{
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring environment(const wchar_t *name)
{
  wchar_t buffer[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(name, buffer, MAX_PATH);
  return n > 0 && n < MAX_PATH ? std::wstring(buffer) : std::wstring();
}

// One argument as CommandLineToArgvW reads it back: quoted, the backslashes before a quote doubled,
// so "C:\" stays a path and does not swallow the quote after it.
std::wstring quoted(const std::wstring &arg)
{
  std::wstring out = L"\"";
  std::size_t slashes = 0;
  for (const wchar_t c : arg)
  {
    if (c == L'\\')
    {
      ++slashes;
      continue;
    }
    out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0;
    out += c;
  }
  out.append(slashes * 2, L'\\');
  return out + L"\"";
}

// WezTerm's window program: on the PATH, or where its installer, a per-user install or Scoop put it.
const std::wstring &wezterm()
{
  static const std::wstring found = []
  {
    wchar_t path[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"wezterm-gui.exe", nullptr, MAX_PATH, path, nullptr) > 0)
      return std::wstring(path);
    for (const auto &candidate : {environment(L"ProgramW6432") + L"\\WezTerm\\wezterm-gui.exe", environment(L"ProgramFiles") + L"\\WezTerm\\wezterm-gui.exe",
                                  environment(L"LOCALAPPDATA") + L"\\Programs\\WezTerm\\wezterm-gui.exe", environment(L"USERPROFILE") + L"\\scoop\\apps\\wezterm\\current\\wezterm-gui.exe"})
    {
      if (file_there(candidate))
        return candidate;
    }
    return std::wstring();
  }();
  return found;
}

// ShellExecute's answer: 0 when it started, else the error, read before anything can change it.
DWORD execute(const wchar_t *verb, const std::wstring &file, const std::wstring &args, const std::wstring &directory)
{
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof info;
  info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  info.lpVerb = verb;
  info.lpFile = file.c_str();
  info.lpParameters = args.empty() ? nullptr : args.c_str();
  info.lpDirectory = directory.empty() ? nullptr : directory.c_str();
  info.nShow = SW_SHOWNORMAL;
  return ShellExecuteExW(&info) ? 0 : GetLastError();
}

std::expected<void, std::string> started(DWORD error)
{
  if (error == 0)
    return {};
  if (error == ERROR_CANCELLED)
    return std::unexpected(std::string("not started: the administrator prompt was declined"));
  return std::unexpected(error_text(error));
}
} // namespace

std::string terminal_name()
{
  return wezterm().empty() ? "Windows Terminal" : "WezTerm";
}

std::expected<void, std::string> open_terminal(const std::string &folder, bool elevated)
{
  // WezTerm when it is installed: `start --cwd` opens the shell its configuration names. Else
  // Windows Terminal, else the console host. An elevated process starts in System32 whatever it is
  // told, so the folder always goes in the arguments.
  const auto dir = wide(folder);
  const wchar_t *verb = elevated ? L"runas" : L"open";
  if (!wezterm().empty())
    return started(execute(verb, wezterm(), L"start --cwd " + quoted(dir), dir));
  if (const DWORD e = execute(verb, L"wt.exe", L"-d " + quoted(dir), dir); e == 0 || e == ERROR_CANCELLED)
    return started(e);
  return started(execute(verb, L"cmd.exe", L"/k cd /d " + quoted(dir), dir));
}

bool can_elevate()
{
  return true;
}

bool is_program(const std::string &name)
{
  const auto dot = name.rfind('.');
  if (dot == std::string::npos)
    return false;
  std::string ext = name.substr(dot + 1);
  for (auto &c : ext)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return ext == "exe" || ext == "com" || ext == "bat" || ext == "cmd" || ext == "msi";
}

std::expected<void, std::string> run_elevated(const std::string &path)
{
  return started(execute(L"runas", wide(path), L"", wide(parent(path))));
}

// ---- the default file manager (G67)

namespace detail
{
// Where the per-user classes are; --probe points it at a key of its own.
std::wstring classes = L"Software\\Classes";
} // namespace detail

namespace
{
constexpr wchar_t verb_name[] = L"vui-explorer";
// The value the default verb had before, kept beside it so restoring puts it back.
constexpr wchar_t previous_value[] = L"VuiExplorerPrevious";
// What Win+E opens.
constexpr wchar_t win_e[] = L"CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}";

std::wstring own_exe()
{
  std::wstring path(MAX_PATH, L'\0');
  for (;;)
  {
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (n < path.size())
    {
      path.resize(n);
      return path;
    }
    path.resize(path.size() * 2);
  }
}

std::wstring key_of(const std::wstring &relative)
{
  return detail::classes + L"\\" + relative;
}

std::optional<std::wstring> read_value(const std::wstring &key, const wchar_t *value)
{
  DWORD size = 0;
  if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS)
    return std::nullopt;
  std::wstring out(size / sizeof(wchar_t), L'\0');
  if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_SZ, nullptr, out.data(), &size) != ERROR_SUCCESS)
    return std::nullopt;
  out.resize(wcsnlen(out.c_str(), out.size()));
  return out;
}

LSTATUS write_value(const std::wstring &key, const wchar_t *value, const std::wstring &data)
{
  return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), value, REG_SZ, data.c_str(), DWORD((data.size() + 1) * sizeof(wchar_t)));
}

std::expected<void, std::string> written(LSTATUS status, const std::wstring &key)
{
  if (status == ERROR_SUCCESS)
    return {};
  return std::unexpected("cannot write HKEY_CURRENT_USER\\" + narrow(key.c_str()) + ": " + error_text(DWORD(status)));
}

// Removes the key and those above it, up to `stop`, that hold nothing at all -- no values, no keys
// -- or do not exist; the first that holds anything stays, and so does everything above it.
void remove_empty_up_to(std::wstring key, const std::wstring &stop)
{
  while (key.size() > stop.size())
  {
    HKEY handle = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &handle) == ERROR_SUCCESS)
    {
      DWORD subkeys = 0, values = 0;
      const bool read = RegQueryInfoKeyW(handle, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
      RegCloseKey(handle);
      if (!read || subkeys > 0 || values > 0 || RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str()) != ERROR_SUCCESS)
        return;
    }
    key.resize(key.rfind(L'\\'));
  }
}
} // namespace

bool can_be_default()
{
  return true;
}

bool is_default()
{
  const auto command = read_value(key_of(L"Directory\\shell\\") + verb_name + L"\\command", nullptr);
  const auto chosen = read_value(key_of(L"Directory\\shell"), nullptr);
  return command && chosen && *chosen == verb_name && command->find(own_exe()) != std::wstring::npos;
}

std::expected<void, std::string> make_default()
{
  const auto exe = own_exe();
  // Folders and drives: a verb of its own, made the default one. Not the Folder class, which also
  // holds This PC, the Recycle Bin and Control Panel, none of which this explorer can show.
  for (const wchar_t *type : {L"Directory", L"Drive"})
  {
    const auto shell = key_of(type) + L"\\shell";
    const auto verb = shell + L"\\" + verb_name;
    if (auto r = written(write_value(verb, nullptr, L"Open in vui explorer"), verb); !r)
      return r;
    if (auto r = written(write_value(verb + L"\\command", nullptr, quoted(exe) + L" --at \"%1\""), verb); !r)
      return r;
    const auto before = read_value(shell, nullptr);
    if (before && *before != verb_name && !read_value(shell, previous_value))
      (void)write_value(shell, previous_value, *before);
    if (auto r = written(write_value(shell, nullptr, verb_name), shell); !r)
      return r;
  }
  // Win+E: the command of the item it opens, with DelegateExecute empty so the command is what runs.
  const auto open_new = key_of(win_e) + L"\\shell\\opennewwindow\\command";
  if (auto r = written(write_value(open_new, nullptr, quoted(exe)), open_new); !r)
    return r;
  if (auto r = written(write_value(open_new, L"DelegateExecute", L""), open_new); !r)
    return r;
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return {};
}

std::vector<std::pair<bool, std::string>> detail::probe_default()
{
  std::vector<std::pair<bool, std::string>> out;
  const auto real = detail::classes;
  const std::wstring root = L"Software\\vui-explorer-probe";
  detail::classes = root + L"\\Classes";
  const auto classes = detail::classes;
  // Something of the user's own beside it, which restoring must leave alone.
  (void)write_value(classes + L"\\Directory\\shell\\theirs", nullptr, L"x");
  const auto made = make_default();
  out.emplace_back(made && is_default(), "registered as the default file manager, under a scratch key" + (made ? std::string() : ": " + made.error()));
  const auto drive = read_value(classes + L"\\Drive\\shell\\vui-explorer\\command", nullptr).value_or(L"");
  const auto win_e_command = read_value(classes + L"\\" + win_e + L"\\shell\\opennewwindow\\command", nullptr).value_or(L"");
  const auto delegate = read_value(classes + L"\\" + win_e + L"\\shell\\opennewwindow\\command", L"DelegateExecute");
  out.emplace_back(drive.find(own_exe()) != std::wstring::npos && drive.find(L"--at \"%1\"") != std::wstring::npos && read_value(classes + L"\\Drive\\shell", nullptr).value_or(L"") == verb_name &&
                     win_e_command.find(own_exe()) != std::wstring::npos && delegate && delegate->empty(),
                   "drives and Win+E point at this executable");
  const auto restored = restore_default();
  HKEY handle = nullptr;
  const bool verb_gone = RegOpenKeyExW(HKEY_CURRENT_USER, (classes + L"\\Directory\\shell\\vui-explorer").c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS;
  if (!verb_gone)
    RegCloseKey(handle);
  // Win+E's key goes; CLSID itself stays, as the real one, full of others' classes, always would.
  const bool clsid_gone = RegOpenKeyExW(HKEY_CURRENT_USER, (classes + L"\\" + win_e).c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS;
  if (!clsid_gone)
    RegCloseKey(handle);
  out.emplace_back(restored && !is_default() && verb_gone && clsid_gone && !read_value(classes + L"\\Directory\\shell", nullptr), "and unregistered without a trace");
  out.emplace_back(read_value(classes + L"\\Directory\\shell\\theirs", nullptr).value_or(L"") == L"x", "leaving what was there before alone");
  RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());
  RegDeleteKeyW(HKEY_CURRENT_USER, root.c_str());
  detail::classes = real;
  return out;
}

std::expected<void, std::string> restore_default()
{
  for (const wchar_t *type : {L"Directory", L"Drive"})
  {
    const auto shell = key_of(type) + L"\\shell";
    RegDeleteTreeW(HKEY_CURRENT_USER, (shell + L"\\" + verb_name).c_str());
    if (const auto chosen = read_value(shell, nullptr); chosen && *chosen == verb_name)
    {
      if (const auto before = read_value(shell, previous_value))
        (void)write_value(shell, nullptr, *before);
      else
        RegDeleteKeyValueW(HKEY_CURRENT_USER, shell.c_str(), nullptr);
    }
    RegDeleteKeyValueW(HKEY_CURRENT_USER, shell.c_str(), previous_value);
    remove_empty_up_to(shell, detail::classes);
  }
  const auto open_new = key_of(win_e) + L"\\shell\\opennewwindow";
  RegDeleteTreeW(HKEY_CURRENT_USER, open_new.c_str());
  remove_empty_up_to(open_new, key_of(L"CLSID"));
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return {};
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

std::expected<picture_t, std::string> thumbnail(const std::string &path, int pixels)
{
  // The shell's image for an item, the same Explorer's thumbnails are: asked for thumbnails only, so
  // a file with none is reported rather than drawn as its icon.
  com_ptr_t<IShellItemImageFactory> factory;
  if (FAILED(SHCreateItemFromParsingName(wide(path).c_str(), nullptr, IID_PPV_ARGS(factory.put()))))
    return std::unexpected(std::string("cannot reach it"));
  HBITMAP bitmap = nullptr;
  if (FAILED(factory->GetImage(SIZE{pixels, pixels}, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &bitmap)) || !bitmap)
    return std::unexpected(std::string("no thumbnail"));
  BITMAP info{};
  GetObjectW(bitmap, sizeof info, &info);
  picture_t out;
  out.width = info.bmWidth;
  out.height = std::abs(info.bmHeight);
  BITMAPINFO request{};
  request.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  request.bmiHeader.biWidth = out.width;
  // Negative: rows from the top.
  request.bmiHeader.biHeight = -out.height;
  request.bmiHeader.biPlanes = 1;
  request.bmiHeader.biBitCount = 32;
  request.bmiHeader.biCompression = BI_RGB;
  std::vector<std::uint8_t> bgra(std::size_t(out.width) * out.height * 4);
  HDC dc = GetDC(nullptr);
  const int rows = GetDIBits(dc, bitmap, 0, UINT(out.height), bgra.data(), &request, DIB_RGB_COLORS);
  ReleaseDC(nullptr, dc);
  DeleteObject(bitmap);
  if (rows != out.height || out.width <= 0)
    return std::unexpected(std::string("could not read the thumbnail"));
  // BGRA with premultiplied alpha, or no alpha at all (every alpha zero) for most photos.
  bool any_alpha = false;
  for (std::size_t i = 3; i < bgra.size(); i += 4)
    any_alpha = any_alpha || bgra[i] != 0;
  out.rgba.resize(bgra.size());
  for (std::size_t i = 0; i < bgra.size(); i += 4)
  {
    const std::uint8_t a = any_alpha ? bgra[i + 3] : 255;
    const auto straight = [a](std::uint8_t c) { return a == 0 || a == 255 ? c : std::uint8_t(std::min(255, c * 255 / a)); };
    out.rgba[i] = straight(bgra[i + 2]);
    out.rgba[i + 1] = straight(bgra[i + 1]);
    out.rgba[i + 2] = straight(bgra[i]);
    out.rgba[i + 3] = a;
  }
  return out;
}

std::expected<void, std::string> trash(const std::string &path)
{
  // The shell's own delete, with undo allowed: the Recycle Bin. No dialogs -- the explorer has
  // asked already -- and the list of paths ends with two NULs.
  auto from = wide(path);
  from.push_back(L'\0');
  from.push_back(L'\0');
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  const int r = SHFileOperationW(&op);
  if (r != 0)
    return std::unexpected("could not move it to the Recycle Bin (error " + std::to_string(r) + ")");
  if (op.fAnyOperationsAborted)
    return std::unexpected(std::string("moving it to the Recycle Bin was cancelled"));
  return {};
}

std::expected<void, std::string> remove_permanently(const std::string &path)
{
  // The same shell delete without undo: it also clears read-only files, which a plain remove_all
  // refuses on Windows.
  auto from = wide(path);
  from.push_back(L'\0');
  from.push_back(L'\0');
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  const int r = SHFileOperationW(&op);
  if (r != 0)
    return std::unexpected("could not delete it (error " + std::to_string(r) + ")");
  if (op.fAnyOperationsAborted)
    return std::unexpected(std::string("deleting it was cancelled"));
  return {};
}

const char *trash_name()
{
  return "Recycle Bin";
}

namespace
{
// A list of paths for SHFileOperation: each ends with a NUL, and the list with another.
std::wstring path_list(const std::vector<std::string> &paths)
{
  std::wstring out;
  for (const auto &p : paths)
  {
    out += wide(p);
    out.push_back(L'\0');
  }
  out.push_back(L'\0');
  return out;
}

std::wstring parent_of(std::wstring path)
{
  while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
    path.pop_back();
  const auto slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool same_path(const std::wstring &a, const std::wstring &b)
{
  auto x = a, y = b;
  while (!x.empty() && (x.back() == L'\\' || x.back() == L'/'))
    x.pop_back();
  while (!y.empty() && (y.back() == L'\\' || y.back() == L'/'))
    y.pop_back();
  std::replace(x.begin(), x.end(), L'/', L'\\');
  std::replace(y.begin(), y.end(), L'/', L'\\');
  return CompareStringOrdinal(x.c_str(), int(x.size()), y.c_str(), int(y.size()), TRUE) == CSTR_EQUAL;
}
} // namespace

std::expected<void, std::string> transfer(const std::vector<std::string> &items, const std::string &folder, bool move)
{
  // The shell's copy and move, which ask what to do when a name is taken and can be undone from
  // Explorer. Copying into the folder an item is in names the copy "name - Copy", as Explorer does;
  // the rest of a paste ask before replacing anything.
  const auto into = wide(folder);
  std::vector<std::string> beside, elsewhere;
  for (const auto &item : items)
  {
    const bool here = same_path(parent_of(wide(item)), into);
    if (here && move)
      continue;
    (here ? beside : elsewhere).push_back(item);
  }
  const auto run = [&](const std::vector<std::string> &from, FILEOP_FLAGS extra) -> std::expected<void, std::string>
  {
    if (from.empty())
      return {};
    const auto list = path_list(from);
    auto to = into;
    to.push_back(L'\0');
    to.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = move ? FO_MOVE : FO_COPY;
    op.pFrom = list.c_str();
    op.pTo = to.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | extra;
    const int r = SHFileOperationW(&op);
    if (op.fAnyOperationsAborted)
      return std::unexpected(std::string(move ? "the move was cancelled" : "the copy was cancelled"));
    if (r != 0)
      return std::unexpected(std::string(move ? "could not move it" : "could not copy it") + " (error " + std::to_string(r) + ")");
    return {};
  };
  if (auto r = run(beside, FOF_RENAMEONCOLLISION); !r)
    return r;
  return run(elsewhere, 0);
}

bool same_volume(const std::string &a, const std::string &b)
{
  wchar_t x[MAX_PATH] = {}, y[MAX_PATH] = {};
  if (!GetVolumePathNameW(wide(a).c_str(), x, MAX_PATH) || !GetVolumePathNameW(wide(b).c_str(), y, MAX_PATH))
    return false;
  return CompareStringOrdinal(x, -1, y, -1, TRUE) == CSTR_EQUAL;
}

std::expected<void, std::string> rename_item(const std::string &path, const std::string &name)
{
  if (name.empty() || name.find_first_of("\\/:*?\"<>|") != std::string::npos)
    return std::unexpected(std::string("a name cannot hold any of \\ / : * ? \" < > |"));
  const auto from = wide(path);
  const auto parent = parent_of(from);
  const auto to = parent + L"\\" + wide(name);
  // MoveFileEx without replacing: a name that is taken fails, and a change of case alone does not.
  if (!MoveFileExW(from.c_str(), to.c_str(), 0))
  {
    const DWORD code = GetLastError();
    if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS)
      return std::unexpected("there is already something called \"" + name + "\" here");
    return std::unexpected(error_text(code));
  }
  SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, from.c_str(), to.c_str());
  return {};
}

namespace
{
UINT drop_effect_format()
{
  static const UINT format = RegisterClipboardFormatW(L"Preferred DropEffect");
  return format;
}

// Windows lets one window at a time hold the clipboard open; another program may have it for a
// moment, so a few tries.
bool open_clipboard(void *owner)
{
  for (int attempt = 0; attempt < 10; ++attempt)
  {
    if (OpenClipboard(static_cast<HWND>(owner)))
      return true;
    Sleep(10);
  }
  return false;
}
} // namespace

std::expected<void, std::string> put_on_clipboard(const std::vector<std::string> &items, bool cut, void *owner)
{
  // CF_HDROP, a DROPFILES header and the paths after it, and the effect Explorer reads to tell a
  // cut from a copy.
  std::wstring paths;
  for (const auto &item : items)
  {
    paths += wide(item);
    paths.push_back(L'\0');
  }
  paths.push_back(L'\0');
  const SIZE_T bytes = sizeof(DROPFILES) + paths.size() * sizeof(wchar_t);
  HGLOBAL files = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
  HGLOBAL effect = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
  if (!files || !effect)
  {
    if (files)
      GlobalFree(files);
    if (effect)
      GlobalFree(effect);
    return std::unexpected(std::string("out of memory"));
  }
  auto *header = static_cast<DROPFILES *>(GlobalLock(files));
  header->pFiles = sizeof(DROPFILES);
  header->fWide = TRUE;
  std::memcpy(reinterpret_cast<char *>(header) + sizeof(DROPFILES), paths.data(), paths.size() * sizeof(wchar_t));
  GlobalUnlock(files);
  *static_cast<DWORD *>(GlobalLock(effect)) = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
  GlobalUnlock(effect);
  if (!open_clipboard(owner))
  {
    GlobalFree(files);
    GlobalFree(effect);
    return std::unexpected(std::string("another program has the clipboard open"));
  }
  EmptyClipboard();
  const bool put = SetClipboardData(CF_HDROP, files) != nullptr;
  if (!put)
    GlobalFree(files);
  if (!SetClipboardData(drop_effect_format(), effect))
    GlobalFree(effect);
  CloseClipboard();
  if (!put)
    return std::unexpected(std::string("could not put the files on the clipboard"));
  return {};
}

std::optional<clipboard_files_t> files_on_clipboard(void *owner)
{
  if (!IsClipboardFormatAvailable(CF_HDROP) || !open_clipboard(owner))
    return std::nullopt;
  std::optional<clipboard_files_t> out;
  if (HANDLE data = GetClipboardData(CF_HDROP))
  {
    if (auto drop = static_cast<HDROP>(GlobalLock(data)))
    {
      out.emplace();
      const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < count; ++i)
      {
        std::wstring name(DragQueryFileW(drop, i, nullptr, 0), L'\0');
        DragQueryFileW(drop, i, name.data(), UINT(name.size() + 1));
        out->items.push_back(narrow(name.c_str()));
      }
      GlobalUnlock(data);
    }
  }
  if (out)
  {
    if (HANDLE effect = GetClipboardData(drop_effect_format()))
    {
      if (const auto *value = static_cast<const DWORD *>(GlobalLock(effect)))
      {
        out->cut = (*value & DROPEFFECT_MOVE) != 0 && (*value & DROPEFFECT_COPY) == 0;
        GlobalUnlock(effect);
      }
    }
  }
  CloseClipboard();
  if (out && out->items.empty())
    out.reset();
  return out;
}

void clear_clipboard(void *owner)
{
  if (!open_clipboard(owner))
    return;
  EmptyClipboard();
  CloseClipboard();
}

namespace
{
bool exists(const std::string &path)
{
  return GetFileAttributesW(wide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string registry_string(HKEY root, const wchar_t *key, const wchar_t *value)
{
  wchar_t buffer[MAX_PATH * 2] = {};
  DWORD size = sizeof buffer;
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buffer, &size) != ERROR_SUCCESS)
    return {};
  return narrow(buffer);
}

std::string program_files()
{
  wchar_t buffer[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(L"ProgramW6432", buffer, MAX_PATH);
  if (n > 0 && n < MAX_PATH)
    return narrow(buffer);
  return "C:\\Program Files";
}

// One argument as CommandLineToArgvW reads it back: quoted, with the backslashes before a quote
// doubled.
void append_argument(std::wstring &line, const std::wstring &arg)
{
  if (!line.empty())
    line += L' ';
  line += L'"';
  std::size_t slashes = 0;
  for (const wchar_t c : arg)
  {
    if (c == L'\\')
    {
      ++slashes;
      continue;
    }
    if (c == L'"')
      line.append(slashes * 2 + 1, L'\\');
    else
      line.append(slashes, L'\\');
    slashes = 0;
    line += c;
  }
  line.append(slashes * 2, L'\\');
  line += L'"';
}
} // namespace

namespace detail
{
std::vector<archiver_t> find_archivers()
{
  std::vector<archiver_t> out;
  const auto files = program_files();
  // 7-Zip says where it is in the registry; the default folder otherwise.
  auto seven = registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\7-Zip", L"Path64");
  if (seven.empty())
    seven = registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\7-Zip", L"Path");
  if (seven.empty())
    seven = files + "\\7-Zip\\";
  if (!seven.empty() && seven.back() != '\\')
    seven += '\\';
  if (exists(seven + "7z.exe"))
    out.push_back({"7-Zip", archiver_t::kind_t::seven_zip, seven + "7z.exe", exists(seven + "7zFM.exe") ? seven + "7zFM.exe" : std::string(), {"zip", "7z"}});
  auto rar = registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WinRAR", L"exe64");
  if (rar.empty())
    rar = files + "\\WinRAR\\WinRAR.exe";
  if (exists(rar))
    out.push_back({"WinRAR", archiver_t::kind_t::winrar, rar, rar, {"rar", "zip"}});
  if (exists(files + "\\PeaZip\\peazip.exe"))
    out.push_back({"PeaZip", archiver_t::kind_t::gui_only, {}, files + "\\PeaZip\\peazip.exe", {}});
  // Windows itself has bsdtar, which reads most archives and writes zip.
  wchar_t system[MAX_PATH] = {};
  GetSystemDirectoryW(system, MAX_PATH);
  const auto tar = narrow(system) + "\\tar.exe";
  if (exists(tar))
    out.push_back({"Windows tar", archiver_t::kind_t::bsdtar, tar, {}, {"zip", "tar.gz"}});
  return out;
}

std::expected<int, std::string> run_tool(const std::vector<std::string> &argv, const std::string &directory)
{
  std::wstring line;
  for (const auto &a : argv)
    append_argument(line, wide(a));
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  PROCESS_INFORMATION process{};
  const auto dir = wide(directory);
  if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.empty() ? nullptr : dir.c_str(), &startup, &process))
    return std::unexpected("cannot start " + argv.front() + ": " + error_text(GetLastError()));
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return int(code);
}
} // namespace detail

std::expected<void, std::string> open_in_archiver(const archiver_t &tool, const std::string &archive)
{
  std::wstring args;
  append_argument(args, wide(archive));
  if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide(tool.gui).c_str(), args.c_str(), nullptr, SW_SHOWNORMAL)) <= 32)
    return std::unexpected(error_text(GetLastError()));
  return {};
}

const char *file_manager_name()
{
  return "Explorer";
}
} // namespace shell

#endif
