#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>

#include <filesystem>

#include "ds2_ios/ios_paths.h"
#include "ds2_ios/ios_controls_layout.h"

namespace ds2_ios {
namespace {
void (*g_low_memory_callback)() = nullptr;
}  // namespace

std::filesystem::path DocumentsPath() {
  NSArray<NSString*>* dirs = NSSearchPathForDirectoriesInDomains(
      NSDocumentDirectory, NSUserDomainMask, YES);
  return std::filesystem::path([dirs.firstObject UTF8String]);
}

std::filesystem::path BundlePath() {
  return std::filesystem::path([[[NSBundle mainBundle] bundlePath] UTF8String]);
}

void SetLowMemoryCallback(void (*callback)()) { g_low_memory_callback = callback; }

void InstallMemoryWarningHandler() {
  [[NSNotificationCenter defaultCenter]
      addObserverForName:UIApplicationDidReceiveMemoryWarningNotification
                  object:nil
                   queue:NSOperationQueue.mainQueue
              usingBlock:^(NSNotification*) {
                if (g_low_memory_callback) g_low_memory_callback();
              }];
}

void InstallTouchOverlay() {
  // The drawn buttons, drag-to-move and the settings panel all live in
  // ios_controls.mm now; this is just the call the input driver makes.
  InstallControlsOverlay();
}

}  // namespace ds2_ios
