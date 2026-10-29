// PGO instrumented builds only (SKATE3_PGO=GEN / SK83_PGO=GEN, see CMakeLists.txt).
//
// The clang profile runtime writes its counters at exit, but the game is
// normally closed by killing the process, so a background thread rewrites
// the raw profile every few seconds instead. Each write replaces the file
// with the cumulative counters so far, so the last one is the whole run.

#ifdef SKATE3_PGO_GEN

#include <chrono>
#include <cstdio>
#include <thread>

#include <windows.h>

#ifndef SKATE3_PGO_RAW_PREFIX
#define SKATE3_PGO_RAW_PREFIX "skate3"
#endif

extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char* name);

namespace {

struct PgoDumper {
  PgoDumper() {
    // Written to a temp name, then renamed over the final one: a kill in
    // the middle of a write never leaves a truncated profile behind.
    static char tmp[512], path[512];
    const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
    std::snprintf(tmp, sizeof(tmp), "%s/%s-%lu.profraw.tmp", SKATE3_PGO_RAW_DIR,
                  SKATE3_PGO_RAW_PREFIX, pid);
    std::snprintf(path, sizeof(path), "%s/%s-%lu.profraw", SKATE3_PGO_RAW_DIR,
                  SKATE3_PGO_RAW_PREFIX, pid);
    __llvm_profile_set_filename(tmp);
    std::thread([] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (__llvm_profile_write_file() == 0) {
          MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
        }
      }
    }).detach();
  }
};

PgoDumper g_pgo_dumper;

}  // namespace

#endif  // SKATE3_PGO_GEN
