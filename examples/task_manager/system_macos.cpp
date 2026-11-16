#if defined(__APPLE__)

// macOS: mach for processor ticks and memory, libproc for processes, sysctl for the rest. Another
// user's process answers only to root for most of what libproc asks, so for those the name and
// state come from what any process may read, and the rest is marked partial.

#include "system.h"

#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <ctime>

namespace sys
{
namespace
{
std::string sysctl_string(const char *name)
{
  std::size_t size = 0;
  if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0)
    return {};
  std::string out(size, '\0');
  if (sysctlbyname(name, out.data(), &size, nullptr, 0) != 0)
    return {};
  while (!out.empty() && out.back() == '\0')
    out.pop_back();
  return out;
}

process_state_t state_of(std::uint32_t status) noexcept
{
  switch (status)
  {
  case SRUN:
    return process_state_t::running;
  case SSLEEP:
  case SIDL:
    return process_state_t::sleeping;
  case SSTOP:
    return process_state_t::stopped;
  case SZOMB:
    return process_state_t::zombie;
  default:
    return process_state_t::unknown;
  }
}
} // namespace

struct probe_t::impl_t
{
  // proc_taskinfo's times are mach absolute time, which is nanoseconds only on Intel.
  mach_timebase_info_data_t timebase{1, 1};
  vm_size_t page_size = 4096;
  std::vector<pid_t> pids;
};

probe_t::probe_t()
  : _impl(std::make_unique<impl_t>())
{
  mach_timebase_info(&_impl->timebase);
  host_page_size(mach_host_self(), &_impl->page_size);
  _machine.cpu_name = sysctl_string("machdep.cpu.brand_string");
  _machine.os_name = "macOS " + sysctl_string("kern.osproductversion");
  int cores = 0;
  std::size_t size = sizeof cores;
  if (sysctlbyname("hw.logicalcpu", &cores, &size, nullptr, 0) == 0)
    _machine.logical_cores = unsigned(cores);
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
  auto &impl = *_impl;

  natural_t count = 0;
  processor_info_array_t info = nullptr;
  mach_msg_type_number_t info_count = 0;
  if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &count, &info, &info_count) == KERN_SUCCESS)
  {
    auto *load = reinterpret_cast<processor_cpu_load_info_t>(info);
    for (natural_t i = 0; i < count; ++i)
    {
      const auto &ticks = load[i].cpu_ticks;
      const std::uint64_t busy = std::uint64_t(ticks[CPU_STATE_USER]) + ticks[CPU_STATE_SYSTEM] + ticks[CPU_STATE_NICE];
      const std::uint64_t total = busy + ticks[CPU_STATE_IDLE];
      s.cores.push_back({busy, total});
      s.cpu.busy += busy;
      s.cpu.total += total;
    }
    vm_deallocate(mach_task_self(), vm_address_t(info), vm_size_t(info_count) * sizeof(integer_t));
  }

  std::uint64_t memsize = 0;
  std::size_t size = sizeof memsize;
  if (sysctlbyname("hw.memsize", &memsize, &size, nullptr, 0) == 0)
    s.memory_total = memsize;
  vm_statistics64_data_t vm{};
  mach_msg_type_number_t vm_count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &vm_count) == KERN_SUCCESS)
    s.memory_used = (std::uint64_t(vm.active_count) + vm.wire_count + vm.compressor_page_count) * impl.page_size;
  xsw_usage swap{};
  size = sizeof swap;
  if (sysctlbyname("vm.swapusage", &swap, &size, nullptr, 0) == 0)
  {
    s.swap_total = swap.xsu_total;
    s.swap_used = swap.xsu_used;
  }
  timeval boot{};
  size = sizeof boot;
  if (sysctlbyname("kern.boottime", &boot, &size, nullptr, 0) == 0)
    s.uptime = std::chrono::seconds(std::time(nullptr) - boot.tv_sec);

  const int wanted = proc_listallpids(nullptr, 0);
  impl.pids.resize(std::size_t(std::max(wanted, 0)) + 64);
  const int listed = proc_listallpids(impl.pids.data(), int(impl.pids.size() * sizeof(pid_t)));
  for (int i = 0; i < listed; ++i)
  {
    const pid_t pid = impl.pids[std::size_t(i)];
    process_t p;
    p.pid = std::uint32_t(pid);
    proc_taskallinfo all{};
    if (proc_pidinfo(pid, PROC_PIDTASKALLINFO, 0, &all, sizeof all) == int(sizeof all))
    {
      p.name = all.pbsd.pbi_name[0] ? all.pbsd.pbi_name : all.pbsd.pbi_comm;
      p.parent = all.pbsd.pbi_ppid;
      p.state = state_of(all.pbsd.pbi_status);
      p.threads = std::uint32_t(all.ptinfo.pti_threadnum);
      p.memory = all.ptinfo.pti_resident_size;
      p.cpu_ns = (all.ptinfo.pti_total_user + all.ptinfo.pti_total_system) * impl.timebase.numer / impl.timebase.denom;
    }
    else
    {
      p.partial = true;
      proc_bsdshortinfo brief{};
      if (proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &brief, sizeof brief) == int(sizeof brief))
      {
        p.name = brief.pbsi_comm;
        p.parent = brief.pbsi_ppid;
        p.state = state_of(brief.pbsi_status);
      }
      else
      {
        char name[2 * MAXCOMLEN] = {};
        proc_name(pid, name, sizeof name);
        p.name = name;
      }
    }
    if (p.name.empty() && pid == 0)
      p.name = "kernel_task";
    s.processes.push_back(std::move(p));
  }

  s.taken = std::chrono::steady_clock::now();
  s.cost = s.taken - started;
  return s;
}
} // namespace sys

#endif
