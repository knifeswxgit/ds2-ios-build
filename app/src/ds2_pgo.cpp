// ds2 - v108: profile collection for profile-guided optimisation of the recompiled game code (DS2_PGO=GEN builds).
//
// devkitA64's libgcov is built without a C library, so the usual dump at exit writes nothing. The game code is
// compiled with -fprofile-arcs -fprofile-info-section=ds2_gcov_info: the compiler lists every gcov_info in that
// section, and __gcov_info_to_gcda serialises one into the bytes of its .gcda file. A thread writes all of them
// to sdmc:/switch/ds2/pgo/ every few minutes (each write replaces the previous one: counters only grow), with the
// object path in the file name ('/' -> '#'). tools/pgo/restore_gcda.py puts them back next to the objects.

#include "ds2_pgo.h"

#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <thread>

#if defined(DS2_PGO_GEN)

struct gcov_info;
extern "C" {
extern const gcov_info* const __start_ds2_gcov_info[];
extern const gcov_info* const __stop_ds2_gcov_info[];
void __gcov_info_to_gcda(const gcov_info* info, void (*filename_fn)(const char*, void*),
                         void (*dump_fn)(const void*, unsigned, void*), void* (*allocate_fn)(unsigned, void*),
                         void* arg);
}

namespace {

constexpr const char* kDir = "sdmc:/switch/ds2/pgo";
std::atomic<bool> g_stop{false};
std::thread g_thread;

struct Writer {
  FILE* f = nullptr;
  bool ok = true;
};

void OnFilename(const char* name, void* arg) {
  auto* w = static_cast<Writer*>(arg);
  std::string mangled(name);
  for (char& c : mangled) {
    if (c == '/') c = '#';
  }
  const std::string path = std::string(kDir) + "/" + mangled;
  w->f = std::fopen(path.c_str(), "wb");
  w->ok = w->f != nullptr;
}

void OnData(const void* data, unsigned size, void* arg) {
  auto* w = static_cast<Writer*>(arg);
  if (w->f && std::fwrite(data, 1, size, w->f) != size) w->ok = false;
}

void* OnAllocate(unsigned size, void* arg) {
  (void)arg;
  return std::malloc(size);
}

void DumpAll() {
  mkdir(kDir, 0777);
  size_t files = 0, failed = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (const gcov_info* const* p = __start_ds2_gcov_info; p < __stop_ds2_gcov_info; ++p) {
    if (!*p) continue;
    Writer w;
    __gcov_info_to_gcda(*p, OnFilename, OnData, OnAllocate, &w);
    if (w.f) std::fclose(w.f);
    ++files;
    if (!w.ok) ++failed;
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  REXLOG_INFO("ds2 pgo: profile written to {} ({} files, {} failed, {:.0f} ms)", kDir, files, failed, ms);
}

}  // namespace

namespace ds2::pgo {

void Start() {
  REXLOG_INFO("ds2 pgo: profile build; the profile is written every 3 minutes to {}", kDir);
  g_thread = std::thread([]() {
    while (!g_stop.load()) {
      for (int i = 0; i < 180 && !g_stop.load(); ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
      DumpAll();
    }
  });
}

void Stop() {
  g_stop.store(true);
  if (g_thread.joinable()) g_thread.join();
}

}  // namespace ds2::pgo

#else

namespace ds2::pgo {
void Start() {}
void Stop() {}
}  // namespace ds2::pgo

#endif
