/*
Copyright (c) 2026 Jørgen Lind

  Permission is hereby granted, free of charge, to any person obtaining a copy of
  this software and associated documentation files (the "Software"), to deal in
  the Software without restriction, including without limitation the rights to
  use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
  of the Software, and to permit persons to whom the Software is furnished to do
  so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "vui/backends.h"
#include "vui/vui.h"
#include "vio/host_loop.h"
#include "win32_common.h"

#include <windows.h>
#include <windowsx.h>

#include <dwmapi.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>

namespace vui::detail
{
namespace
{
constexpr UINT wake_message = WM_APP + 1;
// Posted by start_drag, so the drag's modal loop runs from the message pump, as a size loop does,
// and never inside a behaviour (G70).
constexpr UINT drag_message = WM_APP + 2;
constexpr UINT_PTR modal_timer = 1;
constexpr ULONG_PTR forwarder_exit_key = 0x7669'6f65'7869'74ull;
constexpr wchar_t window_class[] = L"vio_window";
constexpr wchar_t helper_class[] = L"vio_helper";

std::wstring widen(std::string_view text)
{
  if (text.empty())
    return {};
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
  return out;
}

std::string narrow(const wchar_t *text, int length)
{
  if (length == 0)
    return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), bytes, nullptr, nullptr);
  return out;
}

key_t translate_key(WPARAM vk, LPARAM lparam)
{
  const bool extended = (lparam >> 24) & 1;
  if (vk >= 'A' && vk <= 'Z')
    return static_cast<key_t>(static_cast<int>(key_t::a) + static_cast<int>(vk - 'A'));
  if (vk >= '0' && vk <= '9')
    return static_cast<key_t>(static_cast<int>(key_t::digit_0) + static_cast<int>(vk - '0'));
  if (vk >= VK_F1 && vk <= VK_F12)
    return static_cast<key_t>(static_cast<int>(key_t::f1) + static_cast<int>(vk - VK_F1));
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
    return static_cast<key_t>(static_cast<int>(key_t::keypad_0) + static_cast<int>(vk - VK_NUMPAD0));
  switch (vk)
  {
  case VK_SPACE: return key_t::space;
  case VK_OEM_7: return key_t::apostrophe;
  case VK_OEM_COMMA: return key_t::comma;
  case VK_OEM_MINUS: return key_t::minus;
  case VK_OEM_PERIOD: return key_t::period;
  case VK_OEM_2: return key_t::slash;
  case VK_OEM_1: return key_t::semicolon;
  case VK_OEM_PLUS: return key_t::equal;
  case VK_OEM_4: return key_t::left_bracket;
  case VK_OEM_5: return key_t::backslash;
  case VK_OEM_6: return key_t::right_bracket;
  case VK_OEM_3: return key_t::grave_accent;
  case VK_ESCAPE: return key_t::escape;
  case VK_RETURN: return extended ? key_t::keypad_enter : key_t::enter;
  case VK_TAB: return key_t::tab;
  case VK_BACK: return key_t::backspace;
  case VK_INSERT: return key_t::insert;
  case VK_DELETE: return key_t::delete_key;
  case VK_RIGHT: return key_t::right;
  case VK_LEFT: return key_t::left;
  case VK_DOWN: return key_t::down;
  case VK_UP: return key_t::up;
  case VK_PRIOR: return key_t::page_up;
  case VK_NEXT: return key_t::page_down;
  case VK_HOME: return key_t::home;
  case VK_END: return key_t::end;
  case VK_CAPITAL: return key_t::caps_lock;
  case VK_SCROLL: return key_t::scroll_lock;
  case VK_NUMLOCK: return key_t::num_lock;
  case VK_SNAPSHOT: return key_t::print_screen;
  case VK_PAUSE: return key_t::pause;
  case VK_DECIMAL: return key_t::keypad_decimal;
  case VK_DIVIDE: return key_t::keypad_divide;
  case VK_MULTIPLY: return key_t::keypad_multiply;
  case VK_SUBTRACT: return key_t::keypad_subtract;
  case VK_ADD: return key_t::keypad_add;
  case VK_SHIFT: return ((lparam >> 16) & 0xff) == 0x36 ? key_t::right_shift : key_t::left_shift;
  case VK_CONTROL: return extended ? key_t::right_control : key_t::left_control;
  case VK_MENU: return extended ? key_t::right_alt : key_t::left_alt;
  case VK_LWIN: return key_t::left_super;
  case VK_RWIN: return key_t::right_super;
  case VK_APPS: return key_t::menu;
  default: return key_t::unknown;
  }
}

modifier_t current_modifiers()
{
  auto modifiers = modifier_t::none;
  if (GetKeyState(VK_SHIFT) & 0x8000)
    modifiers = modifiers | modifier_t::shift;
  if (GetKeyState(VK_CONTROL) & 0x8000)
    modifiers = modifiers | modifier_t::control;
  if (GetKeyState(VK_MENU) & 0x8000)
    modifiers = modifiers | modifier_t::alt;
  if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)
    modifiers = modifiers | modifier_t::super;
  if (GetKeyState(VK_CAPITAL) & 1)
    modifiers = modifiers | modifier_t::caps_lock;
  if (GetKeyState(VK_NUMLOCK) & 1)
    modifiers = modifiers | modifier_t::num_lock;
  return modifiers;
}

LPCWSTR cursor_resource(cursor_t cursor)
{
  switch (cursor)
  {
  case cursor_t::text: return MAKEINTRESOURCEW(32513);
  case cursor_t::hand: return MAKEINTRESOURCEW(32649);
  case cursor_t::crosshair: return MAKEINTRESOURCEW(32515);
  case cursor_t::resize_horizontal: return MAKEINTRESOURCEW(32644);
  case cursor_t::resize_vertical: return MAKEINTRESOURCEW(32645);
  case cursor_t::resize_all: return MAKEINTRESOURCEW(32646);
  case cursor_t::not_allowed: return MAKEINTRESOURCEW(32648);
  case cursor_t::wait: return MAKEINTRESOURCEW(32514);
  default: return MAKEINTRESOURCEW(32512);
  }
}

std::int64_t refresh_interval(std::int64_t configured)
{
  if (configured > 0)
    return configured;
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
    return 1'000'000'000 / static_cast<std::int64_t>(mode.dmDisplayFrequency);
  return 16'666'667;
}

class win32_display_t;

class win32_thread_t final : public vio::host_wait_t
{
public:
  explicit win32_thread_t(vio::event_loop_t &loop)
    : loop(loop)
  {
  }

  ~win32_thread_t() override
  {
    stop_forwarder();
    if (_arm)
      CloseHandle(_arm);
    if (_finished)
      CloseHandle(_finished);
    if (helper)
      DestroyWindow(helper);
    if (ole)
      OleUninitialize();
  }

  int start()
  {
    // Drag and drop needs OLE in a single-threaded apartment; a thread already in the
    // multithreaded one gets no drag and drop rather than no windows.
    ole = SUCCEEDED(OleInitialize(nullptr));
    _port = reinterpret_cast<HANDLE>(vio::loop_native_handle(loop));
    if (!_port || _port == INVALID_HANDLE_VALUE)
      return vio::errc::not_supported;
    helper = CreateWindowExW(0, helper_class, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), this);
    if (!helper)
      return vio::errc::out_of_memory;
    _arm = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    _finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!_arm || !_finished)
      return vio::errc::out_of_memory;
    _forwarder = std::thread([this] {
      forward();
      SetEvent(_finished);
    });
    if (auto installed = vio::loop_set_host_wait(loop, this); !installed)
      return installed.error().code;
    return 0;
  }

  void wait(std::optional<std::chrono::nanoseconds> timeout) override
  {
    const std::int64_t timeout_ns = detail::timeout_ns(timeout);
    if (timeout_ns == 0)
      return;
    DWORD milliseconds = INFINITE;
    if (timeout_ns > 0)
    {
      const std::int64_t rounded = timeout_ns / 1'000'000 + (timeout_ns % 1'000'000 != 0 ? 1 : 0);
      milliseconds = static_cast<DWORD>(std::min<std::int64_t>(rounded, INFINITE - 1));
    }
    SetEvent(_arm);
    MsgWaitForMultipleObjectsEx(0, nullptr, milliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  }

  void dispatch()
  {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
      if (message.message == WM_QUIT)
        continue;
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }

  void enter_modal()
  {
    if (modal++ == 0)
      arm_modal_timer();
  }

  void leave_modal()
  {
    if (modal > 0 && --modal == 0)
      KillTimer(helper, modal_timer);
  }

  void nested_turn()
  {
    vio::loop_run_nested(loop);
    if (modal > 0)
      arm_modal_timer();
  }

  vio::event_loop_t &loop;
  HWND helper = nullptr;
  bool ole = false;
  int modal = 0;
  std::size_t displays = 0;

private:
  void arm_modal_timer()
  {
    const std::int64_t next = detail::timeout_ns(vio::loop_next_timeout(loop));
    UINT milliseconds = USER_TIMER_MINIMUM;
    if (next > 0)
      milliseconds = static_cast<UINT>(std::clamp<std::int64_t>(next / 1'000'000, USER_TIMER_MINIMUM, 1000));
    SetTimer(helper, modal_timer, milliseconds, nullptr);
  }

  void forward()
  {
    std::array<OVERLAPPED_ENTRY, 64> entries{};
    for (;;)
    {
      WaitForSingleObject(_arm, INFINITE);
      if (_exiting.load(std::memory_order_acquire))
        return;
      ULONG count = 0;
      if (!GetQueuedCompletionStatusEx(_port, entries.data(), static_cast<ULONG>(entries.size()), &count, INFINITE, FALSE))
        continue;
      bool exit = false;
      for (ULONG i = 0; i < count; ++i)
      {
        if (entries[i].lpCompletionKey == forwarder_exit_key)
        {
          exit = true;
          continue;
        }
        PostQueuedCompletionStatus(_port, entries[i].dwNumberOfBytesTransferred, entries[i].lpCompletionKey, entries[i].lpOverlapped);
      }
      PostMessageW(helper, wake_message, 0, 0);
      if (exit || _exiting.load(std::memory_order_acquire))
        return;
    }
  }

  void stop_forwarder()
  {
    if (!_forwarder.joinable())
      return;
    (void)vio::loop_set_host_wait(loop, nullptr);
    _exiting.store(true, std::memory_order_release);
    for (;;)
    {
      SetEvent(_arm);
      PostQueuedCompletionStatus(_port, 0, forwarder_exit_key, nullptr);
      if (WaitForSingleObject(_finished, 10) == WAIT_OBJECT_0)
        break;
    }
    _forwarder.join();
  }

  HANDLE _port = nullptr;
  HANDLE _arm = nullptr;
  HANDLE _finished = nullptr;
  std::thread _forwarder;
  std::atomic<bool> _exiting{false};
};

thread_local std::weak_ptr<win32_thread_t> current_thread;

LRESULT CALLBACK helper_procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
  if (message == WM_NCCREATE)
  {
    auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto *thread = reinterpret_cast<win32_thread_t *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (thread && (message == wake_message || (message == WM_TIMER && wparam == modal_timer)))
  {
    if (thread->modal > 0)
      thread->nested_turn();
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

class win32_window_t final : public window_backend_t
{
public:
  win32_window_t(win32_display_t &display, window_impl_t &window);
  ~win32_window_t() override;

  int create(const window_config_t &config);
  LRESULT procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

  void set_title(std::string_view title) override;
  int set_size(extent_t size) override;
  int set_position(point_t position) override;
  [[nodiscard]] std::expected<point_t, int> position() const override;
  void set_size_limits(extent_t min_size, extent_t max_size) override;
  void set_visible(bool visible) override;
  void set_fullscreen(bool fullscreen) override;
  void minimize() override;
  void maximize() override;
  void restore() override;
  void focus() override;
  void set_cursor(cursor_t cursor) override;
  void frame_wanted() override;
  [[nodiscard]] native_window_t native() const override;
  bool start_drag(std::vector<std::string> paths, drop_action_t allowed) override;

  // What the window's drop target hears (G70).
  DWORD drop(drop_phase_t phase, IDataObject *data, DWORD keys, POINTL at, DWORD allowed);

  HWND handle = nullptr;

private:
  static void tick(void *context);
  [[nodiscard]] float scale() const;
  [[nodiscard]] RECT outer_rect(extent_t client) const;
  void report_size();
  void turn_until_drawn();
  void button(button_t which, bool pressed, LPARAM lparam);
  void run_drag();

  win32_display_t &_display;
  window_impl_t &_window;
  frame_clock_t _clock;
  extent_t _min_size;
  extent_t _max_size;
  DWORD _style = 0;
  DWORD _ex_style = 0;
  frame_theme_t _frame_theme = frame_theme_t::system;
  void apply_frame_theme();
  RECT _windowed{};
  DWORD _windowed_style = 0;
  cursor_t _cursor = cursor_t::arrow;
  WCHAR _high_surrogate = 0;
  int _buttons = 0;
  // The drop target registered for the window, and what is being dragged over it.
  IDropTarget *_drop_target = nullptr;
  IDropTargetHelper *_drop_helper = nullptr;
  std::shared_ptr<const drag_data_t> _dragged_in;
  // A drag asked for and not yet begun.
  std::vector<std::string> _drag_paths;
  drop_action_t _drag_allowed = drop_action_t::none;
  bool _drag_pending = false;
  bool _tracking = false;
  bool _fullscreen = false;
  bool _resizable = true;
};

class win32_display_t final : public display_backend_t
{
public:
  explicit win32_display_t(display_impl_t &display)
    : impl(display)
  {
  }

  ~win32_display_t() override
  {
    if (thread && --thread->displays == 0)
      current_thread.reset();
  }

  int open()
  {
    win32_enable_dpi_awareness();
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW helper_info{};
    helper_info.cbSize = sizeof(helper_info);
    helper_info.lpfnWndProc = &helper_procedure;
    helper_info.hInstance = instance;
    helper_info.lpszClassName = helper_class;
    RegisterClassExW(&helper_info);
    WNDCLASSEXW info{};
    info.cbSize = sizeof(info);
    info.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    info.lpfnWndProc = &win32_display_t::procedure;
    info.hInstance = instance;
    info.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    info.lpszClassName = window_class;
    RegisterClassExW(&info);

    thread = current_thread.lock();
    if (!thread)
    {
      thread = std::make_shared<win32_thread_t>(*impl.loop);
      if (int result = thread->start(); result < 0)
        return result;
      current_thread = thread;
    }
    else if (&thread->loop != impl.loop)
    {
      return vio::errc::busy;
    }
    ++thread->displays;
    return 0;
  }

  [[nodiscard]] const char *name() const noexcept override
  {
    return "win32";
  }
  [[nodiscard]] platform_t platform() const noexcept override
  {
    return platform_t::win32;
  }

  std::expected<std::unique_ptr<window_backend_t>, int> create_window(window_impl_t &window, const window_config_t &config) override
  {
    auto made = std::make_unique<win32_window_t>(*this, window);
    if (int result = made->create(config); result < 0)
      return std::unexpected(result);
    return made;
  }

  void dispatch() override
  {
    thread->dispatch();
  }

  void set_clipboard_text(std::string text) override
  {
    if (!OpenClipboard(thread->helper))
      return;
    EmptyClipboard();
    const std::wstring wide = widen(text);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
    if (memory)
    {
      auto *target = static_cast<wchar_t *>(GlobalLock(memory));
      std::copy(wide.begin(), wide.end(), target);
      target[wide.size()] = 0;
      GlobalUnlock(memory);
      if (!SetClipboardData(CF_UNICODETEXT, memory))
        GlobalFree(memory);
    }
    CloseClipboard();
  }

  void request_clipboard_text() override
  {
    if (!OpenClipboard(thread->helper))
    {
      impl.clipboard_arrived(std::unexpected(vio::errc::busy));
      return;
    }
    std::string text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
    {
      if (const auto *wide = static_cast<const wchar_t *>(GlobalLock(data)))
      {
        text = narrow(wide, static_cast<int>(wcslen(wide)));
        GlobalUnlock(data);
      }
    }
    CloseClipboard();
    impl.clipboard_arrived(std::move(text));
  }

  [[nodiscard]] void *native_display() const noexcept override
  {
    return GetModuleHandleW(nullptr);
  }


  static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
  {
    if (message == WM_NCCREATE)
    {
      auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
      auto *window = static_cast<win32_window_t *>(create->lpCreateParams);
      window->handle = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    }
    auto *window = reinterpret_cast<win32_window_t *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!window)
      return DefWindowProcW(hwnd, message, wparam, lparam);
    return window->procedure(hwnd, message, wparam, lparam);
  }

  display_impl_t &impl;
  std::shared_ptr<win32_thread_t> thread;
};

// ---- drag and drop (G70)

// The window's IDropTarget: OLE calls it while something is dragged over the window, from another
// program or from this one, and it passes each call on to the window.
class drop_target_t final : public IDropTarget
{
public:
  explicit drop_target_t(win32_window_t &window) noexcept
    : window(&window)
  {
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
  {
    if (riid == IID_IUnknown || riid == IID_IDropTarget)
    {
      *out = static_cast<IDropTarget *>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override
  {
    return ++_references;
  }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const ULONG left = --_references;
    if (left == 0)
      delete this;
    return left;
  }

  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD keys, POINTL at, DWORD *effect) override
  {
    *effect = window ? window->drop(drop_phase_t::enter, data, keys, at, *effect) : DROPEFFECT_NONE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL at, DWORD *effect) override
  {
    *effect = window ? window->drop(drop_phase_t::over, nullptr, keys, at, *effect) : DROPEFFECT_NONE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragLeave() override
  {
    if (window)
      window->drop(drop_phase_t::leave, nullptr, 0, {}, DROPEFFECT_NONE);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD keys, POINTL at, DWORD *effect) override
  {
    *effect = window ? window->drop(drop_phase_t::drop, data, keys, at, *effect) : DROPEFFECT_NONE;
    return S_OK;
  }

  // Cleared when the window goes, before the registration is revoked: OLE may hold this longer.
  win32_window_t *window;

private:
  std::atomic<ULONG> _references{1};
};

// Paths and text, as far as the data object has them.
std::shared_ptr<const drag_data_t> read_drag_data(IDataObject *data)
{
  auto out = std::make_shared<drag_data_t>();
  FORMATETC files{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  if (SUCCEEDED(data->GetData(&files, &medium)))
  {
    if (auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal)))
    {
      const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < count; ++i)
      {
        std::wstring name(DragQueryFileW(drop, i, nullptr, 0), L'\0');
        DragQueryFileW(drop, i, name.data(), UINT(name.size() + 1));
        out->paths.push_back(narrow(name.c_str(), int(name.size())));
      }
      GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
  }
  FORMATETC text{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  if (SUCCEEDED(data->GetData(&text, &medium)))
  {
    if (const auto *wide = static_cast<const wchar_t *>(GlobalLock(medium.hGlobal)))
    {
      out->text = narrow(wide, int(wcslen(wide)));
      GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
  }
  return out;
}

// One of the actions allowed, from the answer: the answer itself when it is one.
DWORD one_effect(DWORD effects) noexcept
{
  if (effects & DROPEFFECT_MOVE)
    return DROPEFFECT_MOVE;
  if (effects & DROPEFFECT_COPY)
    return DROPEFFECT_COPY;
  if (effects & DROPEFFECT_LINK)
    return DROPEFFECT_LINK;
  return DROPEFFECT_NONE;
}

// The picture that follows the pointer: the first item as the shell draws it, its thumbnail or its
// icon. Without one Windows shows a blank box wherever a target draws the drag image.
SHDRAGIMAGE drag_image(const std::string &path)
{
  SHDRAGIMAGE image{};
  image.crColorKey = CLR_NONE;
  IShellItemImageFactory *factory = nullptr;
  if (FAILED(SHCreateItemFromParsingName(widen(path).c_str(), nullptr, IID_PPV_ARGS(&factory))))
    return image;
  HBITMAP bitmap = nullptr;
  if (SUCCEEDED(factory->GetImage(SIZE{96, 96}, SIIGBF_BIGGERSIZEOK, &bitmap)) && bitmap)
  {
    BITMAP info{};
    GetObjectW(bitmap, sizeof(info), &info);
    image.hbmpDragImage = bitmap;
    image.sizeDragImage = {info.bmWidth, std::abs(info.bmHeight)};
    // Held near its bottom middle, as Explorer holds what it drags.
    image.ptOffset = {info.bmWidth / 2, std::abs(info.bmHeight) * 3 / 4};
  }
  factory->Release();
  return image;
}

// ---- The drag image over windows that do not draw it (G72). A drop target that forwards to
// IDropTargetHelper draws the shell's image and says so by setting "IsShowingLayered" on the data
// object (1 on enter, 0 on leave); over any other window the drag source draws the same picture
// itself, in a layered window that follows the pointer, so a drag looks the same everywhere.

int read_flag(IDataObject *data, const wchar_t *format)
{
  FORMATETC f{CLIPFORMAT(RegisterClipboardFormatW(format)), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  if (FAILED(data->GetData(&f, &medium)))
    return -1;
  int value = -1;
  if (const auto *p = static_cast<const BOOL *>(GlobalLock(medium.hGlobal)))
  {
    value = *p;
    GlobalUnlock(medium.hGlobal);
  }
  ReleaseStgMedium(&medium);
  return value;
}

std::optional<DROPDESCRIPTION> read_description(IDataObject *data)
{
  FORMATETC f{CLIPFORMAT(RegisterClipboardFormatW(L"DropDescription")), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  if (FAILED(data->GetData(&f, &medium)))
    return std::nullopt;
  std::optional<DROPDESCRIPTION> out;
  if (const auto *p = static_cast<const DROPDESCRIPTION *>(GlobalLock(medium.hGlobal)))
  {
    out = *p;
    GlobalUnlock(medium.hGlobal);
  }
  ReleaseStgMedium(&medium);
  return out;
}

void write_description(IDataObject *data, DROPIMAGETYPE type, const wchar_t *message)
{
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPDESCRIPTION));
  if (!memory)
    return;
  if (auto *p = static_cast<DROPDESCRIPTION *>(GlobalLock(memory)))
  {
    p->type = type;
    lstrcpynW(p->szMessage, message, MAX_PATH);
    GlobalUnlock(memory);
  }
  FORMATETC f{CLIPFORMAT(RegisterClipboardFormatW(L"DropDescription")), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  medium.tymed = TYMED_HGLOBAL;
  medium.hGlobal = memory;
  if (FAILED(data->SetData(&f, &medium, TRUE)))
    GlobalFree(memory);
}

// The first item's picture as premultiplied, top-down pixels, the way UpdateLayeredWindow takes them.
struct pixels_t
{
  std::vector<std::uint32_t> data;
  int width = 0;
  int height = 0;
};

pixels_t read_pixels(HBITMAP bitmap)
{
  pixels_t out;
  BITMAP info{};
  if (!bitmap || !GetObjectW(bitmap, sizeof(info), &info))
    return out;
  out.width = info.bmWidth;
  out.height = std::abs(info.bmHeight);
  out.data.resize(std::size_t(out.width) * out.height);
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof(BITMAPINFOHEADER), out.width, -out.height, 1, 32, BI_RGB};
  HDC screen = GetDC(nullptr);
  const int lines = GetDIBits(screen, bitmap, 0, UINT(out.height), out.data.data(), &bi, DIB_RGB_COLORS);
  ReleaseDC(nullptr, screen);
  if (lines != out.height)
    return {};
  // A thumbnail may have no alpha at all, and an icon may be straight rather than premultiplied.
  bool any_alpha = false, straight = false;
  for (const auto p : out.data)
  {
    const auto a = p >> 24;
    any_alpha = any_alpha || a != 0;
    straight = straight || ((p >> 16) & 0xff) > a || ((p >> 8) & 0xff) > a || (p & 0xff) > a;
  }
  for (auto &p : out.data)
  {
    if (!any_alpha)
      p |= 0xff000000u;
    else if (straight)
    {
      const auto a = p >> 24;
      p = (a << 24) | ((((p >> 16) & 0xff) * a / 255) << 16) | ((((p >> 8) & 0xff) * a / 255) << 8) | ((p & 0xff) * a / 255);
    }
  }
  return out;
}

const wchar_t drag_image_class[] = L"vui_drag_image";

LRESULT CALLBACK drag_image_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
  if (message == WM_NCHITTEST)
    return HTTRANSPARENT;
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

// The drop source given to SHDoDragDrop: the usual button and Escape rules, the arrow for a cursor,
// and the picture drawn by whichever side is not the target.
class drag_source_t final : public IDropSource
{
public:
  drag_source_t(IDataObject *data, HWND owner, const std::string &first)
    : _data(data)
    , _dpi(GetDpiForWindow(owner))
  {
    SHDRAGIMAGE image = drag_image(first);
    _image = read_pixels(image.hbmpDragImage);
    _offset = image.ptOffset;
    if (image.hbmpDragImage)
      DeleteObject(image.hbmpDragImage);
  }
  ~drag_source_t()
  {
    if (_window)
      DestroyWindow(_window);
  }
  drag_source_t(const drag_source_t &) = delete;
  drag_source_t &operator=(const drag_source_t &) = delete;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
  {
    if (riid == IID_IUnknown || riid == IID_IDropSource)
    {
      *out = static_cast<IDropSource *>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  // Lives on run_drag's stack, which outlasts the drag.
  ULONG STDMETHODCALLTYPE AddRef() override { return ++_refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --_refs; }

  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override
  {
    if (escape)
      return DRAGDROP_S_CANCEL;
    if (!(keys & MK_LBUTTON))
      return DRAGDROP_S_DROP;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override
  {
    POINT at{};
    GetCursorPos(&at);
    const HWND under = GetAncestor(WindowFromPoint(at), GA_ROOT);
    const bool moved_on = under != _under;
    _under = under;
    if (read_flag(_data, L"IsShowingLayered") > 0)
    {
      hide();
      describe(effect, moved_on);
      SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
    }
    else
    {
      show(effect, at);
      SetCursor(LoadCursorW(nullptr, effect == DROPEFFECT_NONE ? MAKEINTRESOURCEW(32648) : MAKEINTRESOURCEW(32512)));
    }
    return S_OK;
  }

private:
  static DROPIMAGETYPE type_for(DWORD effect) noexcept
  {
    if (effect & DROPEFFECT_MOVE)
      return DROPIMAGE_MOVE;
    if (effect & DROPEFFECT_COPY)
      return DROPIMAGE_COPY;
    if (effect & DROPEFFECT_LINK)
      return DROPIMAGE_LINK;
    return DROPIMAGE_NONE;
  }
  static const wchar_t *word_for(DWORD effect) noexcept
  {
    if (effect & DROPEFFECT_MOVE)
      return L"Move";
    if (effect & DROPEFFECT_COPY)
      return L"Copy";
    if (effect & DROPEFFECT_LINK)
      return L"Link";
    return L"";
  }

  // What the shell's image says: the target's own words where it gave some, else the action.
  void describe(DWORD effect, bool moved_on)
  {
    const auto current = read_description(_data);
    const bool ours = !current || current->type == DROPIMAGE_INVALID || moved_on ||
                      (current->type == _written_type && std::wcscmp(current->szMessage, _written) == 0);
    if (!ours)
      return;
    const auto type = type_for(effect);
    const auto *word = word_for(effect);
    if (current && current->type == type && std::wcscmp(current->szMessage, word) == 0)
      return;
    write_description(_data, type, word);
    _written_type = type;
    lstrcpynW(_written, word, MAX_PATH);
  }

  void hide()
  {
    if (_window && IsWindowVisible(_window))
      ShowWindow(_window, SW_HIDE);
  }

  int scaled(int value) const noexcept { return MulDiv(value, int(_dpi ? _dpi : 96), 96); }

  void show(DWORD effect, POINT at)
  {
    if (_image.data.empty())
      return;
    if (!_window)
    {
      static const ATOM registered = []
      {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = drag_image_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = drag_image_class;
        return RegisterClassExW(&wc);
      }();
      (void)registered;
      _window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, drag_image_class, L"",
                                WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      if (!_window)
        return;
    }
    const POINT origin{at.x - _offset.x, at.y - _offset.y};
    if (effect == _drawn_effect)
    {
      SetWindowPos(_window, HWND_TOPMOST, origin.x, origin.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
      return;
    }
    _drawn_effect = effect;
    paint(effect, origin);
  }

  // The picture, and under the pointer a label with the action, as the shell lays its own out.
  void paint(DWORD effect, POINT origin)
  {
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HFONT font = CreateFontW(-MulDiv(9, int(_dpi ? _dpi : 96), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    auto *old_font = SelectObject(dc, font);
    const wchar_t *glyph = (effect & DROPEFFECT_MOVE) ? L"→  " : (effect & DROPEFFECT_COPY) ? L"+  " : L"";
    const std::wstring text = std::wstring(glyph) + word_for(effect);
    SIZE extent{};
    if (!text.empty())
      GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &extent);
    const int pad = scaled(6);
    const int lw = text.empty() ? 0 : extent.cx + 2 * pad, lh = text.empty() ? 0 : extent.cy + pad;
    const int lx = _offset.x + scaled(6), ly = _offset.y + scaled(18);
    const int width = std::max(_image.width, lx + lw), height = std::max(_image.height, ly + lh);

    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    void *bits = nullptr;
    HBITMAP canvas = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (canvas && bits)
    {
      auto *px = static_cast<std::uint32_t *>(bits);
      std::fill(px, px + std::size_t(width) * height, 0u);
      for (int y = 0; y < _image.height; ++y)
        std::copy_n(&_image.data[std::size_t(y) * _image.width], _image.width, px + std::size_t(y) * width);
      auto *old_bitmap = SelectObject(dc, canvas);
      if (lw > 0)
      {
        for (int y = ly; y < ly + lh; ++y)
        {
          for (int x = lx; x < lx + lw; ++x)
          {
            const bool edge = y == ly || y == ly + lh - 1 || x == lx || x == lx + lw - 1;
            px[std::size_t(y) * width + x] = edge ? 0xffa0a0a0u : 0xffffffffu;
          }
        }
        GdiFlush();
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0, 0, 0));
        TextOutW(dc, lx + pad, ly + pad / 2, text.c_str(), int(text.size()));
        GdiFlush();
        // GDI writes no alpha: the label is opaque.
        for (int y = ly; y < ly + lh; ++y)
          for (int x = lx; x < lx + lw; ++x)
            px[std::size_t(y) * width + x] |= 0xff000000u;
      }
      POINT source{0, 0};
      SIZE size{width, height};
      BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
      UpdateLayeredWindow(_window, screen, &origin, &size, dc, &source, 0, &blend, ULW_ALPHA);
      SetWindowPos(_window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
      SelectObject(dc, old_bitmap);
    }
    if (canvas)
      DeleteObject(canvas);
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
  }

  IDataObject *_data;
  UINT _dpi;
  ULONG _refs = 1;
  pixels_t _image;
  POINT _offset{};
  HWND _window = nullptr;
  HWND _under = nullptr;
  DWORD _drawn_effect = ~DWORD(0);
  DROPIMAGETYPE _written_type = DROPIMAGE_INVALID;
  wchar_t _written[MAX_PATH] = {};
};

// A shell data object for the paths: what Explorer itself hands out, with CF_HDROP for programs
// that read only that, and the shell's own list of items for those that read more.
IDataObject *make_data_object(const std::vector<std::string> &paths)
{
  std::vector<PIDLIST_ABSOLUTE> items;
  const auto release = [&]
  {
    for (auto *item : items)
      ILFree(item);
  };
  for (const auto &path : paths)
  {
    PIDLIST_ABSOLUTE item = nullptr;
    if (FAILED(SHParseDisplayName(widen(path).c_str(), nullptr, &item, 0, nullptr)) || !item)
    {
      release();
      return nullptr;
    }
    items.push_back(item);
  }
  if (items.empty())
    return nullptr;
  // Under one folder, as a selection always is: children of it. Otherwise from the desktop down.
  PIDLIST_ABSOLUTE parent = ILClone(items.front());
  ILRemoveLastID(parent);
  bool together = true;
  for (auto *item : items)
  {
    PIDLIST_ABSOLUTE up = ILClone(item);
    ILRemoveLastID(up);
    together = together && ILIsEqual(up, parent);
    ILFree(up);
  }
  IDataObject *object = nullptr;
  std::vector<PCUITEMID_CHILD> children;
  for (auto *item : items)
    children.push_back(together ? ILFindLastID(item) : reinterpret_cast<PCUITEMID_CHILD>(item));
  SHCreateDataObject(together ? parent : nullptr, UINT(children.size()), children.data(), nullptr, IID_PPV_ARGS(&object));
  ILFree(parent);
  release();
  return object;
}

DWORD win32_window_t::drop(drop_phase_t phase, IDataObject *data, DWORD keys, POINTL at, DWORD allowed)
{
  POINT client{at.x, at.y};
  POINT screen = client;
  ScreenToClient(handle, &client);
  modifier_t modifiers = modifier_t::none;
  if (keys & MK_SHIFT)
    modifiers = modifiers | modifier_t::shift;
  if (keys & MK_CONTROL)
    modifiers = modifiers | modifier_t::control;
  if (keys & MK_ALT)
    modifiers = modifiers | modifier_t::alt;
  if (phase == drop_phase_t::enter || (phase == drop_phase_t::drop && data))
    _dragged_in = read_drag_data(data);
  // The answer the program has given so far, within what the dragging side allows.
  const DWORD effect = phase == drop_phase_t::leave ? DROPEFFECT_NONE : one_effect(DWORD(_window.drop_answer) & allowed);
  // The shell draws the dragged items' image over windows that tell it where the drag is.
  if (!_drop_helper && phase == drop_phase_t::enter)
    CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&_drop_helper));
  if (_drop_helper)
  {
    switch (phase)
    {
    case drop_phase_t::enter:
      _drop_helper->DragEnter(handle, data, &screen, effect);
      break;
    case drop_phase_t::over:
      _drop_helper->DragOver(&screen, effect);
      break;
    case drop_phase_t::leave:
      _drop_helper->DragLeave();
      break;
    case drop_phase_t::drop:
      _drop_helper->Drop(data, &screen, effect);
      break;
    }
  }
  _window.on_input(drop_event_t{phase, {double(client.x), double(client.y)}, modifiers, drop_action_t(allowed & 7), _dragged_in});
  if (phase == drop_phase_t::leave || phase == drop_phase_t::drop)
  {
    _dragged_in.reset();
    _window.drop_answer = drop_action_t::none;
  }
  return effect;
}

bool win32_window_t::start_drag(std::vector<std::string> paths, drop_action_t allowed)
{
  if (!_display.thread->ole || paths.empty() || _drag_pending)
    return false;
  _drag_paths = std::move(paths);
  _drag_allowed = allowed;
  _drag_pending = true;
  PostMessageW(handle, drag_message, 0, 0);
  return true;
}

// The drag itself: OLE's modal loop, inside which the event loop is turned by the modal timer, so
// the program still draws and still answers drops on its own window. OLE took the button's release;
// the window is told the button is up once the drag has said how it ended.
void win32_window_t::run_drag()
{
  _drag_pending = false;
  drop_action_t result = drop_action_t::none;
  if (IDataObject *data = make_data_object(_drag_paths))
  {
    auto &thread = *_display.thread;
    DWORD effect = DROPEFFECT_NONE;
    // The drag image, and leave to say "Move to ..." in words where the target describes the drop.
    IDragSourceHelper2 *helper = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper))))
    {
      helper->SetFlags(DSH_ALLOWDROPDESCRIPTIONTEXT);
      SHDRAGIMAGE image = drag_image(_drag_paths.front());
      if (image.hbmpDragImage && FAILED(helper->InitializeFromBitmap(&image, data)))
        DeleteObject(image.hbmpDragImage);
      helper->Release();
    }
    drag_source_t source(data, handle, _drag_paths.front());
    thread.enter_modal();
    const HRESULT done = SHDoDragDrop(handle, data, &source, DWORD(_drag_allowed), &effect);
    thread.leave_modal();
    data->Release();
    if (done == DRAGDROP_S_DROP)
      result = drop_action_t(effect & 7);
  }
  _drag_paths.clear();
  _window.on_input(drag_end_event_t{result});
  if (_buttons > 0)
  {
    _buttons = 0;
    if (GetCapture() == handle)
      ReleaseCapture();
    POINT at{};
    GetCursorPos(&at);
    ScreenToClient(handle, &at);
    _window.on_input(pointer_button_event_t{button_t::left, false, {double(at.x), double(at.y)}, current_modifiers()});
  }
}

win32_window_t::win32_window_t(win32_display_t &display, window_impl_t &window)
  : _display(display)
  , _window(window)
  , _clock(*window.display.loop, refresh_interval(window.display.config.frame_interval_ns), &win32_window_t::tick, this)
{
}

win32_window_t::~win32_window_t()
{
  if (_drop_helper)
    _drop_helper->Release();
  if (!handle)
    return;
  if (_drop_target)
  {
    static_cast<drop_target_t *>(_drop_target)->window = nullptr;
    RevokeDragDrop(handle);
    _drop_target->Release();
  }
  SetWindowLongPtrW(handle, GWLP_USERDATA, 0);
  if (GetCapture() == handle)
    ReleaseCapture();
  DestroyWindow(handle);
}

float win32_window_t::scale() const
{
  return static_cast<float>(win32_window_dpi(handle)) / 96.0f;
}

RECT win32_window_t::outer_rect(extent_t client) const
{
  RECT rect{0, 0, client.width, client.height};
  win32_adjust_rect(rect, _style, _ex_style, win32_window_dpi(handle));
  return rect;
}

int win32_window_t::create(const window_config_t &config)
{
  _resizable = config.resizable;
  _min_size = config.min_size;
  _max_size = config.max_size;
  _style = WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
  if (config.decorated)
  {
    _style |= WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    if (config.resizable)
      _style |= WS_MAXIMIZEBOX | WS_THICKFRAME;
  }
  else
  {
    _style |= WS_POPUP;
  }
  _ex_style = WS_EX_APPWINDOW;
  if (config.transparent)
    _ex_style |= WS_EX_NOREDIRECTIONBITMAP;
  RECT rect{0, 0, config.size.width, config.size.height};
  win32_adjust_rect(rect, _style, _ex_style, win32_system_dpi());
  const std::wstring title = widen(config.title);
  CreateWindowExW(_ex_style, window_class, title.c_str(), _style, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, GetModuleHandleW(nullptr), this);
  if (!handle)
    return vio::errc::out_of_memory;
  _frame_theme = config.frame_theme;
  apply_frame_theme();
  if (_display.thread->ole)
  {
    _drop_target = new drop_target_t(*this);
    if (FAILED(RegisterDragDrop(handle, _drop_target)))
    {
      _drop_target->Release();
      _drop_target = nullptr;
    }
  }
  const float factor = scale();
  if (factor != 1.0f)
  {
    const RECT scaled = outer_rect(config.size);
    SetWindowPos(handle, nullptr, 0, 0, scaled.right - scaled.left, scaled.bottom - scaled.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  report_size();
  if (config.fullscreen)
    set_fullscreen(true);
  if (config.visible)
  {
    ShowWindow(handle, SW_SHOWNORMAL);
    _window.on_visible(true);
  }
  return 0;
}

// Whether the user has Windows' apps set to dark: "Choose your app mode" in the colour settings.
static bool apps_use_dark() noexcept
{
  DWORD light = 1;
  DWORD size = sizeof light;
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS)
    return false;
  return light == 0;
}

// A dark title bar is asked of DWM; the attribute is 20 from Windows 10 20H1 and was 19 before it.
// Asked again when the user changes the app mode, and the frame is redrawn for it (G58).
void win32_window_t::apply_frame_theme()
{
  const BOOL dark = _frame_theme == frame_theme_t::dark || (_frame_theme == frame_theme_t::system && apps_use_dark());
  if (FAILED(DwmSetWindowAttribute(handle, 20, &dark, sizeof dark)))
    DwmSetWindowAttribute(handle, 19, &dark, sizeof dark);
  SetWindowPos(handle, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void win32_window_t::report_size()
{
  RECT client{};
  GetClientRect(handle, &client);
  const extent_t size{client.right - client.left, client.bottom - client.top};
  _window.on_resize(size, size, scale());
}

// Inside a size loop Windows shows the window at its new size as soon as WM_SIZE or WM_PAINT returns,
// so the frame for that size has to be drawn before then. It is often more than one turn away: a
// UI wakes its frame loop through the deferred queue, which a turn drains before the hook that
// hands out frames, so with a single turn every other resize was drawn only on the next WM_TIMER,
// a refresh later (G47). Turns run until a frame has been taken or none is due, at most three.
void win32_window_t::turn_until_drawn()
{
  auto &thread = *_display.thread;
  if (thread.modal == 0)
    return;
  const auto before = _window.frames;
  for (int turn = 0; turn < 3; ++turn)
  {
    thread.nested_turn();
    if (_window.frames != before || !_window.frame_ready_now())
      break;
  }
}

void win32_window_t::button(button_t which, bool pressed, LPARAM lparam)
{
  if (pressed)
  {
    if (_buttons++ == 0)
      SetCapture(handle);
  }
  else if (_buttons > 0 && --_buttons == 0)
  {
    ReleaseCapture();
  }
  _window.on_input(pointer_button_event_t{which, pressed, {static_cast<double>(GET_X_LPARAM(lparam)), static_cast<double>(GET_Y_LPARAM(lparam))}, current_modifiers()});
}

LRESULT win32_window_t::procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
  auto &thread = *_display.thread;
  switch (message)
  {
  case WM_CLOSE:
    _window.on_close_request();
    return 0;
  case WM_SIZE:
    if (wparam == SIZE_MINIMIZED)
    {
      _window.on_visible(false);
    }
    else
    {
      if (IsWindowVisible(hwnd))
        _window.on_visible(true);
      report_size();
    }
    turn_until_drawn();
    return 0;
  case WM_SHOWWINDOW:
    _window.on_visible(wparam != 0);
    break;
  case WM_PAINT:
  {
    PAINTSTRUCT paint;
    BeginPaint(hwnd, &paint);
    EndPaint(hwnd, &paint);
    _window.on_damage();
    turn_until_drawn();
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_SETTINGCHANGE:
    if (_frame_theme == frame_theme_t::system && lparam && lstrcmpW(reinterpret_cast<LPCWSTR>(lparam), L"ImmersiveColorSet") == 0)
      apply_frame_theme();
    break;
  case WM_ENTERSIZEMOVE:
  case WM_ENTERMENULOOP:
    thread.enter_modal();
    break;
  case WM_EXITSIZEMOVE:
  case WM_EXITMENULOOP:
    thread.leave_modal();
    break;
  case WM_DPICHANGED:
  {
    const auto *suggested = reinterpret_cast<const RECT *>(lparam);
    SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
    report_size();
    return 0;
  }
  case WM_GETMINMAXINFO:
  {
    if (!handle)
      break;
    auto *info = reinterpret_cast<MINMAXINFO *>(lparam);
    if (_min_size.width > 0 && _min_size.height > 0)
    {
      const RECT rect = outer_rect(_min_size);
      info->ptMinTrackSize = {rect.right - rect.left, rect.bottom - rect.top};
    }
    if (_max_size.width > 0 && _max_size.height > 0)
    {
      const RECT rect = outer_rect(_max_size);
      info->ptMaxTrackSize = {rect.right - rect.left, rect.bottom - rect.top};
    }
    return 0;
  }
  case WM_SETFOCUS:
    _window.on_input(focus_event_t{true});
    return 0;
  case WM_KILLFOCUS:
    _window.on_input(focus_event_t{false});
    return 0;
  case WM_KEYDOWN:
  case WM_SYSKEYDOWN:
  case WM_KEYUP:
  case WM_SYSKEYUP:
  {
    const bool pressed = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    key_event_t event;
    event.key = translate_key(wparam, lparam);
    event.scancode = static_cast<std::uint32_t>((lparam >> 16) & 0x1ff);
    event.modifiers = current_modifiers();
    event.pressed = pressed;
    event.repeat = pressed && ((lparam >> 30) & 1);
    _window.on_input(event);
    if (message == WM_SYSKEYDOWN && wparam == VK_F4)
      break;
    return 0;
  }
  case WM_CHAR:
  case WM_SYSCHAR:
  {
    const auto unit = static_cast<WCHAR>(wparam);
    if (unit >= 0xd800 && unit <= 0xdbff)
    {
      _high_surrogate = unit;
      return 0;
    }
    std::string text;
    if (unit >= 0xdc00 && unit <= 0xdfff && _high_surrogate)
    {
      const WCHAR pair[2] = {_high_surrogate, unit};
      text = narrow(pair, 2);
    }
    else if (unit >= 0x20 && unit != 0x7f)
    {
      text = narrow(&unit, 1);
    }
    _high_surrogate = 0;
    if (!text.empty())
      _window.on_input(text_event_t{std::move(text)});
    return message == WM_SYSCHAR ? DefWindowProcW(hwnd, message, wparam, lparam) : 0;
  }
  case WM_MOUSEMOVE:
  {
    const point_t at{static_cast<double>(GET_X_LPARAM(lparam)), static_cast<double>(GET_Y_LPARAM(lparam))};
    if (!_tracking)
    {
      TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
      TrackMouseEvent(&track);
      _tracking = true;
      _window.on_input(pointer_enter_event_t{true, at});
    }
    _window.on_input(pointer_move_event_t{at, current_modifiers()});
    return 0;
  }
  case WM_MOUSELEAVE:
    _tracking = false;
    _window.on_input(pointer_enter_event_t{false, {}});
    return 0;
  case WM_LBUTTONDOWN:
  case WM_LBUTTONUP:
    button(button_t::left, message == WM_LBUTTONDOWN, lparam);
    return 0;
  case WM_RBUTTONDOWN:
  case WM_RBUTTONUP:
    button(button_t::right, message == WM_RBUTTONDOWN, lparam);
    return 0;
  case WM_MBUTTONDOWN:
  case WM_MBUTTONUP:
    button(button_t::middle, message == WM_MBUTTONDOWN, lparam);
    return 0;
  case WM_XBUTTONDOWN:
  case WM_XBUTTONUP:
    button(GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? button_t::back : button_t::forward, message == WM_XBUTTONDOWN, lparam);
    return TRUE;
  case WM_MOUSEWHEEL:
  case WM_MOUSEHWHEEL:
  {
    POINT at{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    ScreenToClient(hwnd, &at);
    const double amount = static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
    scroll_event_t event{0, 0, {static_cast<double>(at.x), static_cast<double>(at.y)}, current_modifiers()};
    if (message == WM_MOUSEWHEEL)
      event.dy = amount;
    else
      event.dx = amount;
    _window.on_input(event);
    return 0;
  }
  case drag_message:
    run_drag();
    return 0;
  case WM_SETCURSOR:
    if (LOWORD(lparam) == HTCLIENT)
    {
      SetCursor(_cursor == cursor_t::hidden ? nullptr : LoadCursorW(nullptr, cursor_resource(_cursor)));
      return TRUE;
    }
    break;
  default:
    break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void win32_window_t::set_title(std::string_view title)
{
  SetWindowTextW(handle, widen(title).c_str());
}

int win32_window_t::set_size(extent_t size)
{
  if (_fullscreen)
    return vio::errc::busy;
  const RECT rect = outer_rect(size);
  SetWindowPos(handle, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  return 0;
}

int win32_window_t::set_position(point_t position)
{
  SetWindowPos(handle, nullptr, static_cast<int>(position.x), static_cast<int>(position.y), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  return 0;
}

std::expected<point_t, int> win32_window_t::position() const
{
  RECT rect{};
  if (!GetWindowRect(handle, &rect))
    return std::unexpected(vio::errc::io_error);
  return point_t{static_cast<double>(rect.left), static_cast<double>(rect.top)};
}

void win32_window_t::set_size_limits(extent_t min_size, extent_t max_size)
{
  _min_size = min_size;
  _max_size = max_size;
}

void win32_window_t::set_visible(bool visible)
{
  ShowWindow(handle, visible ? SW_SHOWNA : SW_HIDE);
  _window.on_visible(visible && !IsIconic(handle));
}

void win32_window_t::set_fullscreen(bool fullscreen)
{
  if (fullscreen == _fullscreen)
    return;
  _fullscreen = fullscreen;
  if (fullscreen)
  {
    GetWindowRect(handle, &_windowed);
    _windowed_style = static_cast<DWORD>(GetWindowLongPtrW(handle, GWL_STYLE));
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor);
    SetWindowLongPtrW(handle, GWL_STYLE, static_cast<LONG_PTR>((_windowed_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP));
    const RECT &area = monitor.rcMonitor;
    SetWindowPos(handle, HWND_TOP, area.left, area.top, area.right - area.left, area.bottom - area.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
  }
  else
  {
    SetWindowLongPtrW(handle, GWL_STYLE, static_cast<LONG_PTR>(_windowed_style));
    SetWindowPos(handle, nullptr, _windowed.left, _windowed.top, _windowed.right - _windowed.left, _windowed.bottom - _windowed.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
  }
}

void win32_window_t::minimize()
{
  ShowWindow(handle, SW_MINIMIZE);
}

void win32_window_t::maximize()
{
  ShowWindow(handle, SW_MAXIMIZE);
}

void win32_window_t::restore()
{
  ShowWindow(handle, SW_RESTORE);
}

void win32_window_t::focus()
{
  BringWindowToTop(handle);
  SetForegroundWindow(handle);
  SetFocus(handle);
}

void win32_window_t::set_cursor(cursor_t cursor)
{
  _cursor = cursor;
  POINT at{};
  GetCursorPos(&at);
  if (WindowFromPoint(at) == handle)
    SetCursor(cursor == cursor_t::hidden ? nullptr : LoadCursorW(nullptr, cursor_resource(cursor)));
}

void win32_window_t::frame_wanted()
{
  _clock.request();
}

void win32_window_t::tick(void *context)
{
  auto *self = static_cast<win32_window_t *>(context);
  self->_window.on_frame(now_ns(self->_window));
}

native_window_t win32_window_t::native() const
{
  return {platform_t::win32, GetModuleHandleW(nullptr), handle, reinterpret_cast<std::uintptr_t>(handle), nullptr, 0};
}
} // namespace

HWND win32_handle(window_impl_t &window)
{
  return static_cast<HWND>(window.backend->native().handle);
}

std::expected<std::unique_ptr<display_backend_t>, int> make_win32_backend(display_impl_t &display)
{
  auto made = std::make_unique<win32_display_t>(display);
  if (int result = made->open(); result < 0)
    return std::unexpected(result);
  return made;
}
} // namespace vui::detail
