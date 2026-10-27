// Lane W test stub: a fake libopenvr_api.so for frame/tools/vrtiming.py (KARTFRAME_VRTIMING_FRAMES=1) without
// SteamVR. Build: cc -O1 -shared -fPIC -o ~/.cache/kf-<lane>/stub/libopenvr_api.so frame/tools/openvr-stub.c
// Run:   KARTFRAME_STEAMVR_DIR=~/.cache/kf-<lane>/stub KARTFRAME_VRTIMING_FRAMES=1 python3 -I frame/tools/vrtiming.py 5 DIR/vrtiming.log
// A 120 Hz compositor (lane AG: MOCK_XR_HZ, as the mock's, sets the rate) whose system time is CLOCK_MONOTONIC - 1000 s;
// frame index = vsyncs since that clock's zero.
// Lane X: STUB_UPDATE_MS=<start>,<end> sets the compositor's update window in ms after each vsync (default 5.1,5.4;
// mockrun.sh uses 3.0,5.3, the Frame's in session 19), so vrtiming.py's KARTFRAME_END_PHASE publisher has a grid.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct { float m[12]; } M34; typedef struct { float v[3]; } V3;
typedef struct { M34 a; V3 b, c; int r; _Bool v, d; } Pose;
typedef struct {
  uint32_t size, idx, pres, mis, drop, rp; double sys;
  float pre, post, total, cgpu, ccpu, idle, interval, pcall, wpres, submit, wgp, npr, nfr, us, ue, rs;
  Pose pose; uint32_t vr, vf; } FT;
static const double OFF = 1000.0;
static double P = 1.0 / 120.0;
__attribute__((constructor)) static void rate(void) {
  const char* hz = getenv("MOCK_XR_HZ");
  if (hz && atof(hz) >= 30.0) P = 1.0 / atof(hz);
}
static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static uint32_t timings(FT* f, uint32_t n) {
  double s = mono() - OFF; uint64_t cur = (uint64_t)(s / P);
  float us = 5.1f, ue = 5.4f;
  const char* w = getenv("STUB_UPDATE_MS");
  if (w) sscanf(w, "%f,%f", &us, &ue);
  for (uint32_t i = 0; i < n; i++) {
    uint64_t k = cur - (n - 1 - i); FT* t = &f[i]; memset(t, 0, sizeof *t);
    t->idx = (uint32_t)k; t->sys = k * P; t->pres = 1; t->mis = (k % 97) == 0;
    t->wgp = 0.5f; t->npr = 0.6f; t->nfr = 4.6f; t->us = us; t->ue = ue; t->rs = us + 0.02f;
    t->cgpu = 0.8f; t->interval = (float)(P * 1000.0); t->idle = 3.0f; t->vf = 2;
  }
  return n;
}
static void cumul(void* c, uint32_t n) { memset(c, 0, n); }
static _Bool since(float* s, uint64_t* c) { double t = mono() - OFF; *c = (uint64_t)(t / P); *s = (float)(t - *c * P); return 1; }
static void* comp[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, (void*)timings, 0, (void*)cumul};
static void* sys_[6] = {0, 0, 0, 0, 0, (void*)since};
uint32_t VR_InitInternal2(int* e, int t, const char* x) { (void)t; (void)x; *e = 0; return 1; }
void* VR_GetGenericInterface(const char* n, int* e) {
  *e = 0;
  if (!strcmp(n, "FnTable:IVRCompositor_027")) return comp;
  if (!strcmp(n, "FnTable:IVRSystem_022")) return sys_;
  *e = 1; return 0;
}
void VR_ShutdownInternal(void) {}
