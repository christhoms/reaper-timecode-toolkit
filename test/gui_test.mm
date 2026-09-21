// Opens the built plugin's GUI in a plain Cocoa window, feeds LTC for a second, and writes the view to a PNG.
//   ./build/gui_test <plugin binary> <out.png> [ip]
#import <Cocoa/Cocoa.h>
#include <dlfcn.h>
#include <vector>
#include <clap/clap.h>
#include "ltc_core.h"
#define main test_core_main
#include "test_core.cpp"
#undef main

int main(int argc, char **argv) {
  @autoreleasepool {
    if (argc < 3) return 2;
    setenv("CT_ARTNET_PORT", "16454", 1);
    setenv("CT_ARTNET_IP", argc > 3 ? argv[3] : "", 1);
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    void *h = dlopen(argv[1], RTLD_NOW);
    auto *entry = (const clap_plugin_entry_t *)dlsym(h, "clap_entry");
    entry->init(argv[1]);
    auto *fac = (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    clap_host_t host = {CLAP_VERSION_INIT, nullptr, "guitest", "ct", "", "1", host_get_ext, host_noop, host_noop, host_noop};
    const clap_plugin_t *p = fac->create_plugin(fac, &host, fac->get_plugin_descriptor(fac, 0)->id);
    p->init(p);
    auto *gui = (const clap_plugin_gui_t *)p->get_extension(p, CLAP_EXT_GUI);
    uint32_t w = 0, hh = 0;
    bool ok = gui->create(p, CLAP_WINDOW_API_COCOA, false) && gui->get_size(p, &w, &hh);
    NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 200, w, hh) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    clap_window_t cw; cw.api = CLAP_WINDOW_API_COCOA; cw.cocoa = (__bridge void *)win.contentView;
    ok = ok && gui->set_parent(p, &cw) && gui->show(p);
    std::printf("gui create/set_parent/show: %s (%ux%u)\n", ok ? "ok" : "FAILED", w, hh);

    const double sr = 48000; const uint32_t bs = 512;
    p->activate(p, sr, bs, bs); p->start_processing(p);
    LtcGen gen(sr, 30, 30000.0 / 1001.0, true, tc_to_count(6, 30, 12, 7, 30, true));  // worst case for the status line width
    std::vector<float> l(bs), r(bs);
    float *io[2] = {l.data(), r.data()};
    clap_audio_buffer_t in{}, out{}; in.data32 = io; in.channel_count = 2; out.data32 = io; out.channel_count = 2;  // in place
    clap_process_t pr{}; pr.frames_count = bs; pr.audio_inputs = &in; pr.audio_outputs = &out; pr.audio_inputs_count = pr.audio_outputs_count = 1;
    const bool dawMode = argc > 4 && !std::strcmp(argv[4], "daw");  // silent input, transport playing from 00:20:00
    const bool feed = argc <= 4;
    clap_event_transport_t tr{}; tr.header.size = sizeof(tr); tr.header.type = CLAP_EVENT_TRANSPORT;
    tr.flags = CLAP_TRANSPORT_IS_PLAYING | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
    if (dawMode) pr.transport = &tr;
    double t0 = now_s();
    for (int b = 0; b < int(sr * 1.2 / bs); b++) {
      for (uint32_t i = 0; i < bs; i++) { l[i] = 0; r[i] = feed ? gen.next() : 0.0f; }
      while (now_s() < t0 + b * bs / sr) [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.002]];
      tr.song_pos_seconds = (clap_sectime)std::llround((1200.0 + b * bs / sr) * double(CLAP_SECTIME_FACTOR));
      p->process(p, &pr);
    }
    NSView *v = win.contentView.subviews.firstObject;
    NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
    [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[NSString stringWithUTF8String:argv[2]] atomically:YES];
    gui->hide(p); gui->destroy(p);
    p->stop_processing(p); p->deactivate(p); p->destroy(p);
    std::printf("closed cleanly\n");
  }
  return 0;
}
