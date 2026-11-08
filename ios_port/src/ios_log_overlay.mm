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
constexpr int kMaxWindowRetries = 40;

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
  title.text = @"DS2 log  (3 taps top-center = close)";
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
    g_sink_installed = true;
  });
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
      }
    }).detach();
  });
}

void InstallLogOverlay() {
  if (g_overlay_installed) return;
  g_overlay_installed = true;

  dispatch_async(dispatch_get_main_queue(), ^{
    __block int retries = 0;
    void (^try_install)(void) = nil;
    try_install = ^{
      UIWindow* window = KeyWindow();
      if (!window) {
        if (++retries < kMaxWindowRetries) {
          dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                         dispatch_get_main_queue(), try_install);
        }
        return;
      }
      DS2TapProxy* proxy = [[DS2TapProxy alloc] init];
      proxy.onTap = ^{ dispatch_async(dispatch_get_main_queue(), ^{ ToggleOverlay(); }); };
      UITapGestureRecognizer* tap =
          [[UITapGestureRecognizer alloc] initWithTarget:proxy action:NSSelectorFromString(@"tap:")];
      tap.numberOfTapsRequired = 3;
      tap.cancelsTouchesInView = NO;  // game input keeps working
      proxy.gesture = tap;
      objc_setAssociatedObject(window, "ds2_log_tap_proxy", proxy, OBJC_ASSOCIATION_RETAIN);
      [window addGestureRecognizer:tap];
    };
    try_install();
  });
}

}  // namespace ds2_ios
