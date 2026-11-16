#pragma once

// What the task manager knows about the machine, the same on every platform. One probe_t takes
// snapshots; system_windows.cpp, system_linux.cpp and system_macos.cpp each implement it with what
// their system offers, and only one of them compiles on a given platform. Snapshots carry raw
// counters, never rates: a rate is the difference of two snapshots, which the caller takes.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sys
{
// Time a processor spent, in whatever unit the platform counts in; only ratios of differences mean
// anything.
struct cpu_times_t
{
  std::uint64_t busy = 0;
  std::uint64_t total = 0;
};

enum class process_state_t : std::uint8_t
{
  running,
  sleeping,
  // Waiting on I/O it cannot be interrupted out of: Linux's D.
  disk_wait,
  stopped,
  suspended,
  zombie,
  unknown,
};

struct process_t
{
  std::uint32_t pid = 0;
  std::uint32_t parent = 0;
  std::string name;
  process_state_t state = process_state_t::unknown;
  std::uint32_t threads = 0;
  // User and kernel time together, since it started.
  std::uint64_t cpu_ns = 0;
  // Resident set, or working set on Windows.
  std::uint64_t memory = 0;
  // Something the platform would not tell us about it, usually for want of permission.
  bool partial = false;
};

struct snapshot_t
{
  std::chrono::steady_clock::time_point taken;
  // How long taking it took, on the sampling thread.
  std::chrono::nanoseconds cost{0};
  cpu_times_t cpu;
  std::vector<cpu_times_t> cores;
  std::uint64_t memory_total = 0;
  std::uint64_t memory_used = 0;
  std::uint64_t swap_total = 0;
  std::uint64_t swap_used = 0;
  std::chrono::seconds uptime{0};
  std::vector<process_t> processes;
};

// What does not change while the program runs.
struct machine_t
{
  std::string cpu_name;
  std::string os_name;
  unsigned logical_cores = 0;
};

class probe_t
{
public:
  probe_t();
  ~probe_t();
  probe_t(const probe_t &) = delete;
  probe_t &operator=(const probe_t &) = delete;

  [[nodiscard]] const machine_t &machine() const noexcept
  {
    return _machine;
  }
  [[nodiscard]] snapshot_t sample();

private:
  struct impl_t;
  std::unique_ptr<impl_t> _impl;
  machine_t _machine;
};

[[nodiscard]] std::uint32_t current_pid() noexcept;

[[nodiscard]] inline const char *to_string(process_state_t state) noexcept
{
  switch (state)
  {
  case process_state_t::running:
    return "Running";
  case process_state_t::sleeping:
    return "Sleeping";
  case process_state_t::disk_wait:
    return "Disk wait";
  case process_state_t::stopped:
    return "Stopped";
  case process_state_t::suspended:
    return "Suspended";
  case process_state_t::zombie:
    return "Zombie";
  default:
    return "Unknown";
  }
}
} // namespace sys
