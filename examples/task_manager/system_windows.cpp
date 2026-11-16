#if defined(_WIN32)

// Windows: one NtQuerySystemInformation call returns every process with its threads, times and
// working set, and another returns each processor's times. Both are documented only in part,
// so the structures are declared here as the system lays them out (from the Windows SDK's
// winternl.h, with its reserved fields named).

#include "system.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace sys
{
namespace
{
using nt_query_t = LONG(WINAPI *)(ULONG, PVOID, ULONG, PULONG);
constexpr ULONG system_process_information = 5;
constexpr ULONG system_processor_performance_information = 8;
constexpr ULONG system_page_file_information = 18;
constexpr LONG status_info_length_mismatch = LONG(0xC0000004);

struct unicode_string_t
{
  USHORT length;
  USHORT maximum_length;
  PWSTR buffer;
};

struct thread_information_t
{
  LARGE_INTEGER kernel_time;
  LARGE_INTEGER user_time;
  LARGE_INTEGER create_time;
  ULONG wait_time;
  PVOID start_address;
  HANDLE client_process;
  HANDLE client_thread;
  LONG priority;
  LONG base_priority;
  ULONG context_switches;
  ULONG thread_state;
  ULONG wait_reason;
};

struct process_information_t
{
  ULONG next_entry_offset;
  ULONG number_of_threads;
  LARGE_INTEGER working_set_private_size;
  ULONG hard_fault_count;
  ULONG number_of_threads_high_watermark;
  ULONGLONG cycle_time;
  LARGE_INTEGER create_time;
  LARGE_INTEGER user_time;
  LARGE_INTEGER kernel_time;
  unicode_string_t image_name;
  LONG base_priority;
  HANDLE unique_process_id;
  HANDLE inherited_from_unique_process_id;
  ULONG handle_count;
  ULONG session_id;
  ULONG_PTR unique_process_key;
  SIZE_T peak_virtual_size;
  SIZE_T virtual_size;
  ULONG page_fault_count;
  SIZE_T peak_working_set_size;
  SIZE_T working_set_size;
  SIZE_T quota_peak_paged_pool_usage;
  SIZE_T quota_paged_pool_usage;
  SIZE_T quota_peak_non_paged_pool_usage;
  SIZE_T quota_non_paged_pool_usage;
  SIZE_T pagefile_usage;
  SIZE_T peak_pagefile_usage;
  SIZE_T private_page_count;
  LARGE_INTEGER read_operation_count;
  LARGE_INTEGER write_operation_count;
  LARGE_INTEGER other_operation_count;
  LARGE_INTEGER read_transfer_count;
  LARGE_INTEGER write_transfer_count;
  LARGE_INTEGER other_transfer_count;
};

struct processor_times_t
{
  LARGE_INTEGER idle_time;
  // Includes the idle time.
  LARGE_INTEGER kernel_time;
  LARGE_INTEGER user_time;
  LARGE_INTEGER dpc_time;
  LARGE_INTEGER interrupt_time;
  ULONG interrupt_count;
};

// One per page file, sizes in pages.
struct page_file_information_t
{
  ULONG next_entry_offset;
  ULONG total_size;
  ULONG total_in_use;
  ULONG peak_usage;
  unicode_string_t name;
};

constexpr ULONG thread_ready = 1;
constexpr ULONG thread_running = 2;
constexpr ULONG thread_waiting = 5;
constexpr ULONG wait_reason_suspended = 5;

std::string narrow(const wchar_t *text, int length)
{
  if (length <= 0)
    return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
  std::string out(std::size_t(std::max(bytes, 0)), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), bytes, nullptr, nullptr);
  return out;
}

// Running if any thread is running or ready to; suspended if every thread is suspended.
process_state_t state_of(const process_information_t &p)
{
  const auto *threads = reinterpret_cast<const thread_information_t *>(&p + 1);
  if (p.number_of_threads == 0)
    return process_state_t::zombie;
  bool all_suspended = true;
  for (ULONG i = 0; i < p.number_of_threads; ++i)
  {
    const auto &t = threads[i];
    if (t.thread_state == thread_running || t.thread_state == thread_ready)
      return process_state_t::running;
    if (!(t.thread_state == thread_waiting && t.wait_reason == wait_reason_suspended))
      all_suspended = false;
  }
  return all_suspended ? process_state_t::suspended : process_state_t::sleeping;
}
} // namespace

struct probe_t::impl_t
{
  nt_query_t query = nullptr;
  std::vector<std::byte> buffer = std::vector<std::byte>(1 << 20);
};

probe_t::probe_t()
  : _impl(std::make_unique<impl_t>())
{
  if (auto *ntdll = GetModuleHandleW(L"ntdll.dll"))
    _impl->query = reinterpret_cast<nt_query_t>(reinterpret_cast<void *>(GetProcAddress(ntdll, "NtQuerySystemInformation")));
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  _machine.logical_cores = info.dwNumberOfProcessors;
  char name[256] = {};
  DWORD size = sizeof name;
  if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS)
    _machine.cpu_name = name;
  _machine.os_name = "Windows";
  size = sizeof name;
  if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion", RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS)
    _machine.os_name += std::string(" ") + name;
  // Trailing spaces pad some processor names.
  while (!_machine.cpu_name.empty() && _machine.cpu_name.back() == ' ')
    _machine.cpu_name.pop_back();
}

probe_t::~probe_t() = default;

std::uint32_t current_pid() noexcept
{
  return std::uint32_t(GetCurrentProcessId());
}

snapshot_t probe_t::sample()
{
  const auto started = std::chrono::steady_clock::now();
  snapshot_t s;
  auto &impl = *_impl;

  if (impl.query)
  {
    std::vector<processor_times_t> cores(_machine.logical_cores);
    ULONG got = 0;
    if (impl.query(system_processor_performance_information, cores.data(), ULONG(cores.size() * sizeof(processor_times_t)), &got) >= 0)
    {
      cores.resize(got / sizeof(processor_times_t));
      for (const auto &c : cores)
      {
        const auto total = std::uint64_t(c.kernel_time.QuadPart + c.user_time.QuadPart);
        const auto busy = total - std::uint64_t(c.idle_time.QuadPart);
        s.cores.push_back({busy, total});
        s.cpu.busy += busy;
        s.cpu.total += total;
      }
    }

    // The process list grows between asking how big it is and reading it, so grow and retry.
    LONG status = 0;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
      got = 0;
      status = impl.query(system_process_information, impl.buffer.data(), ULONG(impl.buffer.size()), &got);
      if (status != status_info_length_mismatch)
        break;
      impl.buffer.resize(std::max<std::size_t>(impl.buffer.size() * 2, got + (64 << 10)));
    }
    if (status >= 0)
    {
      std::size_t offset = 0;
      for (;;)
      {
        const auto &p = *reinterpret_cast<const process_information_t *>(impl.buffer.data() + offset);
        process_t out;
        out.pid = std::uint32_t(reinterpret_cast<std::uintptr_t>(p.unique_process_id));
        out.parent = std::uint32_t(reinterpret_cast<std::uintptr_t>(p.inherited_from_unique_process_id));
        out.name = out.pid == 0 ? std::string("System Idle Process") : narrow(p.image_name.buffer, p.image_name.length / 2);
        out.threads = p.number_of_threads;
        // 100 ns units.
        out.cpu_ns = std::uint64_t(p.user_time.QuadPart + p.kernel_time.QuadPart) * 100;
        out.memory = p.working_set_size;
        out.state = out.pid == 0 ? process_state_t::running : state_of(p);
        s.processes.push_back(std::move(out));
        if (p.next_entry_offset == 0)
          break;
        offset += p.next_entry_offset;
      }
    }
  }

  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof memory;
  if (GlobalMemoryStatusEx(&memory))
  {
    s.memory_total = memory.ullTotalPhys;
    s.memory_used = memory.ullTotalPhys - memory.ullAvailPhys;
  }
  // Swap is the page files: what they hold, not the commit charge, which counts memory that was
  // promised and never touched.
  alignas(8) std::byte files[4096];
  ULONG got = 0;
  if (impl.query && impl.query(system_page_file_information, files, sizeof files, &got) >= 0 && got >= sizeof(page_file_information_t))
  {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    std::size_t offset = 0;
    for (;;)
    {
      const auto &f = *reinterpret_cast<const page_file_information_t *>(files + offset);
      s.swap_total += std::uint64_t(f.total_size) * info.dwPageSize;
      s.swap_used += std::uint64_t(f.total_in_use) * info.dwPageSize;
      if (f.next_entry_offset == 0 || offset + f.next_entry_offset >= got)
        break;
      offset += f.next_entry_offset;
    }
  }
  s.uptime = std::chrono::seconds(GetTickCount64() / 1000);
  s.taken = std::chrono::steady_clock::now();
  s.cost = s.taken - started;
  return s;
}
} // namespace sys

#endif
