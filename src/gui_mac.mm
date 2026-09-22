#import <Cocoa/Cocoa.h>

#include "plugin.h"

static const CGFloat kW = kGuiW, kH = kGuiH, kBar = kGuiBar;

@interface CTLTCView : NSView <NSTextFieldDelegate> {
  Plugin *_plug;
  NSTextField *_ipField;
  NSTextField *_latField;
  NSStepper *_latStepper;
  NSSegmentedControl *_unitSeg;
  double _latShown;
  NSSegmentedControl *_srcSeg;
  NSSegmentedControl *_muteSeg;
  NSSegmentedControl *_exclSeg;
  NSFont *_fTc, *_fUi;      // looked up once: a lookup that fails mid-session must not reach the draw path
  NSArray<NSColor *> *_colors;  // indexed by UiColor
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
  _fTc = [NSFont monospacedSystemFontOfSize:58 weight:NSFontWeightSemibold] ?: [NSFont userFixedPitchFontOfSize:58] ?: [NSFont systemFontOfSize:58];
  _fUi = [NSFont systemFontOfSize:13];
  NSColor *amber = [NSColor colorWithSRGBRed:1.0 green:0.78 blue:0.30 alpha:1];
  _colors = @[
    [NSColor colorWithSRGBRed:0.42 green:0.46 blue:0.51 alpha:1],  // kUiOff, 3.6:1 on the ground
    [NSColor colorWithSRGBRed:0.60 green:0.65 blue:0.71 alpha:1],  // kUiDim
    [NSColor colorWithSRGBRed:0.36 green:1.0 blue:0.58 alpha:1],   // kUiLtc
    [NSColor colorWithSRGBRed:0.45 green:0.78 blue:1.0 alpha:1],   // kUiDaw
    amber, amber                                                   // kUiCoast, kUiWarn
  ];

  _ipField = [[NSTextField alloc] initWithFrame:NSMakeRect(86, kH - kBar + 7, 150, 22)];
  _ipField.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
  _ipField.stringValue = [NSString stringWithUTF8String:plug->ip.c_str()];
  _ipField.bezelStyle = NSTextFieldRoundedBezel;
  _ipField.focusRingType = NSFocusRingTypeNone;
  _ipField.target = self;
  _ipField.action = @selector(ipEntered:);
  _ipField.delegate = self;
  [self addSubview:_ipField];

  // latency row: value in ms or frames (type it, or step one; hold Shift while clicking the stepper = one of the other unit)
  _latShown = plug->latencyShown();
  _latField = [[NSTextField alloc] initWithFrame:NSMakeRect(86, kH - kBar + 40, 70, 22)];
  _latField.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
  _latField.alignment = NSTextAlignmentRight;
  _latField.bezelStyle = NSTextFieldRoundedBezel;
  _latField.focusRingType = NSFocusRingTypeNone;
  _latField.stringValue = [NSString stringWithFormat:@(plug->latencyFormat()), _latShown];
  _latField.target = self;
  _latField.action = @selector(latencyEntered:);
  [self addSubview:_latField];
  _latStepper = [[NSStepper alloc] initWithFrame:NSMakeRect(158, kH - kBar + 38, 19, 27)];
  _latStepper.minValue = -1;  // used for its direction only: the action reads the sign and puts it back to 0
  _latStepper.maxValue = 1;
  _latStepper.increment = 1.0;
  _latStepper.valueWraps = NO;
  _latStepper.autorepeat = YES;
  _latStepper.doubleValue = 0;
  _latStepper.target = self;
  _latStepper.action = @selector(latencyStepped:);
  [self addSubview:_latStepper];
  _unitSeg = [NSSegmentedControl segmentedControlWithLabels:@[ @(S::latencyUnits[0]), @(S::latencyUnits[1]) ]
                                               trackingMode:NSSegmentSwitchTrackingSelectOne
                                                     target:self
                                                     action:@selector(unitChanged:)];
  _unitSeg.frame = NSMakeRect(182, kH - kBar + 39, 60, 24);
  _unitSeg.controlSize = NSControlSizeSmall;
  _unitSeg.font = [NSFont systemFontOfSize:11];
  _unitSeg.selectedSegment = plug->latencyUnit.load();
  [self addSubview:_unitSeg];

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

  _exclSeg = [NSSegmentedControl segmentedControlWithLabels:@[ @(S::exclusive) ]
                                               trackingMode:NSSegmentSwitchTrackingSelectAny
                                                     target:self
                                                     action:@selector(exclusiveChanged:)];
  _exclSeg.frame = NSMakeRect(kW - 12 - 92, kH - kBar + 39, 92, 24);
  _exclSeg.controlSize = NSControlSizeSmall;
  _exclSeg.font = [NSFont systemFontOfSize:11];
  [_exclSeg setSelected:plug->sender.exclusive() forSegment:0];
  [self addSubview:_exclSeg];

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

- (void)unitChanged:(id)sender {
  if (!_plug) return;
  _plug->setLatencyUnitFromGui((int)_unitSeg.selectedSegment);
  [self showLatency];
}

- (void)exclusiveChanged:(id)sender {
  if (_plug) _plug->setExclusiveFromGui([_exclSeg isSelectedForSegment:0]);
}

- (void)tick:(NSTimer *)t {
  if (_plug) {  // follow changes made from the host (parameter slider, automation)
    if (_srcSeg.selectedSegment != _plug->mode.load()) _srcSeg.selectedSegment = _plug->mode.load();
    if ([_muteSeg isSelectedForSegment:0] != (_plug->muteLtc.load() != 0)) [_muteSeg setSelected:_plug->muteLtc.load() != 0 forSegment:0];
    if ([_exclSeg isSelectedForSegment:0] != _plug->sender.exclusive()) [_exclSeg setSelected:_plug->sender.exclusive() forSegment:0];
    if ((_tickCount++ % 30) == 0) _plug->refreshProject();  // project frame rate / start offset, once a second
    if (_plug->latencyShown() != _latShown && _latField.currentEditor == nil) [self showLatency];
  }
  [self setNeedsDisplay:YES];
}

- (void)showLatency {  // in the unit the window is set to
  if (!_plug) return;
  _latShown = _plug->latencyShown();
  _latField.stringValue = [NSString stringWithFormat:@(_plug->latencyFormat()), _latShown];
}

- (void)latencyEntered:(id)sender {
  if (!_plug) return;
  NSString *s = [_latField.stringValue stringByReplacingOccurrencesOfString:@"," withString:@"."];
  _plug->setLatencyShownFromGui(s.doubleValue);
  [self showLatency];
  [self.window makeFirstResponder:self];
}

- (void)latencyStepped:(id)sender {
  if (!_plug) return;
  const int dir = _latStepper.doubleValue > 0 ? 1 : -1;
  _latStepper.doubleValue = 0;
  _plug->stepLatencyFromGui(dir, (NSEvent.modifierFlags & NSEventModifierFlagShift) != 0);  // Shift = one of the other unit
  [self showLatency];
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

- (NSDictionary *)attrsWithFont:(NSFont *)font color:(UiColor)c {
  NSMutableDictionary *a = [NSMutableDictionary dictionaryWithCapacity:2];
  if (font) a[NSFontAttributeName] = font;
  NSColor *col = (NSUInteger)c < _colors.count ? _colors[c] : nil;
  if (col) a[NSForegroundColorAttributeName] = col;
  return a;
}

- (void)drawCentered:(const char *)s font:(NSFont *)font color:(UiColor)c y:(CGFloat)y {
  NSString *str = s ? [NSString stringWithUTF8String:s] : nil;
  if (!str.length) return;
  NSDictionary *a = [self attrsWithFont:font color:c];
  [str drawAtPoint:NSMakePoint(floor((kW - [str sizeWithAttributes:a].width) / 2), y) withAttributes:a];
}

- (void)draw:(const char *)s at:(NSPoint)pt color:(UiColor)c rightAligned:(BOOL)right {
  NSString *str = s ? [NSString stringWithUTF8String:s] : nil;
  if (!str.length) return;
  NSDictionary *a = [self attrsWithFont:_fUi color:c];
  if (right) pt.x -= [str sizeWithAttributes:a].width;
  [str drawAtPoint:pt withAttributes:a];
}

- (void)drawRect:(NSRect)dirty {
  @try {  // an exception here would otherwise terminate the host
    [[NSColor colorWithSRGBRed:0.055 green:0.062 blue:0.075 alpha:1] setFill];
    NSRectFill(self.bounds);
    if (!_plug) return;
    const UiState u = ui_state(*_plug);
    [self drawCentered:u.tc font:_fTc color:u.tcColor y:18];
    [self drawCentered:u.line.c_str() font:_fUi color:u.lineColor y:96];

    [[NSColor colorWithSRGBRed:0.10 green:0.11 blue:0.13 alpha:1] setFill];
    NSRectFill(NSMakeRect(0, kH - kBar, kW, kBar));
    [self draw:S::artnetTo at:NSMakePoint(12, kH - kBar + 10) color:kUiDim rightAligned:NO];
    [self draw:S::latency at:NSMakePoint(12, kH - kBar + 43) color:kUiDim rightAligned:NO];
    [self draw:S::source at:NSMakePoint(12, kH - kBar + 77) color:kUiDim rightAligned:NO];
    [self draw:u.latencyNote.c_str() at:NSMakePoint(248, kH - kBar + 43) color:kUiDim rightAligned:NO];
    [self draw:u.send.c_str() at:NSMakePoint(kW - 12, kH - kBar + 10) color:u.sendColor rightAligned:YES];
  } @catch (NSException *e) {
    NSLog(@"CT LTC ArtNet: draw failed: %@", e);
  }
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
