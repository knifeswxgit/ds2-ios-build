/**
 * @file        ios_controls.mm
 * @brief       On-screen touch controls: drawing, drag-to-move, display
 *              scaling and the single settings button ("SET").
 *
 * The input driver (ios_touch_input_driver.cpp) hit-tests against the same
 * runtime-mutable zone list (ds2_ios/ios_controls_layout.h), so what is
 * drawn is exactly what is pressed. The layout persists in NSUserDefaults.
 */

#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>

#include <algorithm>
#include <string>
#include <vector>

#include "ds2_ios/ios_controls_layout.h"
#include "ds2_ios/ios_paths.h"
#include "ds2_ios/ios_log_overlay.h"

namespace ds2_ios {

// ---------------------------------------------------------------------------
// Default layout (landscape, normalized 0..1). Button masks are
// X_INPUT_GAMEPAD_* values. button == 0 marks an analog trigger zone.
// ---------------------------------------------------------------------------
const TouchZone kDefaultTouchZones[] = {
    {0.82f, 0.40f, 0.90f, 0.55f, 0x1000, "A"},
    {0.91f, 0.40f, 0.99f, 0.55f, 0x2000, "B"},
    {0.82f, 0.24f, 0.90f, 0.39f, 0x4000, "X"},
    {0.91f, 0.24f, 0.99f, 0.39f, 0x8000, "Y"},
    {0.00f, 0.28f, 0.08f, 0.38f, 0x0100, "LB"},
    {0.92f, 0.28f, 1.00f, 0.38f, 0x0200, "RB"},
    {0.00f, 0.15f, 0.08f, 0.25f, 0x0000, "LT"},
    {0.92f, 0.15f, 1.00f, 0.25f, 0x0000, "RT"},
    {0.40f, 0.00f, 0.50f, 0.07f, 0x0010, "START"},
    {0.50f, 0.00f, 0.60f, 0.07f, 0x0020, "BACK"},
    {0.00f, 0.44f, 0.05f, 0.52f, 0x0001, "^"},
    {0.00f, 0.52f, 0.05f, 0.60f, 0x0002, "v"},
    {0.05f, 0.44f, 0.10f, 0.52f, 0x0004, "<"},
    {0.05f, 0.52f, 0.10f, 0.60f, 0x0008, ">"},
};
const int kDefaultTouchZoneCount =
    static_cast<int>(sizeof(kDefaultTouchZones) / sizeof(kDefaultTouchZones[0]));

namespace {

std::vector<TouchZone> g_zones;
UIView* g_root = nil;               // transparent container for the buttons
std::vector<UIView*> g_zone_views;  // one view per zone, same order
UIButton* g_gear = nil;             // single settings button
UIView* g_panel = nil;              // settings panel
bool g_edit_mode = false;

NSString* const kLayoutKey = @"ds2_touch_layout_v1";
NSString* const kScaleKey = @"ds2_display_scale_v1";

// ---------------------------------------------------------------------------
// Layout storage
// ---------------------------------------------------------------------------

std::vector<TouchZone> DefaultZones() {
  return std::vector<TouchZone>(kDefaultTouchZones,
                                kDefaultTouchZones + kDefaultTouchZoneCount);
}

void LoadZones() {
  g_zones = DefaultZones();
  NSArray* saved = [NSUserDefaults.standardUserDefaults arrayForKey:kLayoutKey];
  if (!saved) return;
  if (saved.count != (NSUInteger)(kDefaultTouchZoneCount * 4)) return;
  for (int i = 0; i < kDefaultTouchZoneCount; ++i) {
    float v[4];
    for (int c = 0; c < 4; ++c) {
      NSNumber* n = saved[(NSUInteger)(i * 4 + c)];
      if (![n isKindOfClass:[NSNumber class]]) return;
      v[c] = n.floatValue;
    }
    g_zones[(size_t)i] = {v[0], v[1], v[2], v[3], kDefaultTouchZones[i].button,
                          kDefaultTouchZones[i].label};
  }
}

void PersistZones() {
  NSMutableArray* arr = [NSMutableArray arrayWithCapacity:g_zones.size() * 4];
  for (const auto& z : g_zones) {
    [arr addObject:@(z.x0)];
    [arr addObject:@(z.y0)];
    [arr addObject:@(z.x1)];
    [arr addObject:@(z.y1)];
  }
  [NSUserDefaults.standardUserDefaults setObject:arr forKey:kLayoutKey];
}

// ---------------------------------------------------------------------------
// Scaling of the rendered game view (base / stretch / manual)
// ---------------------------------------------------------------------------

enum ScaleMode { kScaleBase = 0, kScaleStretch = 1, kScaleManual = 2 };

ScaleMode g_scale_mode = kScaleBase;
float g_manual_sx = 1.0f, g_manual_sy = 1.0f;

UIView* FindMetalView(UIView* root) {
  if ([root.layer isKindOfClass:[CAMetalLayer class]]) return root;
  for (UIView* sub in root.subviews) {
    UIView* found = FindMetalView(sub);
    if (found) return found;
  }
  return nil;
}

void ApplyScale() {
  UIWindow* window = g_root.window ?: UIApplication.sharedApplication.keyWindow;
  if (!window) return;
  UIView* game = FindMetalView(window);
  if (!game) return;
  const CGSize win = window.bounds.size;
  const CGSize cur = game.bounds.size;
  if (cur.width < 1 || cur.height < 1 || win.width < 1 || win.height < 1) return;
  CGFloat sx = 1.0, sy = 1.0;
  switch (g_scale_mode) {
    case kScaleBase:
      break;
    case kScaleStretch:
      sx = win.width / cur.width;
      sy = win.height / cur.height;
      break;
    case kScaleManual:
      sx = g_manual_sx;
      sy = g_manual_sy;
      break;
  }
  game.transform = CGAffineTransformMakeScale(sx, sy);
}

void LoadScaleSettings() {
  NSDictionary* d = [NSUserDefaults.standardUserDefaults dictionaryForKey:kScaleKey];
  if (!d) return;
  g_scale_mode = (ScaleMode)[d[@"mode"] intValue];
  g_manual_sx = [d[@"sx"] floatValue];
  g_manual_sy = [d[@"sy"] floatValue];
  if (g_manual_sx < 0.5f) g_manual_sx = 1.0f;
  if (g_manual_sy < 0.5f) g_manual_sy = 1.0f;
}

void SaveScaleSettings() {
  [NSUserDefaults.standardUserDefaults setObject:@{
    @"mode" : @(g_scale_mode),
    @"sx" : @(g_manual_sx),
    @"sy" : @(g_manual_sy),
  }
                                          forKey:kScaleKey];
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void StyleZoneView(UIView* v, const char* label) {
  v.layer.borderColor = [UIColor.whiteColor colorWithAlphaComponent:0.45].CGColor;
  v.layer.borderWidth = 1.2f;
  v.layer.cornerRadius = 8;
  v.backgroundColor = [UIColor.whiteColor colorWithAlphaComponent:0.06];
  UILabel* text = [[UILabel alloc] initWithFrame:v.bounds];
  text.text = [NSString stringWithUTF8String:label];
  text.textColor = [UIColor.whiteColor colorWithAlphaComponent:0.65];
  text.font = [UIFont boldSystemFontOfSize:11];
  text.textAlignment = NSTextAlignmentCenter;
  text.autoresizingMask =
      UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  text.userInteractionEnabled = NO;
  [v addSubview:text];
}

void AddStickRing(CGFloat cx, CGFloat cy, CGFloat diameter) {
  UIView* ring = [[UIView alloc] initWithFrame:CGRectZero];
  ring.layer.borderColor = [UIColor.systemBlueColor colorWithAlphaComponent:0.55].CGColor;
  ring.layer.borderWidth = 2;
  ring.layer.cornerRadius = diameter / 2;
  ring.backgroundColor = [UIColor.systemBlueColor colorWithAlphaComponent:0.08];
  ring.tag = 0x53544B31;  // "STK1"
  ring.userInteractionEnabled = NO;
  [g_root addSubview:ring];
  // Position relative to root; re-fit on layout.
  ring.frame = CGRectMake(g_root.bounds.size.width * cx - diameter / 2,
                          g_root.bounds.size.height * cy - diameter / 2,
                          diameter, diameter);
}

void LayoutZoneViews() {
  if (!g_root) return;
  const CGSize s = g_root.bounds.size;
  for (size_t i = 0; i < g_zone_views.size() && i < g_zones.size(); ++i) {
    const TouchZone& z = g_zones[i];
    UIView* v = g_zone_views[i];
    v.frame = CGRectMake(s.width * z.x0, s.height * z.y0,
                         s.width * (z.x1 - z.x0), s.height * (z.y1 - z.y0));
  }
  const CGFloat ring_d = s.width * 0.20f;
  for (UIView* sub in g_root.subviews) {
    if (sub.tag != 0x53544B31) continue;
    if (sub.frame.origin.x < s.width * 0.5f) {
      sub.frame = CGRectMake(s.width * 0.06f, s.height * 0.62f, ring_d, ring_d);
    } else {
      sub.frame = CGRectMake(s.width * 0.94f - ring_d, s.height * 0.62f, ring_d, ring_d);
    }
  }
}

// ---------------------------------------------------------------------------
// Drag handling (edit mode)
// ---------------------------------------------------------------------------

void HandleZonePan(UIPanGestureRecognizer* pan) {
  if (!g_edit_mode) return;
  const int idx = (int)pan.view.tag;
  if (idx < 0 || idx >= (int)g_zones.size()) return;
  UIView* v = pan.view;
  CGPoint t = [pan translationInView:g_root];
  [pan setTranslation:CGPointZero inView:g_root];
  const CGSize s = g_root.bounds.size;
  if (s.width < 1 || s.height < 1) return;
  const float dx = (float)(t.x / s.width);
  const float dy = (float)(t.y / s.height);
  TouchZone& z = g_zones[(size_t)idx];
  SetTouchZoneRect(idx, z.x0 + dx, z.y0 + dy, z.x1 + dx, z.y1 + dy);
  v.frame = CGRectMake(s.width * z.x0, s.height * z.y0,
                       s.width * (z.x1 - z.x0), s.height * (z.y1 - z.y0));
  if (pan.state == UIGestureRecognizerStateEnded) PersistZones();
}

// Block-based target/action bridge (UIButton + gesture recognizers).
@interface DS2BlockAction : NSObject
@property(nonatomic, copy) void (^block)(id);
- (void)invoke:(id)sender;
@end
@implementation DS2BlockAction
- (void)invoke:(id)sender {
  if (self.block) self.block(sender);
}
@end

DS2BlockAction* BlockAction(void (^blk)(id)) {
  DS2BlockAction* a = [[DS2BlockAction alloc] init];
  a.block = blk;
  return a;
}

// Keep a proxy alive for as long as its owner view lives.
void RetainOnView(id owner, id object) {
  objc_setAssociatedObject(owner, (void*)"ds2_block_action", object,
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

// ---------------------------------------------------------------------------
// Settings panel
// ---------------------------------------------------------------------------

UIButton* PanelButton(NSString* title, UIView* parent, CGFloat y) {
  UIButton* b = [UIButton buttonWithType:UIButtonTypeSystem];
  b.frame = CGRectMake(10, y, 208, 30);
  [b setTitle:title forState:UIControlStateNormal];
  [b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
  b.titleLabel.font = [UIFont systemFontOfSize:13];
  b.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
  b.backgroundColor = [UIColor.whiteColor colorWithAlphaComponent:0.10];
  b.layer.cornerRadius = 6;
  b.contentEdgeInsets = UIEdgeInsetsMake(0, 8, 0, 8);
  [parent addSubview:b];
  return b;
}

UIButton* g_edit_button = nil;
UIButton* g_scale_button = nil;

const char* ScaleModeName() {
  switch (g_scale_mode) {
    case kScaleStretch: return "Stretch fullscreen";
    case kScaleManual: return "Manual";
    default: return "Base";
  }
}

void UpdatePanelLabels() {
  if (g_edit_button) {
    [g_edit_button setTitle:(g_edit_mode ? @"Done moving buttons" : @"Move buttons")
                   forState:UIControlStateNormal];
  }
  if (g_scale_button) {
    [g_scale_button
        setTitle:[NSString stringWithFormat:@"Scale: %s (%.2fx%.2f)", ScaleModeName(),
                                           g_manual_sx, g_manual_sy]
        forState:UIControlStateNormal];
  }
}

void ToggleEdit() {
  g_edit_mode = !g_edit_mode;
  for (UIView* v : g_zone_views) {
    v.userInteractionEnabled = g_edit_mode;
    v.backgroundColor = g_edit_mode
                            ? [UIColor.systemYellowColor colorWithAlphaComponent:0.15]
                            : [UIColor.whiteColor colorWithAlphaComponent:0.06];
    for (UIGestureRecognizer* gr in v.gestureRecognizers) gr.enabled = g_edit_mode;
  }
  UpdatePanelLabels();
}

void SetScaleMode(ScaleMode mode) {
  g_scale_mode = mode;
  SaveScaleSettings();
  ApplyScale();
  UpdatePanelLabels();
}

void CycleScale() {
  SetScaleMode((ScaleMode)(((int)g_scale_mode + 1) % 3));
}

}  // namespace

const TouchZone* GetTouchZones() {
  static dispatch_once_t once;
  dispatch_once(&once, ^{ LoadZones(); });
  return g_zones.data();
}

int GetTouchZoneCount() {
  return kDefaultTouchZoneCount;
}

void SetTouchZoneRect(int index, float x0, float y0, float x1, float y1) {
  if (index < 0 || index >= (int)g_zones.size()) return;
  // keep within the screen and non-degenerate
  x0 = std::clamp(x0, 0.0f, 0.99f);
  y0 = std::clamp(y0, 0.0f, 0.99f);
  x1 = std::clamp(x1, x0 + 0.02f, 1.0f);
  y1 = std::clamp(y1, y0 + 0.02f, 1.0f);
  g_zones[(size_t)index] = {x0, y0, x1, y1, g_zones[(size_t)index].button,
                            g_zones[(size_t)index].label};
}

void ResetTouchZones() {
  g_zones = DefaultZones();
  PersistZones();
}

void RelayoutZoneViews() { LayoutZoneViews(); }

// Root view that re-fits zone frames on rotation/resize.
@interface DS2ControlsRootView : UIView
@end
@implementation DS2ControlsRootView
- (void)layoutSubviews {
  [super layoutSubviews];
  ds2_ios::RelayoutZoneViews();
}
@end

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void InstallControlsOverlay() {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    LoadZones();
    LoadScaleSettings();
  });

  if (g_root) return;  // already installed

  UIWindow* window = nil;
  if (@available(iOS 15.0, *)) {
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
      if ([scene isKindOfClass:[UIWindowScene class]]) {
        for (UIWindow* w in ((UIWindowScene*)scene).windows) {
          if (w.isKeyWindow) window = w;
        }
      }
    }
  }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  if (!window) window = UIApplication.sharedApplication.keyWindow;
#pragma clang diagnostic pop
  if (!window) return;

  DS2ControlsRootView* root =
      [[DS2ControlsRootView alloc] initWithFrame:window.bounds];
  root.tag = 0x44533243;  // "DS2C"
  root.autoresizingMask =
      UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  root.userInteractionEnabled = NO;  // play mode: touches fall through
  root.backgroundColor = UIColor.clearColor;
  g_root = root;
  [window addSubview:root];

  // Stick rings (visual only; stick capture is the lower left/right quadrant).
  AddStickRing(0.16f, 0.72f, window.bounds.size.width * 0.20f);
  AddStickRing(0.84f, 0.72f, window.bounds.size.width * 0.20f);

  // Zone views + drag recognizers.
  for (int i = 0; i < GetTouchZoneCount(); ++i) {
    UIView* v = [[UIView alloc] initWithFrame:CGRectZero];
    v.tag = i;
    v.userInteractionEnabled = NO;  // enabled only in edit mode
    StyleZoneView(v, GetTouchZones()[i].label);
    [root addSubview:v];
    g_zone_views.push_back(v);

    DS2BlockAction* pan_action = BlockAction(^(id sender) {
      HandleZonePan((UIPanGestureRecognizer*)sender);
    });
    UIPanGestureRecognizer* pan =
        [[UIPanGestureRecognizer alloc] initWithTarget:pan_action
                                                action:@selector(invoke:)];
    pan.enabled = NO;
    RetainOnView(v, pan_action);
    [v addGestureRecognizer:pan];
  }
  LayoutZoneViews();

  // Single settings button ("SET").
  g_gear = [UIButton buttonWithType:UIButtonTypeSystem];
  g_gear.frame = CGRectMake(window.bounds.size.width * 0.62f, 4, 56, 22);
  g_gear.autoresizingMask =
      UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin;
  [g_gear setTitle:@"SET" forState:UIControlStateNormal];
  [g_gear setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
  g_gear.titleLabel.font = [UIFont boldSystemFontOfSize:11];
  g_gear.backgroundColor = [UIColor.blackColor colorWithAlphaComponent:0.45];
  g_gear.layer.cornerRadius = 5;
  g_gear.alpha = 0.7;
  g_gear.layer.zPosition = 9999;
  [window addSubview:g_gear];
  [window bringSubviewToFront:g_gear];

  DS2BlockAction* gear_action = BlockAction(^(id) {
    g_panel.hidden = !g_panel.hidden;
    if (!g_panel.hidden) UpdatePanelLabels();
  });
  RetainOnView(g_gear, gear_action);
  [g_gear addTarget:gear_action action:@selector(invoke:)
   forControlEvents:UIControlEventTouchUpInside];

  // Settings panel (hidden by default).
  const CGFloat panel_w = 228, panel_h = 240;
  g_panel = [[UIView alloc]
      initWithFrame:CGRectMake(window.bounds.size.width - panel_w - 8, 30, panel_w,
                               panel_h)];
  g_panel.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin;
  g_panel.backgroundColor = [UIColor.blackColor colorWithAlphaComponent:0.85];
  g_panel.layer.cornerRadius = 10;
  g_panel.hidden = YES;
  g_panel.userInteractionEnabled = YES;
  g_panel.layer.zPosition = 9998;
  [window addSubview:g_panel];

  UIButton* close = PanelButton(@"Close", g_panel, 8);
  DS2BlockAction* close_action = BlockAction(^(id) { g_panel.hidden = YES; });
  RetainOnView(close, close_action);
  [close addTarget:close_action action:@selector(invoke:)
  forControlEvents:UIControlEventTouchUpInside];

  g_edit_button = PanelButton(@"Move buttons", g_panel, 44);
  DS2BlockAction* edit_action = BlockAction(^(id) { ToggleEdit(); });
  RetainOnView(g_edit_button, edit_action);
  [g_edit_button addTarget:edit_action action:@selector(invoke:)
  forControlEvents:UIControlEventTouchUpInside];

  UIButton* reset = PanelButton(@"Reset layout", g_panel, 80);
  DS2BlockAction* reset_action = BlockAction(^(id) {
    ResetTouchZones();
    LayoutZoneViews();
  });
  RetainOnView(reset, reset_action);
  [reset addTarget:reset_action action:@selector(invoke:)
  forControlEvents:UIControlEventTouchUpInside];

  UIButton* logs = PanelButton(@"Show logs", g_panel, 116);
  DS2BlockAction* logs_action = BlockAction(^(id) {
    g_panel.hidden = YES;
    ToggleLogOverlay();
  });
  RetainOnView(logs, logs_action);
  [logs addTarget:logs_action action:@selector(invoke:)
  forControlEvents:UIControlEventTouchUpInside];

  g_scale_button = PanelButton(@"Scale: Base", g_panel, 152);
  DS2BlockAction* scale_action = BlockAction(^(id) { CycleScale(); });
  RetainOnView(g_scale_button, scale_action);
  [g_scale_button addTarget:scale_action action:@selector(invoke:)
  forControlEvents:UIControlEventTouchUpInside];

  UIButton* manual = PanelButton(@"Manual: zoom +5%", g_panel, 188);
  DS2BlockAction* manual_action = BlockAction(^(id) {
    g_manual_sx += 0.05f;
    g_manual_sy += 0.05f;
    if (g_manual_sx > 1.6f) { g_manual_sx = 1.0f; g_manual_sy = 1.0f; }
    SetScaleMode(kScaleManual);
  });
  RetainOnView(manual, manual_action);
  [manual addTarget:manual_action action:@selector(invoke:)
  forControlEvents:UIControlEventTouchUpInside];

  UpdatePanelLabels();

  // The Metal view may not exist yet at install time; retry.
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{ ApplyScale(); });
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(4.0 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{ ApplyScale(); });

  DebugMark("controls: overlay installed");
}

void SetGameDisplayScale(float sx, float sy) {
  g_scale_mode = kScaleManual;
  g_manual_sx = sx;
  g_manual_sy = sy;
  SaveScaleSettings();
  ApplyScale();
}

}  // namespace ds2_ios
