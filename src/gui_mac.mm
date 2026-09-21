#import <Cocoa/Cocoa.h>

#include "plugin.h"

static const CGFloat kW = kGuiW, kH = kGuiH, kBar = kGuiBar;

@interface CTLTCView : NSView <NSTextFieldDelegate> {
  Plugin *_plug;
  NSTextField *_ipField;
  NSTextField *_offField;
  NSStepper *_offStepper;
  double _offShown;
  NSSegmentedControl *_srcSeg;
  NSSegmentedControl *_muteSeg;
  int _tickCount;
  NSTimer *_timer;
}
- (instancetype)initWithPlugin:(Plugin *)plug;
- (void)shutdown;
@end

@implementation CTLTCView

- (instancetype)initWithPlugin:(Plugin *)plug {
  self = [super initWithFrame:NSMakeRect(0, 0, kW, kH)];
  if (!self) return nil;
  _plug = plug;

  _ipField = [[NSTextField alloc] initWithFrame:NSMakeRect(86, kH - kBar + 7, 150, 22)];
  _ipField.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
  _ipField.stringValue = [NSString stringWithUTF8String:plug->ip.c_str()];
  _ipField.bezelStyle = NSTextFieldRoundedBezel;
  _ipField.focusRingType = NSFocusRingTypeNone;
  _ipField.target = self;
  _ipField.action = @selector(ipEntered:);
  _ipField.delegate = self;
  [self addSubview:_ipField];

  // offset row: value in ms (type it, or step 1 ms; hold Shift while clicking the stepper = 1 frame)
  _offShown = plug->sender.offsetMs();
  _offField = [[NSTextField alloc] initWithFrame:NSMakeRect(86, kH - kBar + 40, 70, 22)];
  _offField.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
  _offField.alignment = NSTextAlignmentRight;
  _offField.bezelStyle = NSTextFieldRoundedBezel;
  _offField.focusRingType = NSFocusRingTypeNone;
  _offField.stringValue = [NSString stringWithFormat:@"%.1f", _offShown];
  _offField.target = self;
  _offField.action = @selector(offsetEntered:);
  [self addSubview:_offField];
  _offStepper = [[NSStepper alloc] initWithFrame:NSMakeRect(158, kH - kBar + 38, 19, 27)];
  _offStepper.minValue = ctltc::kOffsetMinMs;
  _offStepper.maxValue = ctltc::kOffsetMaxMs;
  _offStepper.increment = 1.0;
  _offStepper.valueWraps = NO;
  _offStepper.autorepeat = YES;
  _offStepper.doubleValue = _offShown;
  _offStepper.target = self;
  _offStepper.action = @selector(offsetStepped:);
  [self addSubview:_offStepper];

  // source row
  _srcSeg = [NSSegmentedControl segmentedControlWithLabels:@[ @(S::sourceNames[0]), @(S::sourceNames[1]), @(S::sourceNames[2]) ]
                                              trackingMode:NSSegmentSwitchTrackingSelectOne
                                                    target:self
                                                    action:@selector(sourceChanged:)];
  _srcSeg.frame = NSMakeRect(84, kH - kBar + 72, 220, 24);
  _srcSeg.controlSize = NSControlSizeSmall;
  _srcSeg.font = [NSFont systemFontOfSize:11];
  _srcSeg.selectedSegment = plug->mode.load();
  [self addSubview:_srcSeg];

  _muteSeg = [NSSegmentedControl segmentedControlWithLabels:@[ @(S::muteLtc) ]
                                               trackingMode:NSSegmentSwitchTrackingSelectAny
                                                     target:self
                                                     action:@selector(muteChanged:)];
  _muteSeg.frame = NSMakeRect(kW - 12 - 92, kH - kBar + 72, 92, 24);
  _muteSeg.controlSize = NSControlSizeSmall;
  _muteSeg.font = [NSFont systemFontOfSize:11];
  [_muteSeg setSelected:plug->muteLtc.load() != 0 forSegment:0];
  [self addSubview:_muteSeg];

  _timer = [NSTimer timerWithTimeInterval:1.0 / 30.0 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
  [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
  return self;
}

- (void)shutdown {
  [_timer invalidate];
  _timer = nil;
  _plug = nullptr;
  [self removeFromSuperview];
}

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return YES; }

- (void)sourceChanged:(id)sender {
  if (_plug) _plug->setModeFromGui((int)_srcSeg.selectedSegment);
}

- (void)muteChanged:(id)sender {
  if (_plug) _plug->setMuteFromGui([_muteSeg isSelectedForSegment:0]);
}

- (void)tick:(NSTimer *)t {
  if (_plug) {  // follow changes made from the host (parameter slider, automation)
    if (_srcSeg.selectedSegment != _plug->mode.load()) _srcSeg.selectedSegment = _plug->mode.load();
    if ([_muteSeg isSelectedForSegment:0] != (_plug->muteLtc.load() != 0)) [_muteSeg setSelected:_plug->muteLtc.load() != 0 forSegment:0];
    if ((_tickCount++ % 30) == 0) _plug->refreshProject();  // project frame rate / start offset, once a second
    const double v = _plug->sender.offsetMs();
    if (v != _offShown && _offField.currentEditor == nil) [self showOffset:v];
  }
  [self setNeedsDisplay:YES];
}

- (void)showOffset:(double)ms {
  _offShown = ms;
  _offField.stringValue = [NSString stringWithFormat:@"%.1f", ms];
  _offStepper.doubleValue = ms;
}

- (void)offsetEntered:(id)sender {
  if (!_plug) return;
  NSString *s = [_offField.stringValue stringByReplacingOccurrencesOfString:@"," withString:@"."];
  _plug->setOffsetFromGui(ctltc::clamp_offset(s.doubleValue));
  [self showOffset:_plug->sender.offsetMs()];
  [self.window makeFirstResponder:self];
}

- (void)offsetStepped:(id)sender {
  if (!_plug) return;
  double v = _offStepper.doubleValue;
  if (NSEvent.modifierFlags & NSEventModifierFlagShift) {  // Shift = one frame per click
    const double step = _plug->frameDur.load() * 1e3, dir = v > _offShown ? 1.0 : -1.0;
    v = _offShown + dir * step;
  }
  _plug->setOffsetFromGui(ctltc::clamp_offset(v));
  [self showOffset:_plug->sender.offsetMs()];
}

- (void)ipEntered:(id)sender {
  if (!_plug) return;
  NSString *s = [_ipField.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
  if (_plug->setIP(std::string(s.UTF8String ? s.UTF8String : ""))) {
    _ipField.stringValue = s;
    _ipField.textColor = [NSColor controlTextColor];
    [self.window makeFirstResponder:self];
  } else {
    _ipField.textColor = [NSColor systemRedColor];
  }
}

- (void)controlTextDidChange:(NSNotification *)n { _ipField.textColor = [NSColor controlTextColor]; }

- (BOOL)acceptsFirstResponder { return YES; }

- (void)drawCentered:(NSString *)s font:(NSFont *)font color:(NSColor *)c y:(CGFloat)y {
  NSDictionary *a = @{NSFontAttributeName : font, NSForegroundColorAttributeName : c};
  NSSize sz = [s sizeWithAttributes:a];
  [s drawAtPoint:NSMakePoint(floor((kW - sz.width) / 2), y) withAttributes:a];
}

- (void)drawRect:(NSRect)dirty {
  [[NSColor colorWithSRGBRed:0.055 green:0.062 blue:0.075 alpha:1] setFill];
  NSRectFill(self.bounds);
  if (!_plug) return;
  const UiState u = ui_state(*_plug);
  NSColor *dim = [NSColor colorWithSRGBRed:0.60 green:0.65 blue:0.71 alpha:1];
  auto color = [&](UiColor c) -> NSColor * {
    switch (c) {
      case kUiLtc: return [NSColor colorWithSRGBRed:0.36 green:1.0 blue:0.58 alpha:1];
      case kUiDaw: return [NSColor colorWithSRGBRed:0.45 green:0.78 blue:1.0 alpha:1];
      case kUiCoast: case kUiWarn: return [NSColor colorWithSRGBRed:1.0 green:0.78 blue:0.30 alpha:1];
      case kUiOff: return [NSColor colorWithSRGBRed:0.42 green:0.46 blue:0.51 alpha:1];  // 3.6:1 on the ground
      default: return dim;
    }
  };
  [self drawCentered:@(u.tc) font:[NSFont monospacedSystemFontOfSize:58 weight:NSFontWeightSemibold] color:color(u.tcColor) y:18];
  [self drawCentered:@(u.line.c_str()) font:[NSFont systemFontOfSize:13] color:color(u.lineColor) y:96];

  [[NSColor colorWithSRGBRed:0.10 green:0.11 blue:0.13 alpha:1] setFill];
  NSRectFill(NSMakeRect(0, kH - kBar, kW, kBar));
  NSDictionary *la = @{NSFontAttributeName : [NSFont systemFontOfSize:13], NSForegroundColorAttributeName : dim};
  [@(S::artnetTo) drawAtPoint:NSMakePoint(12, kH - kBar + 10) withAttributes:la];
  [@(S::offset) drawAtPoint:NSMakePoint(12, kH - kBar + 43) withAttributes:la];
  [@(S::source) drawAtPoint:NSMakePoint(12, kH - kBar + 77) withAttributes:la];
  [@(S::ms) drawAtPoint:NSMakePoint(184, kH - kBar + 43) withAttributes:la];
  if (!u.offsetNote.empty()) [@(u.offsetNote.c_str()) drawAtPoint:NSMakePoint(216, kH - kBar + 43) withAttributes:la];

  NSDictionary *sa = @{NSFontAttributeName : [NSFont systemFontOfSize:13], NSForegroundColorAttributeName : color(u.sendColor)};
  NSString *st = @(u.send.c_str());
  [st drawAtPoint:NSMakePoint(kW - [st sizeWithAttributes:sa].width - 12, kH - kBar + 10) withAttributes:sa];
}

@end


const char *gui_api() { return CLAP_WINDOW_API_COCOA; }
bool gui_make(Plugin *s) {
  if (!s->view) s->view = (__bridge_retained void *)[[CTLTCView alloc] initWithPlugin:s];
  return s->view != nullptr;
}
void gui_free(Plugin *s) {
  if (!s->view) return;
  CTLTCView *v = (__bridge_transfer CTLTCView *)s->view;
  s->view = nullptr;
  [v shutdown];
}
bool gui_parent(Plugin *s, const clap_window_t *win) {
  if (!s->view || !win || !win->cocoa) return false;
  CTLTCView *v = (__bridge CTLTCView *)s->view;
  [v setFrame:NSMakeRect(0, 0, kW, kH)];
  [(__bridge NSView *)win->cocoa addSubview:v];
  return true;
}
void gui_visible(Plugin *s, bool on) {
  if (s->view) [(__bridge CTLTCView *)s->view setHidden:!on];
}
