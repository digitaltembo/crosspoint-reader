// Headless library benchmark for the simulator (env:simulator_bench).
//
// Runs against the simulated SD root (CROSSPOINT_SIM_SD) and prints one
// "BENCH {json}" line per timed run with wall time, CPU time and the
// simulator's storage-call counts. scripts/library_bench.py drives it and
// compares builds.
//
//   --mode build      (default) time library::buildLibraryIndex()
//     --prep MODE     cold:    delete /.crosspoint before every run
//                     warm:    one untimed priming build, then unchanged rebuilds
//                     changed: priming build, then bump --touch's mtime before each run
//     --touch PATH    SD path of the book to modify for --prep changed
//   --mode search     build the index once (untimed), then time each search
//     --query Q       a query as typed on the keyboard; repeatable
//     --orders LIST   comma-separated sort orders: title, author, recent (default all)
//   --runs N          timed runs per build or per query and order (default 5)
//   --no-metadata     build from filenames only
#if defined(SIMULATOR) && defined(CROSSPOINT_LIBRARY_BENCH)

#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <LibraryText.h>
#include <Memory.h>
#include <SimulatorIoStats.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

// Baselines from before the search loop moved out of LibraryListActivity have
// no LibrarySearch.h; searchRows() carries a copy of that older loop for them.
#if __has_include(<LibrarySearch.h>)
#include <LibrarySearch.h>
#define BENCH_HAS_LIBRARY_SEARCH 1
#else
#define BENCH_HAS_LIBRARY_SEARCH 0
#endif

namespace {

enum class Mode { Build, Search };
enum class Prep { Cold, Warm, Changed };

struct SortChoice {
  const char* name;
  library::SortOrder order;
};

constexpr SortChoice SORT_CHOICES[] = {
    {"title", library::SortOrder::TitleAsc},
    {"author", library::SortOrder::AuthorAsc},
    {"recent", library::SortOrder::RecentDesc},
};

struct BenchOptions {
  Mode mode = Mode::Build;
  int runs = 5;
  Prep prep = Prep::Cold;
  const char* touchPath = nullptr;
  bool readMetadata = true;
  std::vector<const char*> queries;
  std::vector<SortChoice> orders;
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

bool parseOrders(const char* list, std::vector<SortChoice>& out) {
  std::string remaining(list);
  size_t start = 0;
  while (start <= remaining.size()) {
    size_t end = remaining.find(',', start);
    if (end == std::string::npos) end = remaining.size();
    const std::string name = remaining.substr(start, end - start);
    bool found = false;
    for (const auto& choice : SORT_CHOICES) {
      if (name == choice.name) {
        out.push_back(choice);
        found = true;
      }
    }
    if (!found) {
      fprintf(stderr, "unknown sort order %s\n", name.c_str());
      return false;
    }
    start = end + 1;
  }
  return true;
}

bool parseOptions(const int argc, char** argv, BenchOptions& options) {
  options.queries.reserve(static_cast<size_t>(argc));
  for (int i = 1; i < argc; i++) {
    const char* arg = argv[i];
    const bool hasValue = i + 1 < argc;
    if (strcmp(arg, "--mode") == 0 && hasValue) {
      const char* mode = argv[++i];
      if (strcmp(mode, "build") == 0) {
        options.mode = Mode::Build;
      } else if (strcmp(mode, "search") == 0) {
        options.mode = Mode::Search;
      } else {
        fprintf(stderr, "unknown --mode %s\n", mode);
        return false;
      }
    } else if (strcmp(arg, "--runs") == 0 && hasValue) {
      options.runs = atoi(argv[++i]);
    } else if (strcmp(arg, "--prep") == 0 && hasValue) {
      const char* prep = argv[++i];
      if (strcmp(prep, "cold") == 0) {
        options.prep = Prep::Cold;
      } else if (strcmp(prep, "warm") == 0) {
        options.prep = Prep::Warm;
      } else if (strcmp(prep, "changed") == 0) {
        options.prep = Prep::Changed;
      } else {
        fprintf(stderr, "unknown --prep %s\n", prep);
        return false;
      }
    } else if (strcmp(arg, "--touch") == 0 && hasValue) {
      options.touchPath = argv[++i];
    } else if (strcmp(arg, "--query") == 0 && hasValue) {
      options.queries.push_back(argv[++i]);
    } else if (strcmp(arg, "--orders") == 0 && hasValue) {
      options.orders.clear();
      if (!parseOrders(argv[++i], options.orders)) return false;
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
  if (options.mode == Mode::Build && options.prep == Prep::Changed && !options.touchPath) {
    fprintf(stderr, "--prep changed needs --touch PATH\n");
    return false;
  }
  if (options.mode == Mode::Search && options.queries.empty()) {
    fprintf(stderr, "--mode search needs at least one --query\n");
    return false;
  }
  if (options.orders.empty()) options.orders.assign(std::begin(SORT_CHOICES), std::end(SORT_CHOICES));
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
  struct stat info{};
  if (stat(path.c_str(), &info) != 0) {
    fprintf(stderr, "cannot stat %s\n", path.c_str());
    return false;
  }
  const time_t modified = info.st_mtime + 2;
  struct timeval times[2] = {{info.st_atime, 0}, {modified, 0}};
  return utimes(path.c_str(), times) == 0;
}

struct Timer {
  std::clock_t cpuStart = std::clock();
  std::chrono::steady_clock::time_point wallStart = std::chrono::steady_clock::now();

  double wallMs() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wallStart).count();
  }
  double cpuMs() const { return 1000.0 * static_cast<double>(std::clock() - cpuStart) / CLOCKS_PER_SEC; }
};

// The shared `"io":{...}` member of every BENCH line.
void printIo() {
  const SimulatorIoStats& io = simulatorIoStats();
  printf(
      "\"io\":{\"opens\":%llu,\"reads\":%llu,\"bytesRead\":%llu,\"writes\":%llu,\"bytesWritten\":%llu,"
      "\"seeks\":%llu,\"dirEntries\":%llu,\"pathOps\":%llu}",
      static_cast<unsigned long long>(io.opens), static_cast<unsigned long long>(io.reads),
      static_cast<unsigned long long>(io.bytesRead), static_cast<unsigned long long>(io.writes),
      static_cast<unsigned long long>(io.bytesWritten), static_cast<unsigned long long>(io.seeks),
      static_cast<unsigned long long>(io.dirEntries), static_cast<unsigned long long>(io.pathOps));
}

void printJsonString(const char* text) {
  putchar('"');
  for (const char* c = text; *c; c++) {
    if (*c == '"' || *c == '\\') {
      printf("\\%c", *c);
    } else if (static_cast<unsigned char>(*c) < 0x20) {
      printf("\\u%04x", static_cast<unsigned char>(*c));
    } else {
      putchar(*c);
    }
  }
  putchar('"');
}

void printBuildRun(const BenchOptions& options, const int run, const bool ok, const Timer& timer,
                   const library::BuildStats& stats) {
  printf(
      "BENCH {\"mode\":\"build\",\"prep\":\"%s\",\"metadata\":%s,\"run\":%d,\"ok\":%s,\"wallMs\":%.3f,"
      "\"cpuMs\":%.3f,\"books\":%u,\"folders\":%u,\"parsed\":%u,\"metadataReused\":%u,\"unchanged\":%u,"
      "\"added\":%u,\"removed\":%u,\"renamed\":%u,\"enriched\":%u,\"indexReplaced\":%s,\"ranksDegraded\":%s,",
      prepName(options.prep), options.readMetadata ? "true" : "false", run, ok ? "true" : "false", timer.wallMs(),
      timer.cpuMs(), stats.books, stats.folders, stats.parsed, stats.metadataReused, stats.unchanged, stats.added,
      stats.removed, stats.renamed, stats.enriched, stats.indexReplaced ? "true" : "false",
      stats.ranksDegraded ? "true" : "false");
  printIo();
  printf("}\n");
  fflush(stdout);
}

void printSearchRun(const BenchOptions& options, const char* query, const SortChoice& order, const int run,
                    const uint16_t books, const uint16_t matches, const Timer& timer) {
  printf("BENCH {\"mode\":\"search\",\"metadata\":%s,\"query\":", options.readMetadata ? "true" : "false");
  printJsonString(query);
  printf(",\"order\":\"%s\",\"run\":%d,\"books\":%u,\"matches\":%u,\"wallMs\":%.3f,\"cpuMs\":%.3f,", order.name, run,
         books, matches, timer.wallMs(), timer.cpuMs());
  printIo();
  printf("}\n");
  fflush(stdout);
}

uint16_t searchRows(library::LibraryIndexFile& index, const library::SortOrder order, const char* query,
                    uint16_t* out) {
#if BENCH_HAS_LIBRARY_SEARCH
  return library::filterRows(index, order, query, out);
#else
  // LibraryListActivity::applyFilter's loop at the time: title fold, then author.
  const std::string needle = library::fold(query);
  const uint16_t total = index.bookCount();
  uint16_t count = 0;
  std::string author;
  for (uint16_t row = 0; row < total; row++) {
    const uint16_t ordinal = index.ordinalForRow(order, row);
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    if (library::matchesQuery(std::string_view(record.fold, record.foldLen), needle)) {
      out[count++] = row;
      continue;
    }
    author.clear();
    if (index.readAuthor(record, author) && library::matchesQuery(library::fold(author), needle)) {
      out[count++] = row;
    }
  }
  return count;
#endif
}

int runBuild(const BenchOptions& options) {
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
    const Timer timer;
    const bool ok = library::buildLibraryIndex("/", stats, options.readMetadata);
    printBuildRun(options, run, ok, timer, stats);
    if (!ok) failures++;
  }
  return failures == 0 ? 0 : 1;
}

int runSearch(const BenchOptions& options) {
  Storage.removeDir("/.crosspoint");
  library::BuildStats stats;
  if (!library::buildLibraryIndex("/", stats, options.readMetadata)) {
    fprintf(stderr, "index build failed\n");
    return 1;
  }

  library::LibraryIndexFile index;
  if (!index.open(library::libraryIndexPath())) {
    fprintf(stderr, "cannot open the index: %s\n", library::clixValidityName(index.validity()));
    return 1;
  }
  const uint16_t books = index.bookCount();
  auto rows = makeUniqueNoThrow<uint16_t[]>(books == 0 ? 1 : books);
  if (!rows) {
    fprintf(stderr, "OOM: search rows\n");
    return 1;
  }

  for (const char* query : options.queries) {
    for (const SortChoice& order : options.orders) {
      for (int run = 0; run < options.runs; run++) {
        resetSimulatorIoStats();
        const Timer timer;
        const uint16_t matches = searchRows(index, order.order, query, rows.get());
        printSearchRun(options, query, order, run, books, matches, timer);
      }
    }
  }
  return 0;
}

}  // namespace

int crosspointSimHeadlessMain(const int argc, char** argv) {
  BenchOptions options;
  if (!parseOptions(argc, argv, options)) return 2;
  if (!Storage.begin()) {
    fprintf(stderr, "storage unavailable\n");
    return 1;
  }
  return options.mode == Mode::Search ? runSearch(options) : runBuild(options);
}

#endif
