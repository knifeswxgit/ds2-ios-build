/**
 * @file        ios_log_overlay.mm
 * @brief       On-device log viewer: triple-tap top-center shows the log.
 *
 * Every spdlog line goes into a ring buffer and into Documents/ds2_log.txt, so
 * a black screen can be diagnosed both on the device and over USB.
 */

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>

#include <mach/mach.h>
#include <dlfcn.h>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>

#include <rex/logging/api.h>

#include "ds2_ios/ios_log_overlay.h"
#include "ds2_ios/ios_paths.h"

namespace {

constexpr size_t kMaxLines = 1500;
constexpr long kMaxFileBytes = 4 * 1024 * 1024;

std::mutex g_mutex;
std::deque<std::string> g_lines;
bool g_sink_installed = false;
bool g_overlay_installed = false;
bool g_visible = false;
FILE* g_file = nullptr;

void AppendLineLocked(const std::string& line) {
  g_lines.push_back(line);
  while (g_lines.size() > kMaxLines) g_lines.pop_front();
  if (!g_file) return;
  fwrite(line.data(), 1, line.size(), g_file);
  fputc('\n', g_file);
  if (g_lines.size() % 32 == 0) fflush(g_file);
}

std::string JoinLines() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::string out;
  out.reserve(128 * 1024);
  for (const auto& line : g_lines) {
    out += line;
    out += '\n';
  }
  return out;
}

class OverlaySink final : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    spdlog::memory_buf_t buf;
    formatter_->format(msg, buf);
    std::string line(buf.data(), buf.size());
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.empty()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    AppendLineLocked(line);
  }

  void flush_() override {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) fflush(g_file);
  }
};

UIWindow* KeyWindow() {
  if (@available(iOS 15.0, *)) {
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
      if (scene.activationState != UISceneActivationStateForegroundActive) continue;
      if (![scene isKindOfClass:UIWindowScene.class]) continue;
      for (UIWindow* w in ((UIWindowScene*)scene).windows) {
        if (w.isKeyWindow) return w;
      }
    }
  }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  return UIApplication.sharedApplication.keyWindow;
#pragma clang diagnostic pop
}

std::string JoinLinesCopy() {
  return JoinLines();
}

}  // namespace

// Routes triple taps to a block (UITapGestureRecognizer needs target/selector).
@interface DS2TapProxy : NSObject
@property(nonatomic, copy) void (^onTap)(void);
@property(nonatomic, strong) UITapGestureRecognizer* gesture;
- (void)tap:(UITapGestureRecognizer*)gr;
@end
@implementation DS2TapProxy
- (void)tap:(UITapGestureRecognizer*)gr {
  if (self.onTap) self.onTap();
}
@end

namespace {

UIView* g_panel = nil;
UITextView* g_text = nil;
NSTimer* g_timer = nil;

void RefreshText() {
  if (!g_text) return;
  std::string joined = JoinLines();
  NSString* str = [NSString stringWithUTF8String:joined.c_str()];
  if (!str) str = @"<log decode error>";
  NSRange end = {str.length, 0};
  [g_text setText:str];
  [g_text scrollRangeToVisible:end];
}

void HideOverlay() {
  g_visible = false;
  if (g_timer) {
    [g_timer invalidate];
    g_timer = nil;
  }
  if (g_panel) {
    [g_panel removeFromSuperview];
    g_panel = nil;
    g_text = nil;
  }
}

void ShowOverlay(UIWindow* window) {
  const CGRect bounds = window.bounds;
  const CGRect frame = CGRectMake(bounds.size.width * 0.04, bounds.size.height * 0.10,
                                  bounds.size.width * 0.92, bounds.size.height * 0.62);

  g_panel = [[UIView alloc] initWithFrame:frame];
  g_panel.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.93];
  g_panel.layer.cornerRadius = 12;
  g_panel.layer.borderColor = [UIColor.systemGreenColor colorWithAlphaComponent:0.5].CGColor;
  g_panel.layer.borderWidth = 1;
  g_panel.layer.zPosition = 99999;
  g_panel.userInteractionEnabled = YES;

  g_text = [[UITextView alloc] initWithFrame:CGRectMake(6, 30, frame.size.width - 12,
                                                        frame.size.height - 40)];
  g_text.backgroundColor = UIColor.clearColor;
  g_text.textColor = UIColor.greenColor;
  g_text.editable = NO;
  g_text.showsVerticalScrollIndicator = YES;
  if (@available(iOS 13.0, *)) {
    g_text.font = [UIFont monospacedSystemFontOfSize:8.5 weight:UIFontWeightRegular];
  } else {
    g_text.font = [UIFont fontWithName:@"Menlo" size:8.5];
  }
  [g_panel addSubview:g_text];

  UILabel* title = [[UILabel alloc] initWithFrame:CGRectMake(10, 6, frame.size.width - 110, 18)];
  title.text = @"DS2 log";
  title.textColor = UIColor.greenColor;
  title.font = [UIFont boldSystemFontOfSize:10];
  [g_panel addSubview:title];

  DS2TapProxy* proxy = [[DS2TapProxy alloc] init];
  proxy.onTap = ^{ HideOverlay(); [g_panel removeFromSuperview]; g_panel = nil; g_text = nil; };

  UIButton* copy = [UIButton buttonWithType:UIButtonTypeSystem];
  copy.frame = CGRectMake(frame.size.width - 100, 4, 48, 24);
  [copy setTitle:@"Copy" forState:UIControlStateNormal];
  copy.titleLabel.font = [UIFont boldSystemFontOfSize:11];
  [copy addTarget:proxy action:NSSelectorFromString(@"copyTap") forControlEvents:UIControlEventTouchUpInside];
  [g_panel addSubview:copy];

  UIButton* close = [UIButton buttonWithType:UIButtonTypeSystem];
  close.frame = CGRectMake(frame.size.width - 50, 4, 46, 24);
  [close setTitle:@"Close" forState:UIControlStateNormal];
  close.titleLabel.font = [UIFont boldSystemFontOfSize:11];
  [close addTarget:proxy action:NSSelectorFromString(@"closeTap") forControlEvents:UIControlEventTouchUpInside];
  [g_panel addSubview:close];

  objc_setAssociatedObject(g_panel, "ds2_proxy", proxy, OBJC_ASSOCIATION_RETAIN);

  RefreshText();
  [window addSubview:g_panel];

  g_timer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer* t) {
    if (!g_visible) {
      [t invalidate];
      return;
    }
    RefreshText();
  }];
  [[NSRunLoop mainRunLoop] addTimer:g_timer forMode:NSRunLoopCommonModes];

  g_visible = true;
}

void ToggleOverlay() {
  UIWindow* window = KeyWindow();
  if (!window) return;
  if (g_visible) {
    HideOverlay();
    return;
  }
  ShowOverlay(window);
}

}  // namespace

@implementation DS2TapProxy (Actions)
- (void)copyTap {
  UIPasteboard.generalPasteboard.string =
      [NSString stringWithUTF8String:JoinLinesCopy().c_str()];
}
- (void)closeTap {
  HideOverlay();
  [g_panel removeFromSuperview];
  g_panel = nil;
  g_text = nil;
}
@end

namespace ds2_ios {

void InstallLogSink() {
  static std::once_flag once;
  std::call_once(once, [] {
    NSString* docs = [NSString stringWithUTF8String:DocumentsPath().string().c_str()];
    NSString* path = [docs stringByAppendingPathComponent:@"ds2_log.txt"];
    NSDictionary* attrs = [NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
    if (attrs && [attrs fileSize] > kMaxFileBytes) {
      [NSFileManager.defaultManager removeItemAtPath:path error:nil];
    }
    g_file = fopen(path.UTF8String, "a");
    if (g_file) setvbuf(g_file, nullptr, _IOFBF, 64 * 1024);

    auto sink = std::make_shared<OverlaySink>();
    sink->set_pattern("[%H:%M:%S.%e] [%l] %v");
    rex::AddSink(sink);
    // Belt and braces: attach to every spdlog logger that exists (REXLOG uses
    // per-category loggers created lazily; the registry walk above can miss
    // ones created after this call, and default_logger may differ).
    spdlog::apply_all([&](const std::shared_ptr<spdlog::logger>& l) {
      if (l) l->sinks().push_back(sink);
    });
    if (spdlog::default_logger()) {
      spdlog::default_logger()->sinks().push_back(sink);
    }
    spdlog::flush_every(std::chrono::seconds(1));
    g_sink_installed = true;
  });
}

// ---------------------------------------------------------------------------
// Hang watchdog: if the main thread stops making progress, dump its stack.
// ---------------------------------------------------------------------------

mach_port_t g_main_thread = MACH_PORT_NULL;
std::chrono::steady_clock::time_point g_last_real_mark;
int g_hang_dumps = 0;

// Walk the arm64 frame-pointer chain of `thread` and log each return address
// with dladdr symbolication. Reads the target's stack through vm_read, so it
// is safe from another thread.
void DumpThreadStack(mach_port_t thread, const char* why) {
  char stamp[32];
  time_t now = time(nullptr);
  strftime(stamp, sizeof(stamp), "%H:%M:%S", localtime(&now));

  kern_return_t kr = thread_suspend(thread);
  if (kr != KERN_SUCCESS && kr != KERN_TERMINATED) {
    AppendLineLocked(std::string("[") + stamp + "] [hang] thread_suspend failed: " +
                     std::to_string(kr));
    return;
  }

  arm_unified_thread_state_t state{};
  mach_msg_type_number_t count = ARM_UNIFIED_THREAD_STATE_COUNT;
  kr = thread_get_state(thread, ARM_UNIFIED_THREAD_STATE, (thread_state_t)&state, &count);
  if (kr != KERN_SUCCESS) {
    AppendLineLocked(std::string("[") + stamp + "] [hang] thread_get_state failed: " +
                     std::to_string(kr));
    if (kr != KERN_TERMINATED) thread_resume(thread);
    return;
  }

  uint64_t pc = state.ts_64.__pc;
  uint64_t fp = state.ts_64.__fp;
  AppendLineLocked(std::string("[") + stamp + "] [hang] " + why +
                   " — main thread stack (pc=0x" + [](uint64_t v) {
                     char b[24]; snprintf(b, sizeof(b), "%llx", (unsigned long long)v); return std::string(b);
                   }(pc) + "):");

  for (int i = 0; i < 64 && fp != 0; ++i) {
    // arm64 frame: [fp] = next fp, [fp+8] = return address
    uint64_t buf[2] = {0, 0};
    vm_size_t got = 0;
    kr = vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof(buf),
                           (vm_address_t)buf, &got);
    if (kr != KERN_SUCCESS || got != sizeof(buf)) break;
    uint64_t lr = buf[1];

    Dl_info info{};
    std::string sym = "0x" + [](uint64_t v) {
      char b[24]; snprintf(b, sizeof(b), "%llx", (unsigned long long)v); return std::string(b);
    }(lr);
    if (dladdr((const void*)lr, &info) && info.dli_sname) {
      sym = std::string(info.dli_sname) + " + " +
            std::to_string((long)(lr - (uint64_t)info.dli_saddr));
    } else if (info.dli_fname) {
      sym += " (" + std::string(info.dli_fname) + ")";
    }
    AppendLineLocked("    #" + std::to_string(i) + " " + sym);

    if (buf[0] <= fp) break;  // fp chain must move upward
    fp = buf[0];
  }

  if (kr == KERN_SUCCESS) thread_resume(thread);
  AppendLineLocked(std::string("[") + stamp + "] [hang] stack dump end");
  if (g_file) fflush(g_file);
}

void HangWatchdogTick() {
  // Called from the heartbeat thread only. Heartbeat marks never refresh
  // g_last_real_mark, so a stuck main thread is detected here.
  if (g_main_thread == MACH_PORT_NULL) return;
  auto idle = std::chrono::steady_clock::now() - g_last_real_mark;
  if (idle < std::chrono::seconds(25)) return;
  // dump every ~30s while the hang persists (heartbeat = 5s, dump on 5th)
  if (g_hang_dumps >= 6) return;  // enough evidence collected
  ++g_hang_dumps;
  DumpThreadStack(g_main_thread, "no main-thread progress for 25s+");
}

void DebugMark(const char* stage) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_file) {
    NSString* docs = [NSString stringWithUTF8String:DocumentsPath().string().c_str()];
    NSString* path = [docs stringByAppendingPathComponent:@"ds2_log.txt"];
    NSDictionary* attrs = [NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
    if (attrs && [attrs fileSize] > kMaxFileBytes) {
      [NSFileManager.defaultManager removeItemAtPath:path error:nil];
    }
    g_file = fopen(path.UTF8String, "a");
    if (g_file) setvbuf(g_file, nullptr, _IOFBF, 64 * 1024);
  }
  // The very first mark comes from the main() thread; remember its mach port
  // so the hang watchdog can later dump exactly that thread's stack.
  if (g_main_thread == MACH_PORT_NULL) {
    g_main_thread = mach_thread_self();
  }
  // Only marks made BY the main thread count as progress; heartbeat marks
  // arrive from the watchdog thread and must not reset the hang timer.
  {
    mach_port_t self_t = mach_thread_self();
    const bool is_main = (self_t == g_main_thread);
    mach_port_deallocate(mach_task_self(), self_t);
    if (is_main) {
      g_last_real_mark = std::chrono::steady_clock::now();
    }
  }
  time_t now = time(nullptr);
  char stamp[32];
  strftime(stamp, sizeof(stamp), "%H:%M:%S", localtime(&now));
  std::string line = std::string("[") + stamp + "] [mark] " + stage;
  AppendLineLocked(line);
  if (g_file) fflush(g_file);

  // Once the first mark lands, start a background heartbeat: if the main
  // thread wedges inside some init call, heartbeats keep appearing and prove
  // the process itself is alive (vs suspended by the system).
  static std::once_flag hb_flag;
  std::call_once(hb_flag, [] {
    std::thread([] {
      int n = 0;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        DebugMark(("heartbeat " + std::to_string(++n)).c_str());
        HangWatchdogTick();
      }
    }).detach();
  });
}

void InstallLogOverlay() {
  if (g_overlay_installed) return;
  g_overlay_installed = true;
  // No standalone UI any more: the log panel is toggled from the controls
  // settings panel (SET -> Show logs) via ToggleLogOverlay below. The sink is
  // installed separately by InstallLogSink().
}

void ToggleLogOverlay() {
  dispatch_async(dispatch_get_main_queue(), ^{ ToggleOverlay(); });
}

}  // namespace ds2_ios
