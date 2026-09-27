// Headless library-index benchmark for the simulator (env:simulator_bench).
//
// Runs library::buildLibraryIndex() against the simulated SD root
// (CROSSPOINT_SIM_SD) and prints one "BENCH {json}" line per timed run with
// wall time, CPU time, the builder's BuildStats and the simulator's storage-call
// counts. scripts/library_bench.py drives it and compares builds.
//
//   --runs N          timed runs (default 5)
//   --prep MODE       cold:    delete /.crosspoint before every run
//                     warm:    one untimed priming build, then unchanged rebuilds
//                     changed: priming build, then bump --touch's mtime before each run
//   --touch PATH      SD path of the book to modify for --prep changed
//   --no-metadata     build from filenames only
#if defined(SIMULATOR) && defined(CROSSPOINT_LIBRARY_BENCH)

#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <SimulatorIoStats.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace {

enum class Prep { Cold, Warm, Changed };

struct BenchOptions {
  int runs = 5;
  Prep prep = Prep::Cold;
  const char* touchPath = nullptr;
  bool readMetadata = true;
};

const char* prepName(const Prep prep) {
  switch (prep) {
    case Prep::Cold:
      return "cold";
    case Prep::Warm:
      return "warm";
    case Prep::Changed:
      return "changed";
  }
  return "?";
}

bool parseOptions(const int argc, char** argv, BenchOptions& options) {
  for (int i = 1; i < argc; i++) {
    const char* arg = argv[i];
    const bool hasValue = i + 1 < argc;
    if (strcmp(arg, "--runs") == 0 && hasValue) {
      options.runs = atoi(argv[++i]);
    } else if (strcmp(arg, "--prep") == 0 && hasValue) {
      const char* mode = argv[++i];
      if (strcmp(mode, "cold") == 0) {
        options.prep = Prep::Cold;
      } else if (strcmp(mode, "warm") == 0) {
        options.prep = Prep::Warm;
      } else if (strcmp(mode, "changed") == 0) {
        options.prep = Prep::Changed;
      } else {
        fprintf(stderr, "unknown --prep %s\n", mode);
        return false;
      }
    } else if (strcmp(arg, "--touch") == 0 && hasValue) {
      options.touchPath = argv[++i];
    } else if (strcmp(arg, "--no-metadata") == 0) {
      options.readMetadata = false;
    } else {
      fprintf(stderr, "unknown argument %s\n", arg);
      return false;
    }
  }
  if (options.runs < 1) {
    fprintf(stderr, "--runs must be at least 1\n");
    return false;
  }
  if (options.prep == Prep::Changed && !options.touchPath) {
    fprintf(stderr, "--prep changed needs --touch PATH\n");
    return false;
  }
  return true;
}

// Host path behind an SD path, resolved the way the simulator's HalStorage does.
std::string hostPath(const char* sdPath) {
  const char* root = getenv("CROSSPOINT_SIM_SD");
  std::string path = (root && *root) ? root : "./fs_";
  if (sdPath[0] != '/') path.push_back('/');
  path += sdPath;
  return path;
}

// Moves the file's mtime forward by one FAT tick (2 s), so the builder sees a
// changed book exactly as it would on the device.
bool bumpModificationTime(const char* sdPath) {
  const std::string path = hostPath(sdPath);
  struct stat info {};
  if (stat(path.c_str(), &info) != 0) {
    fprintf(stderr, "cannot stat %s\n", path.c_str());
    return false;
  }
  const time_t modified = info.st_mtime + 2;
  struct timeval times[2] = {{info.st_atime, 0}, {modified, 0}};
  return utimes(path.c_str(), times) == 0;
}

double msSince(const std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void printRun(const BenchOptions& options, const int run, const bool ok, const double wallMs, const double cpuMs,
              const library::BuildStats& stats) {
  const SimulatorIoStats& io = simulatorIoStats();
  printf(
      "BENCH {\"prep\":\"%s\",\"metadata\":%s,\"run\":%d,\"ok\":%s,\"wallMs\":%.3f,\"cpuMs\":%.3f,"
      "\"books\":%u,\"folders\":%u,\"parsed\":%u,\"metadataReused\":%u,\"unchanged\":%u,\"added\":%u,"
      "\"removed\":%u,\"renamed\":%u,\"enriched\":%u,\"indexReplaced\":%s,\"ranksDegraded\":%s,"
      "\"io\":{\"opens\":%llu,\"reads\":%llu,\"bytesRead\":%llu,\"writes\":%llu,\"bytesWritten\":%llu,"
      "\"seeks\":%llu,\"dirEntries\":%llu,\"pathOps\":%llu}}\n",
      prepName(options.prep), options.readMetadata ? "true" : "false", run, ok ? "true" : "false", wallMs, cpuMs,
      stats.books, stats.folders, stats.parsed, stats.metadataReused, stats.unchanged, stats.added, stats.removed,
      stats.renamed, stats.enriched, stats.indexReplaced ? "true" : "false", stats.ranksDegraded ? "true" : "false",
      static_cast<unsigned long long>(io.opens), static_cast<unsigned long long>(io.reads),
      static_cast<unsigned long long>(io.bytesRead), static_cast<unsigned long long>(io.writes),
      static_cast<unsigned long long>(io.bytesWritten), static_cast<unsigned long long>(io.seeks),
      static_cast<unsigned long long>(io.dirEntries), static_cast<unsigned long long>(io.pathOps));
  fflush(stdout);
}

}  // namespace

int crosspointSimHeadlessMain(const int argc, char** argv) {
  BenchOptions options;
  if (!parseOptions(argc, argv, options)) return 2;
  if (!Storage.begin()) {
    fprintf(stderr, "storage unavailable\n");
    return 1;
  }

  if (options.prep != Prep::Cold) {
    // The priming build is part of the setup, not the measurement.
    Storage.removeDir("/.crosspoint");
    library::BuildStats priming;
    if (!library::buildLibraryIndex("/", priming, options.readMetadata)) {
      fprintf(stderr, "priming build failed\n");
      return 1;
    }
  }

  int failures = 0;
  for (int run = 0; run < options.runs; run++) {
    if (options.prep == Prep::Cold) {
      Storage.removeDir("/.crosspoint");
    } else if (options.prep == Prep::Changed && !bumpModificationTime(options.touchPath)) {
      return 1;
    }

    resetSimulatorIoStats();
    library::BuildStats stats;
    const std::clock_t cpuStart = std::clock();
    const auto wallStart = std::chrono::steady_clock::now();
    const bool ok = library::buildLibraryIndex("/", stats, options.readMetadata);
    const double wallMs = msSince(wallStart);
    const double cpuMs = 1000.0 * static_cast<double>(std::clock() - cpuStart) / CLOCKS_PER_SEC;
    printRun(options, run, ok, wallMs, cpuMs, stats);
    if (!ok) failures++;
  }
  return failures == 0 ? 0 : 1;
}

#endif
