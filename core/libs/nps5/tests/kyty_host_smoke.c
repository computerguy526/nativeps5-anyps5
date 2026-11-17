// nps5: smoke test of the NPS5_GPU=kyty host helpers in libnps5_gpu (core/libs/nps5/KytyHost.cpp) on real AnyPS5
// libkernel event queues. Not a CMake target (plain C, loads the built libraries):
//   clang -O1 core/libs/nps5/tests/kyty_host_smoke.c -o /tmp/kyty_host_smoke
//   DYLD_LIBRARY_PATH=build-arm64/core/libs/libs/unpatched NPS5_VPAD_SCRIPT="50:+cross;400:-cross" /tmp/kyty_host_smoke build-arm64/core/libs/libs/unpatched
// Prints "KYTY HOST OK <checks>".
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct KernelEvent { uintptr_t ident; int16_t filter; uint16_t flags; uint32_t fflags; intptr_t data; void *udata; } KernelEvent;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void *must(void *h, const char *n) { void *p = dlsym(h, n); if (!p) { fprintf(stderr, "missing %s\n", n); exit(1); } return p; }

int main(int argc, char **argv) {
  char path[1024];
  const char *dir = argc > 1 ? argv[1] : ".";
  snprintf(path, sizeof path, "%s/libkernel.prx", dir);
  void *k = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
  snprintf(path, sizeof path, "%s/libnps5_gpu.dylib", dir);
  void *g = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!k || !g) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
  int (*create)(int64_t *, const char *) = must(k, "sceKernelCreateEqueue");
  int (*wait)(int64_t, KernelEvent *, int, int *, const uint32_t *) = must(k, "sceKernelWaitEqueue");
  int (*add)(int64_t, uint32_t, int64_t, void *) = must(g, "Nps5KytyHostAddGpuEvent");
  int (*del)(int64_t, uint32_t, int64_t) = must(g, "Nps5KytyHostDeleteGpuEvent");
  void (*deliver)(uint32_t, int64_t, uint64_t) = must(g, "Nps5KytyHostDeliverEvent");
  int (*script)(void) = must(g, "Nps5KytyHostStartPadScript");
  must(g, "Nps5KytyHostPublishPad");
  must(g, "Nps5KytyHostRequestExit");

  int64_t eq = 0;
  CHECK(create(&eq, "kyty smoke") == 0 && eq != 0);
  CHECK(add(0, 1, 5, NULL) != 0);          // no queue
  CHECK(add(eq, 9, 5, NULL) != 0);         // unknown kind
  CHECK(add(eq, 1, 5, (void *)0x1234) == 0); // AGC end-of-pipe interrupt, event id 5
  CHECK(add(eq, 2, 1, (void *)0x5678) == 0); // VideoOut flip
  KernelEvent ev[4];
  int n = -1;
  uint32_t timeout = 20000; // 20 ms
  memset(ev, 0, sizeof ev);
  wait(eq, ev, 4, &n, &timeout);
  CHECK(n == 0); // nothing delivered yet
  deliver(1, 6, 0); // another event id: not ours
  deliver(1, 5, 0);
  deliver(1, 5, 0);
  n = -1; timeout = 200000;
  CHECK(wait(eq, ev, 4, &n, &timeout) == 0 && n == 1);
  CHECK(ev[0].filter == -14 && ev[0].ident == 5 && ev[0].udata == (void *)0x1234 && ev[0].data == 2); // two interrupts, one event
  deliver(2, 1, 0xabcd);
  n = -1; timeout = 200000;
  CHECK(wait(eq, ev, 4, &n, &timeout) == 0 && n == 1);
  CHECK(ev[0].filter == -13 && ev[0].ident == 0 && ev[0].udata == (void *)0x5678 && ev[0].data == 0xabcd);
  CHECK(del(eq, 1, 5) == 0);
  deliver(1, 5, 0);
  n = -1; timeout = 20000;
  wait(eq, ev, 4, &n, &timeout);
  CHECK(n == 0);
  // The scripted pad: 0 without a script, 1 with one (the thread then publishes port 0).
  // KYTY_SMOKE_BAD_SCRIPT=1: the script is expected to be rejected (-1, also on the second call).
  const int want = getenv("KYTY_SMOKE_BAD_SCRIPT") ? -1 : getenv("NPS5_VPAD_SCRIPT") && *getenv("NPS5_VPAD_SCRIPT") ? 1 : 0;
  CHECK(script() == want);
  if (want) { usleep(150000); CHECK(script() == want); }
  printf("KYTY HOST OK %d\n", checks);
  fflush(stdout);
  _exit(0);
}
