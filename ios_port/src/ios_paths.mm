#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>

#include <filesystem>

#include "ds2_ios/ios_paths.h"
#include "ds2_ios/ios_controls_layout.h"

namespace ds2_ios {
namespace {
void (*g_low_memory_callback)() = nullptr;

UIWindow* KeyWindow() {
  if (@available(iOS 15.0, *)) {
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
      if (scene.activationState == UISceneActivationStateForegroundActive &&
          [scene isKindOfClass:UIWindowScene.class]) {
        for (UIWindow* w in ((UIWindowScene*)scene).windows) {
          if (w.isKeyWindow) return w;
        }
      }
    }
  }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  return UIApplication.sharedApplication.keyWindow;
#pragma clang diagnostic pop
}
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
  UIWindow* window = KeyWindow();
  UIView* root = window.rootViewController.view;
  if (!root || [root viewWithTag:0x44533249]) return;  // already installed

  UIView* overlay = [[UIView alloc] initWithFrame:root.bounds];
  overlay.tag = 0x44533249;  // "DS2I"
  overlay.userInteractionEnabled = NO;
  overlay.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  overlay.backgroundColor = UIColor.clearColor;

  auto ring = ^(CGRect r, UIColor* color) {
    CAShapeLayer* layer = [CAShapeLayer layer];
    layer.path = [UIBezierPath bezierPathWithOvalInRect:r].CGPath;
    layer.strokeColor = color.CGColor;
    layer.fillColor = [color colorWithAlphaComponent:0.08].CGColor;
    layer.lineWidth = 2.0;
    [overlay.layer addSublayer:layer];
  };
  auto box = ^(CGRect r, NSString* label) {
    CAShapeLayer* layer = [CAShapeLayer layer];
    layer.path = [UIBezierPath bezierPathWithRoundedRect:r cornerRadius:10].CGPath;
    layer.strokeColor = [UIColor.whiteColor colorWithAlphaComponent:0.35].CGColor;
    layer.fillColor = [UIColor.whiteColor colorWithAlphaComponent:0.07].CGColor;
    layer.lineWidth = 1.5;
    [overlay.layer addSublayer:layer];
    CATextLayer* text = [CATextLayer layer];
    text.string = label;
    text.frame = r;
    text.alignmentMode = kCAAlignmentCenter;
    text.foregroundColor = [UIColor.whiteColor colorWithAlphaComponent:0.6].CGColor;
    text.fontSize = 13;
    text.contentsScale = UIScreen.mainScreen.scale;
    [overlay.layer addSublayer:text];
  };

  const CGFloat w = CGRectGetWidth(root.bounds), h = CGRectGetHeight(root.bounds);
  UIColor* stick = [UIColor.systemBlueColor colorWithAlphaComponent:0.55];
  ring(CGRectMake(w * 0.04, h * 0.55, w * 0.22, w * 0.22), stick);   // left stick
  ring(CGRectMake(w * 0.74, h * 0.55, w * 0.22, w * 0.22), stick);   // right stick

  // Draw exactly the same normalized rectangles the input driver hit-tests,
  // so every drawn button is pressable and nothing hangs off the right edge.
  for (const auto& z : kTouchZones) {
    box(CGRectMake(w * z.x0, h * z.y0, w * (z.x1 - z.x0), h * (z.y1 - z.y0)),
        [NSString stringWithUTF8String:z.label]);
  }

  [root addSubview:overlay];
}

}  // namespace ds2_ios
