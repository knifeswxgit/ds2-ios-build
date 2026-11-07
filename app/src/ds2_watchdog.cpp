// ds2 - load/hang telemetry, see ds2_watchdog.h.

#include "ds2_watchdog.h"

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/logging.h>
#include <rex/filesystem.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xfile.h>
#include <rex/system/xthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <malloc.h>
#include <deque>
#include <filesystem>
#include <unistd.h>
#include <map>
#include <set>
#include <string>
#include <thread>

#include "ds2_switch_platform.h"
#include "ds2_func_counts.h"

REXCVAR_DEFINE_BOOL(ds2_switch_stack_sampling, false, "DS2",
                    "Switch profiler: sample thread stacks into rex_perfil.log (same as logs/rex/perfil_pilas.flag)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#if defined(__SWITCH__)
extern "C" void RexSwitchPerfRequestStackSampling(bool enable);
#endif

REXCVAR_DEFINE_BOOL(ds2_watchdog, true, "DS2",
                    "Load/hang telemetry in the main log: snapshots of guest threads, open files and memory, "
                    "file open/close events and stall alarms")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(ds2_export_stats, false, "DS2",
                    "DIAGNOSTIC: every heartbeat, log how many times each kernel/XAM export was called since the "
                    "previous one");
REXCVAR_DEFINE_INT32(ds2_watchdog_snapshot_s, 10, "DS2", "Seconds between [ds2 wd] snapshots (0 = only on stalls)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(ds2_watchdog_heartbeat_s, 2, "DS2",
                     "Seconds between one-line [ds2 wd] hb heartbeats: vblank counter and the shaders the GPU command "
                     "processor last bound (0 = off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Environment for Mesa/NVK, applied before Vulkan is created (as nfsmw_mesa_entorno in nfsmw-nx), e.g.
// "NVK_SWITCH_NO_UBO_CBUF=1;MESA_SHADER_CACHE_DISABLE=true".
REXCVAR_DEFINE_INT32(ds2_watchdog_probe_pages, 0, "DS2",
                     "Switch DIAGNOSTIC: every heartbeat, test how many more small kernel mapping changes still "
                     "succeed, up to this many (0 = off). Shows the kernel resource running out")
    .range(0, 1024)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(ds2_watchdog_probe_tables, 32, "DS2",
                     "Switch DIAGNOSTIC: every heartbeat, test how many more kernel page tables can still be taken, "
                     "up to this many (max 128, 0 = off). Without a system resource of its own the process shares "
                     "them with the whole system (pt_left in the heartbeat)")
    .range(0, 128)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(ds2_switch_commit_granule_kb, 2048, "DS2",
                     "Switch: guest memory is committed in aligned pieces of this size (KB, power of two). 2048 lets "
                     "the kernel map each piece without page tables; 4 = the SDK's original exact commits")
    .range(4, 65536)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(ds2_switch_nvmap_mb, 0, "DS2",
                     "Switch: MB of the transfer memory libnx gives nvservices for GPU memory bookkeeping (libnx: 8). "
                     "0 = leave libnx's value")
    .range(0, 256)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(ds2_mesa_env, "", "DS2", "Mesa/NVK environment variables, 'NAME=value;NAME=value'")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// On by default in nfsmw-nx (nfsmw_switch_saltynx). Off here until the first console runs are understood: every
// second until it succeeds it takes the single session of SaltyNX's "SaltySD" port, which Status-Monitor-Overlay
// also needs, and from the third try it may release this process's "sm:" session for a few milliseconds.
REXCVAR_DEFINE_BOOL(ds2_switch_saltynx, false, "DS2",
                    "Switch: publish FPS/resolution to SaltyNX overlays (Status Monitor) and follow ReverseNX")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// v55: clocks for the session (through the clock sysmodule, Horizon OC / sys-clk). 0 = leave that clock alone.
REXCVAR_DEFINE_INT32(ds2_switch_cpu_mhz, 0, "DS2", "Switch: CPU clock for the session in MHz (0 = not touched)")
    .range(0, 3000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(ds2_switch_gpu_mhz, 0, "DS2", "Switch: GPU clock for the session in MHz (0 = not touched)")
    .range(0, 2000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(ds2_switch_mem_mhz, 0, "DS2", "Switch: memory clock for the session in MHz (0 = not touched)")
    .range(0, 4000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__SWITCH__)
extern "C" void RexSwitchRelojesFijar(unsigned cpu_mhz, unsigned gpu_mhz, unsigned mem_mhz);
#endif

namespace ds2::watchdog {

void ApplyPlatformSwitches() {
#if defined(__SWITCH__)
  RexSwitchPerfRequestStackSampling(REXCVAR_GET(ds2_switch_stack_sampling));
  RexSwitchRelojesFijar(unsigned(REXCVAR_GET(ds2_switch_cpu_mhz)), unsigned(REXCVAR_GET(ds2_switch_gpu_mhz)),
                        unsigned(REXCVAR_GET(ds2_switch_mem_mhz)));
  REXLOG_INFO("[ds2] session clocks requested: CPU {} MHz, GPU {} MHz, memory {} MHz (0 = not touched)",
              REXCVAR_GET(ds2_switch_cpu_mhz), REXCVAR_GET(ds2_switch_gpu_mhz), REXCVAR_GET(ds2_switch_mem_mhz));
#endif
  ds2::platform::SetSaltyNxBridge(REXCVAR_GET(ds2_switch_saltynx));
  REXLOG_INFO("[ds2] SaltyNX overlay bridge: {}", REXCVAR_GET(ds2_switch_saltynx) ? "on" : "off");
  {
    const size_t g = ds2::platform::SetCommitGranule(size_t(REXCVAR_GET(ds2_switch_commit_granule_kb)) << 10);
    const auto k = ds2::platform::QueryKernelStats();
    REXLOG_INFO("[ds2] guest commit granule: {} KB; own system resource: {} KB{}", g >> 10, k.sysres_total >> 10,
                k.sysres_total ? "" : " (none: page tables come from the kernel heap shared with the system)");
  }
  REXLOG_INFO("[ds2] nvservices transfer memory: {} MB", ds2::platform::SetNvTransferMemMb(REXCVAR_GET(ds2_switch_nvmap_mb)));
  const std::string env = REXCVAR_GET(ds2_mesa_env);
  size_t start = 0;
  while (start < env.size()) {
    size_t end = env.find(';', start);
    if (end == std::string::npos) end = env.size();
    const std::string item = env.substr(start, end - start);
    start = end + 1;
    const size_t eq = item.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    const std::string name = item.substr(0, eq), value = item.substr(eq + 1);
    const int rc = setenv(name.c_str(), value.c_str(), 1);
    REXLOG_INFO("[ds2] mesa env {}={}{}", name, value, rc ? " (setenv failed)" : "");
  }
}

namespace {

std::thread g_thread;
std::atomic<bool> g_run{false};
rex::Runtime* g_runtime = nullptr;

// The main log can lose its last seconds when the console is switched off by holding POWER (data handed to the
// file system but not committed). logs/ds2_wd_last.txt is rewritten and fsync'ed on every heartbeat, so after a
// forced power-off it still says when the process was last alive and what it was doing.
std::deque<std::string> g_events;  // recent file events, watchdog thread only
std::string g_histogram;           // last kernel block histogram, watchdog thread only
uint64_t g_histogram_blocks = 0;

void Note(std::string line) {
  g_events.push_back(std::move(line));
  while (g_events.size() > 24) g_events.pop_front();
}

void WriteDurable(const std::string& heartbeat) {
  static const std::filesystem::path path = [] {
    const auto dir = rex::filesystem::GetExecutableFolder();
    if (dir.empty()) return std::filesystem::path();
    std::error_code ec;
    std::filesystem::create_directories(dir / "logs", ec);
    return dir / "logs" / "ds2_wd_last.txt";
  }();
  if (path.empty()) return;
  // Written to a temporary file and renamed, so a failing write (the kernel ran out of resources) leaves the
  // previous version instead of an empty file.
  const std::string tmp = path.string() + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "w");
  if (!f) return;
  std::fprintf(f, "%s\nkernel blocks by kind (at %llu blocks): %s\nrecent file events:\n", heartbeat.c_str(),
               (unsigned long long)g_histogram_blocks, g_histogram.c_str());
  for (const auto& e : g_events) std::fprintf(f, "  %s\n", e.c_str());
  const bool ok = std::fflush(f) == 0 && fsync(fileno(f)) == 0 && !std::ferror(f);
  std::fclose(f);
  if (ok) {
    std::remove(path.string().c_str());
    std::rename(tmp.c_str(), path.string().c_str());
  }
}

// One line: is the process alive (this line keeps coming), does the guest see vblanks, and is the GPU command
// processor still moving (the shaders it binds change while it executes the game's draws).
void LogHeartbeat(int seconds, size_t threads, size_t files) {
  uint32_t vblank = 0;
  uint64_t vs = 0, ps = 0;
  bool paused = false;
  auto* igs = g_runtime ? g_runtime->graphics_system() : nullptr;
  if (auto* gs = static_cast<rex::graphics::GraphicsSystem*>(igs)) {
    if (auto* cp = gs->command_processor()) {
      vblank = cp->counter();
      paused = cp->is_paused();
      if (auto* v = cp->active_vertex_shader()) vs = v->ucode_data_hash();
      if (auto* p = cp->active_pixel_shader()) ps = p->ucode_data_hash();
    }
  }
  const struct mallinfo mi = mallinfo();
  const auto k = ds2::platform::QueryKernelStats();
  std::string probe;
  if (const int pp = REXCVAR_GET(ds2_watchdog_probe_pages); pp > 0) {
    uint32_t rc = 0;
    const uint32_t left = ds2::platform::ProbeKernelHeadroom(uint32_t(pp), &rc);
    probe = rc ? fmt::format(" headroom={} (0x{:X})", left, rc) : fmt::format(" headroom>={}", left);
  }
  if (const int pt = REXCVAR_GET(ds2_watchdog_probe_tables); pt > 0) {
    uint32_t rc = 0;
    const uint32_t left = ds2::platform::ProbePageTableHeadroom(uint32_t(pt), &rc);
    probe += rc ? fmt::format(" pt_left={} (0x{:X})", left, rc) : fmt::format(" pt_left>={}", left);
  }
  if (k.valid) probe += fmt::format(" pt_win={}/{} chunks={}", k.pt_windows_split, k.pt_windows_mapped, k.chunks);
  const std::string line = fmt::format(
      "[ds2 wd] t={}s hb vblank={} cp_vs={:016X} cp_ps={:016X}{} threads={} files={} malloc_mb={} blocks={} "
      "sysres_kb={}/{} merged_runs={} merge_fail={}",
      seconds, vblank, vs, ps, paused ? " cp_paused" : "", threads, files, size_t(mi.uordblks) >> 20, k.blocks,
      k.sysres_used >> 10, k.sysres_total >> 10, k.coalesced, k.coalesce_failures) + probe;
  REXLOG_INFO("{}", line);
  // Kernel/XAM exports called since the previous heartbeat (all calls, sorted by count).
  if (REXCVAR_GET(ds2_export_stats)) {
    std::vector<std::pair<uint64_t, const char*>> called;
    for (auto* c = rex::ppc::ExportCallCounterHead(); c; c = c->next) {
      const uint64_t now_calls = c->calls.load(std::memory_order_relaxed);
      if (now_calls != c->reported) {
        called.emplace_back(now_calls - c->reported, c->name);
        c->reported = now_calls;
      }
    }
    std::sort(called.begin(), called.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string list;
    for (const auto& [n, name] : called) {
      list += fmt::format(" {}={}", name, n);
    }
    REXLOG_INFO("[ds2 wd] t={}s exports:{}", seconds, list);
  }
  // A fast rise in memory blocks is what preceded every 2001-0103 so far: record what kind of blocks they are.
  if (k.valid && (g_histogram.empty() || k.blocks > g_histogram_blocks + 500 || k.blocks + 500 < g_histogram_blocks)) {
    g_histogram = ds2::platform::KernelBlockHistogram(8);
    g_histogram_blocks = k.blocks;
    REXLOG_INFO("[ds2 wd] t={}s kernel blocks by kind ({}): {}", seconds, k.blocks, g_histogram);
  }
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  char stamp[32] = {};
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
  WriteDurable(std::string(stamp) + " " + line);
}

constexpr int kStallSeconds = 5;
constexpr int kStallRepeatSeconds = 15;

void LogMemory(const char* tag) {
  uint64_t used = 0, total = 0;
  const struct mallinfo mi = mallinfo();
  if (ds2::platform::HostMemory(&used, &total)) {
    const auto k = ds2::platform::QueryKernelStats();
    REXLOG_INFO("[ds2 wd] {} memory: host {} / {} MB, malloc arena {} MB in use {} MB, guest backing committed {} MB, "
                "mapped {} MB | kernel: {} memory blocks ({} inaccessible), system resource {} / {} KB, "
                "guest mapper last result 0x{:X}",
                tag, used >> 20, total >> 20, size_t(mi.arena) >> 20, size_t(mi.uordblks) >> 20,
                ds2::platform::GuestCommitted() >> 20, ds2::platform::GuestMapped() >> 20, k.blocks,
                k.blocks_inaccessible, k.sysres_used >> 10, k.sysres_total >> 10, k.last_gm);
  } else {
    REXLOG_INFO("[ds2 wd] {} memory: malloc arena {} MB in use {} MB", tag, size_t(mi.arena) >> 20,
                size_t(mi.uordblks) >> 20);
  }
}

template <typename Threads>
void LogThreads(const Threads& threads, bool error) {
  for (auto& t : threads) {
    const auto* cp = t->creation_params();
    auto* st = t->thread_state();
    if (!st || !st->context()) {
      REXLOG_INFO("[ds2 wd]   thread 0x{:X} entry=0x{:08X} '{}' no context", t->thread_id(), cp->start_address,
                  t->name());
      continue;
    }
    const auto& c = *st->context();
    if (error) {
      REXLOG_ERROR("[ds2 wd]   thread 0x{:X} entry=0x{:08X} '{}' main={} running={} lr=0x{:08X} r1=0x{:08X} "
                   "r3=0x{:08X} ctr=0x{:08X} last_indirect=0x{:08X}",
                   t->thread_id(), cp->start_address, t->name(), t->main_thread(), t->is_running(),
                   static_cast<uint32_t>(c.lr), c.r1.u32, c.r3.u32, c.ctr.u32, c.last_indirect_target);
    } else {
      REXLOG_INFO("[ds2 wd]   thread 0x{:X} entry=0x{:08X} '{}' main={} running={} lr=0x{:08X} r1=0x{:08X} "
                  "r3=0x{:08X} ctr=0x{:08X} last_indirect=0x{:08X}",
                  t->thread_id(), cp->start_address, t->name(), t->main_thread(), t->is_running(),
                  static_cast<uint32_t>(c.lr), c.r1.u32, c.r3.u32, c.ctr.u32, c.last_indirect_target);
    }
  }
}

void Main() {
  const int every = REXCVAR_GET(ds2_watchdog_snapshot_s);
  const int heartbeat = REXCVAR_GET(ds2_watchdog_heartbeat_s);
  std::multiset<std::string> files_before;
  uint64_t sig_before = 0;
  int still = 0, since_alarm = 0, since_snapshot = 0, seconds = 0;
  bool alarmed = false;
  REXLOG_INFO("[ds2 wd] started (snapshot every {} s, stall after {} s)", every, kStallSeconds);
  LogMemory("start");
  rex::FlushLogging();

  while (g_run.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!g_run.load()) break;
    ++seconds;
    ds2::func_counts::Tick(seconds);
    auto* kernel = rex::system::kernel_state();
    if (!kernel) continue;

    auto threads = kernel->object_table()->GetObjectsByType<rex::system::XThread>();
    auto files = kernel->object_table()->GetObjectsByType<rex::system::XFile>();
    bool wrote = false;

    // Open guest files: what the loader is working on. Logged as differences, so a load leaves a trail.
    std::multiset<std::string> files_now;
    for (auto& f : files) files_now.insert(f->path());
    if (files_now != files_before) {
      for (const auto& p : files_now)
        if (files_before.count(p) < files_now.count(p)) {
          REXLOG_INFO("[ds2 wd] t={}s file + {}", seconds, p);
          Note(fmt::format("t={}s + {}", seconds, p));
        }
      for (const auto& p : files_before)
        if (files_now.count(p) < files_before.count(p)) {
          REXLOG_INFO("[ds2 wd] t={}s file - {}", seconds, p);
          Note(fmt::format("t={}s - {}", seconds, p));
        }
      wrote = true;
    }

    // Signature of where every guest thread is (sorted by id: the table order is not stable).
    std::map<uint32_t, uint64_t> per_thread;
    for (auto& t : threads) {
      auto* st = t->thread_state();
      if (!st || !st->context()) continue;
      const auto& c = *st->context();
      per_thread[t->thread_id()] = static_cast<uint64_t>(c.lr) ^ (static_cast<uint64_t>(c.r1.u32) << 20) ^
                                   (static_cast<uint64_t>(c.r3.u32) << 40);
    }
    uint64_t sig = 1469598103934665603ull;
    for (const auto& [id, v] : per_thread) {
      sig = (sig ^ id) * 1099511628211ull;
      sig = (sig ^ v) * 1099511628211ull;
    }
    sig = (sig ^ files_now.size()) * 1099511628211ull;

    if (heartbeat > 0 && seconds % heartbeat == 0) {
      LogHeartbeat(seconds, threads.size(), files_now.size());
      wrote = true;
    }

    if (every > 0 && ++since_snapshot >= every) {
      since_snapshot = 0;
      REXLOG_INFO("[ds2 wd] t={}s snapshot: {} guest threads, {} open files", seconds, threads.size(),
                  files_now.size());
      LogMemory("snapshot");
      if (const std::string r = ds2::platform::ResourceLimits(); !r.empty())
        REXLOG_INFO("[ds2 wd] t={}s resource limits: {}", seconds, r);
      if (const std::string h = ds2::platform::KernelBlockHistogram(8); !h.empty())
        REXLOG_INFO("[ds2 wd] t={}s kernel blocks by kind: {}", seconds, h);
      LogThreads(threads, false);
      wrote = true;
    }

    if (sig != sig_before || files_now != files_before) {
      if (alarmed) {
        REXLOG_WARN("[ds2 wd] t={}s moving again after {} s", seconds, still);
        wrote = true;
      }
      alarmed = false;
      still = 0;
      since_alarm = 0;
    } else {
      ++still;
      ++since_alarm;
      if (still >= kStallSeconds && (!alarmed || since_alarm >= kStallRepeatSeconds)) {
        since_alarm = 0;
        alarmed = true;
        REXLOG_ERROR("[ds2 wd] t={}s STALL: {} s without any change in {} guest threads and {} open files",
                     seconds, still, threads.size(), files_now.size());
        LogMemory("stall");
        if (const std::string h = ds2::platform::KernelBlockHistogram(8); !h.empty())
          REXLOG_ERROR("[ds2 wd] t={}s kernel blocks by kind: {}", seconds, h);
        LogThreads(threads, true);
        wrote = true;
      }
    }
    sig_before = sig;
    files_before = std::move(files_now);
    if (wrote) rex::FlushLogging();
  }
}

}  // namespace

void Start(rex::Runtime* runtime) {
  if (!REXCVAR_GET(ds2_watchdog) || g_run.exchange(true)) return;
  g_runtime = runtime;
  g_thread = std::thread(Main);
}

void Stop() {
  if (!g_run.exchange(false)) return;
  if (g_thread.joinable()) g_thread.join();
}

}  // namespace ds2::watchdog
