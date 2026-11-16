#if defined(__linux__)

// Linux: everything is a file under /proc. /proc/stat has each processor's times in clock ticks,
// /proc/meminfo the memory, and /proc/<pid>/stat one line per process.

#include "system.h"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace sys
{
namespace
{
std::string read_file(const char *path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

process_state_t state_of(char c) noexcept
{
  switch (c)
  {
  case 'R':
    return process_state_t::running;
  case 'S':
  case 'I':
    return process_state_t::sleeping;
  case 'D':
    return process_state_t::disk_wait;
  case 'T':
  case 't':
    return process_state_t::stopped;
  case 'Z':
  case 'X':
    return process_state_t::zombie;
  default:
    return process_state_t::unknown;
  }
}

// "cpu  user nice system idle iowait irq softirq steal ..."; idle and iowait are the idle part.
bool parse_cpu_line(const char *line, cpu_times_t &out)
{
  unsigned long long v[8] = {};
  const char *values = std::strchr(line, ' ');
  if (!values || std::sscanf(values, "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 4)
    return false;
  std::uint64_t total = 0;
  for (auto x : v)
    total += x;
  out.total = total;
  out.busy = total - (v[3] + v[4]);
  return true;
}
} // namespace

struct probe_t::impl_t
{
  long ticks_per_second = 100;
  long page_size = 4096;
};

probe_t::probe_t()
  : _impl(std::make_unique<impl_t>())
{
  _impl->ticks_per_second = std::max(sysconf(_SC_CLK_TCK), 1L);
  _impl->page_size = std::max(sysconf(_SC_PAGESIZE), 1L);
  _machine.logical_cores = unsigned(std::max(sysconf(_SC_NPROCESSORS_ONLN), 1L));
  std::istringstream cpuinfo(read_file("/proc/cpuinfo"));
  for (std::string line; std::getline(cpuinfo, line);)
  {
    if (line.rfind("model name", 0) == 0 || line.rfind("Model", 0) == 0)
    {
      const auto colon = line.find(':');
      if (colon != std::string::npos)
        _machine.cpu_name = line.substr(line.find_first_not_of(' ', colon + 1));
      break;
    }
  }
  std::istringstream release(read_file("/etc/os-release"));
  for (std::string line; std::getline(release, line);)
  {
    if (line.rfind("PRETTY_NAME=", 0) == 0)
    {
      _machine.os_name = line.substr(12);
      if (_machine.os_name.size() >= 2 && _machine.os_name.front() == '"')
        _machine.os_name = _machine.os_name.substr(1, _machine.os_name.size() - 2);
    }
  }
  if (_machine.os_name.empty())
    _machine.os_name = "Linux";
}

probe_t::~probe_t() = default;

std::uint32_t current_pid() noexcept
{
  return std::uint32_t(getpid());
}

snapshot_t probe_t::sample()
{
  const auto started = std::chrono::steady_clock::now();
  snapshot_t s;

  std::istringstream stat(read_file("/proc/stat"));
  for (std::string line; std::getline(stat, line);)
  {
    if (line.rfind("cpu", 0) != 0)
      break;
    cpu_times_t times;
    if (!parse_cpu_line(line.c_str(), times))
      continue;
    if (line[3] == ' ')
      s.cpu = times;
    else
      s.cores.push_back(times);
  }

  std::istringstream meminfo(read_file("/proc/meminfo"));
  std::uint64_t available = 0;
  std::uint64_t swap_free = 0;
  for (std::string line; std::getline(meminfo, line);)
  {
    unsigned long long kb = 0;
    if (std::sscanf(line.c_str(), "MemTotal: %llu", &kb) == 1)
      s.memory_total = kb * 1024;
    else if (std::sscanf(line.c_str(), "MemAvailable: %llu", &kb) == 1)
      available = kb * 1024;
    else if (std::sscanf(line.c_str(), "SwapTotal: %llu", &kb) == 1)
      s.swap_total = kb * 1024;
    else if (std::sscanf(line.c_str(), "SwapFree: %llu", &kb) == 1)
      swap_free = kb * 1024;
  }
  s.memory_used = s.memory_total > available ? s.memory_total - available : 0;
  s.swap_used = s.swap_total > swap_free ? s.swap_total - swap_free : 0;
  s.uptime = std::chrono::seconds(std::atoll(read_file("/proc/uptime").c_str()));

  if (DIR *proc = opendir("/proc"))
  {
    char path[64];
    while (auto *entry = readdir(proc))
    {
      char *end = nullptr;
      const auto pid = std::strtoul(entry->d_name, &end, 10);
      if (!end || *end != '\0' || entry->d_name[0] == '\0')
        continue;
      std::snprintf(path, sizeof path, "/proc/%lu/stat", pid);
      const auto line = read_file(path);
      // The name is in parentheses and may hold anything, parentheses included: the fields
      // resume after the last ')'.
      const auto open = line.find('(');
      const auto close = line.rfind(')');
      if (open == std::string::npos || close == std::string::npos || close < open)
        continue;
      process_t p;
      p.pid = std::uint32_t(pid);
      p.name = line.substr(open + 1, close - open - 1);
      char state = '?';
      unsigned long long ppid = 0, utime = 0, stime = 0, threads = 0, rss = 0;
      // state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime cutime
      // cstime priority nice num_threads itrealvalue starttime vsize rss
      const int got = std::sscanf(line.c_str() + close + 1, " %c %llu %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %*d %*d %llu %*d %*u %*u %llu", &state, &ppid, &utime, &stime,
                                  &threads, &rss);
      if (got < 6)
        p.partial = true;
      p.state = state_of(state);
      p.parent = std::uint32_t(ppid);
      p.cpu_ns = (utime + stime) * 1000000000ull / std::uint64_t(_impl->ticks_per_second);
      p.threads = std::uint32_t(threads);
      p.memory = rss * std::uint64_t(_impl->page_size);
      s.processes.push_back(std::move(p));
    }
    closedir(proc);
  }

  s.taken = std::chrono::steady_clock::now();
  s.cost = s.taken - started;
  return s;
}
} // namespace sys

#endif
