// Host test of the iambic keyer state machine (keyer.cpp) with mocked GPIO.
// Drives paddle scenarios with a 1 ms tick and checks the generated edge
// times against the expected element pattern (in dits, 20 WPM = 60 ms).
//
//   g++ -std=gnu++17 -w -Imock -I../.. keyer_sim.cpp -o keyer_sim && ./keyer_sim
#include <string>
#include <vector>
#include "shared.h"
int g_pin[64];
Settings g_set;
void ledcWrite(uint8_t, uint32_t) {}
#include "../../keyer.cpp"

struct P { int ms; int dit; int dah; };          // paddle state from this ms on

static std::string run(char mode, const std::vector<P>& ps, int endms, int testAt = -1) {
  g_set.wpm = 20; g_set.mode = mode; g_set.vol = 25; g_set.swap = false; g_set.skEnable = false;
  for (int i = 0; i < 64; i++) g_pin[i] = 1;
  kState = K_IDLE; ditBuf = dahBuf = 0; beingSent = 0; lastSent = 1; iambicAFlag = 0;
  nextIdx = 0; edgeCount = 0; msg = nullptr; msgReq = false;
  size_t k = 0; uint32_t seen = 0, lastT = 0; bool lastD = false, first = true;
  std::string out;
  for (int ms = 0; ms < endms; ms++) {
    while (k < ps.size() && ps[k].ms <= ms) { g_pin[PIN_DIT] = !ps[k].dit; g_pin[PIN_DAH] = !ps[k].dah; k++; }
    if (ms == testAt) keyer_send_test();
    keyer_tick(1000000u + ms * 1000u + 137);     // tick phase not aligned to the ms
    while (seen < edgeCount) {
      uint32_t e = ring[seen & (EDGE_RING - 1)]; seen++;
      uint32_t t = RKEY_EV_TIME(e);
      if (!first) {
        char b[24]; snprintf(b, sizeof b, "%s%.2f ", lastD ? "M" : "s", (t - lastT) / 60000.0); out += b;
      }
      first = false; lastT = t; lastD = RKEY_EV_DOWN(e);
    }
  }
  if (!out.empty()) out.pop_back();
  return out;
}

int main() {
  struct T { const char* name; char mode; std::vector<P> ps; int end; const char* want; int testAt; } tests[] = {   // testAt: ms of keyer_send_test(), 0 = none
    { "hold dit",                     'B', {{0,1,0},{400,0,0}}, 1000, "M1.00 s1.00 M1.00 s1.00 M1.00 s1.00 M1.00" },
    { "hold dah",                     'B', {{0,0,1},{600,0,0}}, 1500, "M3.00 s1.00 M3.00 s1.00 M3.00" },
    { "squeeze B",                    'B', {{0,1,0},{5,1,1},{500,0,0}}, 1500, "M1.00 s1.00 M3.00 s1.00 M1.00 s1.00 M3.00" },
    { "squeeze A",                    'A', {{0,1,0},{5,1,1},{500,0,0}}, 1500, "M1.00 s1.00 M3.00 s1.00 M1.00 s1.00 M3.00" },
    { "release early in dit (CMOS)",  'B', {{0,0,1},{30,1,1},{250,0,0}}, 1500, "M3.00 s1.00 M1.00" },
    { "release at 50% of dit, B",     'B', {{0,0,1},{30,1,1},{270,0,0}}, 1500, "M3.00 s1.00 M1.00 s1.00 M3.00" },
    { "release at 50% of dit, A",     'A', {{0,0,1},{30,1,1},{270,0,0}}, 1500, "M3.00 s1.00 M1.00" },
    { "dit tap 10 ms",                'B', {{0,1,0},{10,0,0}}, 500, "M1.00" },
    { "dit latched during dah",       'B', {{0,0,1},{100,0,0},{120,1,0},{140,0,0}}, 800, "M3.00 s1.00 M1.00" },
    { "TEST message",                 'B', {}, 2000, "M3.00 s3.00 M1.00 s3.00 M1.00 s1.00 M1.00 s1.00 M1.00 s3.00 M3.00", 1 },
    { "TEST aborted by dit",          'B', {{100,1,0},{130,0,0}}, 2000, "M3.00 s1.00 M1.00", 1 },
    { "TEST waits for the paddle",    'B', {{0,1,0},{30,0,0}}, 2000, "M1.00", 10 },
  };
  int fails = 0;
  for (auto& t : tests) {
    std::string got = run(t.mode, t.ps, t.end, t.testAt ? t.testAt : -1);
    bool ok = got == t.want;
    fails += !ok;
    printf("%s  %-30s %s%s%s\n", ok ? "PASS" : "FAIL", t.name, got.c_str(), ok ? "" : "  want: ", ok ? "" : t.want);
  }
  printf(fails ? "%d FAILED\n" : "all passed\n", fails);
  return fails ? 1 : 0;
}
