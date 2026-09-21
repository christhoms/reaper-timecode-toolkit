// Offline check of the decoder + coaster on a real recording: mono float32 samples on stdin.
//   ffmpeg -v error -i <file> -af "pan=mono|c0=c1" -f f32le -ar 48000 - | build/ltc_file_check [srate] [coast limit]
// Prints what the bare decoder gets, what comes out with coasting, and every discontinuity in the output.
#include <cstdio>
#include <vector>
#include "ltc_core.h"
using namespace ctltc;

int main(int argc, char **argv) {
  const double sr = argc > 1 ? std::atof(argv[1]) : 48000.0;
  const int limit = argc > 2 ? std::atoi(argv[2]) : kCoastDefault;
  Decoder d; d.reset(sr);
  Coaster c; c.reset(sr); c.setLimit(limit);
  Frame raw, fr;
  std::vector<float> buf(1 << 16);
  long i = 0, checked = 0, rawOnly = 0, out = 0, synth = 0, prev = -1, run = 0, longest = 0, jumps = 0;
  double prevSamp = 0;
  size_t n;
  while ((n = std::fread(buf.data(), sizeof(float), buf.size(), stdin)) > 0) {
    for (size_t k = 0; k < n; k++, i++) {
      const int kind = d.pushRaw(buf[k], raw);
      checked += kind == 2; rawOnly += kind == 1;
      double back = 0; bool s = false;
      if (!c.tick(kind, raw, d.sinceEdge(), fr, back, s)) continue;
      const long cnt = tc_to_count(fr.h, fr.m, fr.s, fr.f, fr.fps, fr.df);
      const double samp = double(i + 1) - back;
      out++; synth += s;
      run = s ? run + 1 : 0; longest = std::max(longest, run);
      if (prev >= 0 && cnt != prev + 1) {
        jumps++;
        const Timecode a = count_to_tc(prev, fr.fps, fr.df);
        if (jumps <= 60) std::printf("  %9.3f s: %02d:%02d:%02d:%02d -> %02d:%02d:%02d:%02d  (%+ld frames, %.3f s after the previous frame)\n", samp / sr, a.h, a.m, a.s, a.f,
                                     fr.h, fr.m, fr.s, fr.f, cnt - prev - 1, (samp - prevSamp) / sr);
      }
      prev = cnt; prevSamp = samp;
    }
  }
  std::printf("%.1f s of audio, coast limit %d\n  decoder alone: %ld checked frames (+%ld well-formed but unchecked)\n  output: %ld frames, %ld filled in, longest filled-in run %ld, %ld not confirmed by a late decode, %ld discontinuities\n",
              i / sr, limit, checked, rawOnly, out, synth, longest, c.filled(), jumps);
  return 0;
}
