// macOS window: the same layout as app_win.cpp, with AppKit pickers.
#import <Cocoa/Cocoa.h>

#include "app.h"

static const CGFloat kW = 440, kTop = 134, kRow = 33, kH = kTop + 6 + 4 * kRow;
static CGFloat rowY(int i) { return kTop + 6 + i * kRow; }

@interface RXView : NSView {
 @public
  rx::App *_app;
  NSPopUpButton *_source, *_rate, *_ltc, *_chan, *_mtc;
  NSTextField *_lat;
  NSStepper *_step;
  NSFont *_fTc, *_fUi;
  std::vector<rx::Source> _sources;
  std::vector<rx::Device> _audio, _midi;
  NSTimer *_timer;
}
@end

@implementation RXView

- (instancetype)initWithApp:(rx::App *)app {
  self = [super initWithFrame:NSMakeRect(0, 0, kW, kH)];
  if (!self) return nil;
  _app = app;
  _fTc = [NSFont monospacedSystemFontOfSize:58 weight:NSFontWeightSemibold] ?: [NSFont systemFontOfSize:58];
  _fUi = [NSFont systemFontOfSize:13];
  _source = [self popupAt:NSMakeRect(84, rowY(0), 344, 24) action:@selector(sourcePicked:)];
  _rate = [self popupAt:NSMakeRect(84, rowY(1), 92, 24) action:@selector(ratePicked:)];
  _ltc = [self popupAt:NSMakeRect(84, rowY(2), 256, 24) action:@selector(ltcPicked:)];
  _chan = [self popupAt:NSMakeRect(348, rowY(2), 80, 24) action:@selector(chanPicked:)];
  _mtc = [self popupAt:NSMakeRect(84, rowY(3), 344, 24) action:@selector(mtcPicked:)];
  for (int i = 0; i < 5; i++) [_rate addItemWithTitle:@(R::rateNames[i])];
  [_rate selectItemAtIndex:app->set.rate.load()];

  _lat = [[NSTextField alloc] initWithFrame:NSMakeRect(262, rowY(1) + 1, 74, 22)];
  _lat.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
  _lat.alignment = NSTextAlignmentRight;
  _lat.bezelStyle = NSTextFieldRoundedBezel;
  _lat.focusRingType = NSFocusRingTypeNone;
  _lat.target = self;
  _lat.action = @selector(latencyEntered:);
  [self addSubview:_lat];
  _step = [[NSStepper alloc] initWithFrame:NSMakeRect(338, rowY(1) - 1, 19, 27)];
  _step.minValue = -1; _step.maxValue = 1; _step.increment = 1; _step.valueWraps = NO; _step.autorepeat = YES;
  _step.target = self;
  _step.action = @selector(latencyStepped:);
  [self addSubview:_step];
  [self showLatency];
  [self fillAll];

  _timer = [NSTimer timerWithTimeInterval:1.0 / 30.0 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
  [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
  return self;
}

- (NSPopUpButton *)popupAt:(NSRect)r action:(SEL)action {
  NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:r pullsDown:NO];
  p.controlSize = NSControlSizeSmall;
  p.font = [NSFont systemFontOfSize:11];
  p.target = self;
  p.action = action;
  [self addSubview:p];
  [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(willPop:) name:NSPopUpButtonWillPopUpNotification object:p];
  return p;
}

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (void)willPop:(NSNotification *)n { [self fillAll]; }

// lists are read again each time a picker opens
- (void)fillAll {
  if (!_app) return;
  _sources = _app->listener.sources();
  const std::string src = _app->listener.choice();
  [_source removeAllItems];
  [_source addItemWithTitle:@(R::automatic)];
  NSInteger sel = src.empty() ? 0 : -1;
  for (size_t i = 0; i < _sources.size(); i++) {
    [_source addItemWithTitle:@(rx::App::sourceLabel(_sources[i].ip).c_str())];
    if (_sources[i].ip == src) sel = (NSInteger)i + 1;
  }
  if (sel < 0) { [_source addItemWithTitle:@(src.c_str())]; sel = _source.numberOfItems - 1; }  // chosen, not sending now
  [_source selectItemAtIndex:sel];

  _audio = rx::audio_outputs();
  [self fill:_ltc from:_audio current:_app->ltcDev];
  _midi = rx::midi_outputs();
  [self fill:_mtc from:_midi current:_app->mtcPort];

  [_chan removeAllItems];
  [_chan addItemWithTitle:@(R::allChannels)];
  for (int c = 1; c <= _app->audio.channels(); c++) [_chan addItemWithTitle:@(R::channel(c).c_str())];
  const int ch = _app->ltcCh.load();
  [_chan selectItemAtIndex:ch < _chan.numberOfItems ? ch : 0];
}

- (void)fill:(NSPopUpButton *)p from:(const std::vector<rx::Device> &)list current:(const std::string &)cur {
  [p removeAllItems];
  [p addItemWithTitle:@(R::off)];
  NSInteger sel = 0;
  for (size_t i = 0; i < list.size(); i++) {
    [p addItemWithTitle:@""];                       // titles may repeat: set them after adding
    p.lastItem.title = @(list[i].name.c_str());
    if (list[i].id == cur) sel = (NSInteger)i + 1;
  }
  [p selectItemAtIndex:sel];
}

- (void)sourcePicked:(id)s {
  const NSInteger k = _source.indexOfSelectedItem;
  if (k == 0) _app->setSource("");
  else if (k > 0 && (size_t)k <= _sources.size()) _app->setSource(_sources[k - 1].ip);
}
- (void)ratePicked:(id)s { _app->setRate((int)_rate.indexOfSelectedItem); }
- (void)ltcPicked:(id)s {
  const NSInteger k = _ltc.indexOfSelectedItem;
  _app->setLtc(k > 0 && (size_t)k <= _audio.size() ? _audio[k - 1].id : "");
  [self fillAll];
}
- (void)chanPicked:(id)s { _app->setLtcChannel((int)_chan.indexOfSelectedItem); }
- (void)mtcPicked:(id)s {
  const NSInteger k = _mtc.indexOfSelectedItem;
  _app->setMtc(k > 0 && (size_t)k <= _midi.size() ? _midi[k - 1].id : "");
}

- (void)showLatency { _lat.stringValue = [NSString stringWithFormat:@"%.1f", _app->set.latencyMs.load()]; }
- (void)latencyEntered:(id)s {
  NSString *t = [_lat.stringValue stringByReplacingOccurrencesOfString:@"," withString:@"."];
  _app->setLatency(t.doubleValue);
  [self showLatency];
  [self.window makeFirstResponder:self];
}
- (void)latencyStepped:(id)s {
  const int dir = _step.doubleValue > 0 ? 1 : -1;
  _step.doubleValue = 0;
  _app->setLatency(_app->set.latencyMs.load() + dir * ((NSEvent.modifierFlags & NSEventModifierFlagShift) ? 10.0 : 1.0));
  [self showLatency];
}

- (void)tick:(NSTimer *)t { [self setNeedsDisplay:YES]; }

- (void)text:(const char *)s font:(NSFont *)font color:(NSColor *)c at:(NSPoint)pt centered:(BOOL)centered {
  NSString *str = s ? [NSString stringWithUTF8String:s] : nil;
  if (!str.length || !font || !c) return;
  NSDictionary *a = @{NSFontAttributeName : font, NSForegroundColorAttributeName : c};
  if (centered) pt.x = floor((kW - [str sizeWithAttributes:a].width) / 2);
  [str drawAtPoint:pt withAttributes:a];
}

- (void)drawRect:(NSRect)dirty {
  @try {
    [[NSColor colorWithSRGBRed:0.055 green:0.062 blue:0.075 alpha:1] setFill];
    NSRectFill(self.bounds);
    if (!_app) return;
    const rx::App::Ui u = _app->ui();
    NSColor *dim = [NSColor colorWithSRGBRed:0.60 green:0.65 blue:0.71 alpha:1];
    NSColor *tc = u.running ? [NSColor colorWithSRGBRed:0.36 green:1.0 blue:0.58 alpha:1] : [NSColor colorWithSRGBRed:0.42 green:0.46 blue:0.51 alpha:1];
    [self text:u.tc font:_fTc color:tc at:NSMakePoint(0, 18) centered:YES];
    [self text:u.line.c_str() font:_fUi color:u.fault ? [NSColor colorWithSRGBRed:1.0 green:0.78 blue:0.30 alpha:1] : dim at:NSMakePoint(0, 96) centered:YES];
    [[NSColor colorWithSRGBRed:0.10 green:0.11 blue:0.13 alpha:1] setFill];
    NSRectFill(NSMakeRect(0, kTop, kW, kH - kTop));
    const char *labels[4] = {R::source, R::rate, R::ltc, R::mtc};
    for (int i = 0; i < 4; i++) [self text:labels[i] font:_fUi color:dim at:NSMakePoint(12, rowY(i) + 4) centered:NO];
    [self text:R::latency font:_fUi color:dim at:NSMakePoint(200, rowY(1) + 4) centered:NO];
    [self text:R::ms font:_fUi color:dim at:NSMakePoint(362, rowY(1) + 4) centered:NO];
  } @catch (NSException *e) {
    NSLog(@"receiver: draw failed: %@", e);
  }
}

@end

@interface RXDelegate : NSObject <NSApplicationDelegate> {
  rx::App _app;
  NSWindow *_window;
}
@end

@implementation RXDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)n {
  _app.start();
  _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kW, kH)
                                         styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
  _window.title = @(R::appName);
  _window.contentView = [[RXView alloc] initWithApp:&_app];
  [_window center];
  [_window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { return YES; }
- (void)applicationWillTerminate:(NSNotification *)n { _app.shutdown(); }
@end

int main() {
  @autoreleasepool {
    NSApplication *app = [NSApplication sharedApplication];
    app.activationPolicy = NSApplicationActivationPolicyRegular;
    NSMenu *bar = [NSMenu new], *menu = [NSMenu new];
    NSMenuItem *item = [NSMenuItem new];
    [bar addItem:item];
    [menu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
    item.submenu = menu;
    app.mainMenu = bar;
    static RXDelegate *delegate;
    delegate = [RXDelegate new];
    app.delegate = delegate;
    [app run];
  }
  return 0;
}
