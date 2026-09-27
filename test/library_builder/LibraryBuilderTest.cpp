#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include "Epub.h"
#include "LibraryBuilder.h"
#include "LibraryGenerated.h"
#include "LibraryIndexFile.h"
#include "LibrarySearch.h"

using namespace library;

namespace {

constexpr char INDEX[] = "/.crosspoint/library.idx";

std::string numbered(const char* prefix, const unsigned value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%s%04u", prefix, value);
  return text;
}

std::string pathAt(LibraryIndexFile& index, const SortOrder order, const uint16_t row) {
  const uint16_t ordinal = index.ordinalForRow(order, row);
  if (ordinal == 0xFFFF) return {};
  ClixRecord record{};
  if (!index.readRecord(ordinal, record)) return {};
  std::string path;
  return index.readPath(record, path) ? path : std::string();
}

// Rewrite a current index as the previous format would have stored it: same
// folders, records and names, with author[N] and arrival[N] in the list
// section's place and no list fields.
std::vector<uint8_t> asPreviousFormat(const std::vector<uint8_t>& current) {
  ClixHeader from{};
  std::memcpy(&from, current.data(), sizeof(from));
  ClixHeader to = from;
  to.formatVersion = CLIX_PREVIOUS_FORMAT_VERSION;
  to.listCount = 0;
  layoutSections(to, from.folderLen, builtinListEntryBytes(from.bookCount), from.nameLen);
  to.listLen = 0;
  std::vector<uint8_t> bytes(to.selfSize, 0);
  std::memcpy(bytes.data(), &to, sizeof(to));
  std::copy_n(current.begin() + from.folderStart, from.listStart - from.folderStart, bytes.begin() + to.folderStart);
  std::copy_n(current.begin() + from.nameStart, from.nameLen, bytes.begin() + to.nameStart);
  return bytes;
}

uint16_t firstSeenOf(LibraryIndexFile& index, const std::string& path) {
  for (uint16_t ordinal = 0; ordinal < index.bookCount(); ordinal++) {
    ClixRecord record{};
    std::string recordPath;
    if (index.readRecord(ordinal, record) && index.readPath(record, recordPath) && recordPath == path) {
      return record.firstSeen;
    }
  }
  return 0xFFFF;
}

uint16_t ordinalOf(LibraryIndexFile& index, const std::string& path) {
  for (uint16_t ordinal = 0; ordinal < index.bookCount(); ordinal++) {
    ClixRecord record{};
    std::string recordPath;
    if (index.readRecord(ordinal, record) && index.readPath(record, recordPath) && recordPath == path) return ordinal;
  }
  return 0xFFFF;
}

// What an external tool adds. Books entries are paths; Groups entries are list
// ids.
struct ExternalList {
  uint8_t kind;
  uint8_t flags;
  std::string label;
  std::vector<std::string> books;
  std::vector<uint16_t> children;
  uint8_t icon = CLIX_ICON_DEFAULT;
};

// The live index with `lists` appended after the built-in ones, written the way
// an external tool would: same folders, records and names, a longer list
// section.
std::vector<uint8_t> withExternalLists(const std::vector<ExternalList>& lists) {
  const std::vector<uint8_t>& current = fake::files["/.crosspoint/library.idx"]->bytes;
  LibraryIndexFile index;
  EXPECT_TRUE(index.open("/.crosspoint/library.idx"));
  ClixHeader from{};
  std::memcpy(&from, current.data(), sizeof(from));
  const uint16_t n = from.bookCount;
  const uint16_t listCount = static_cast<uint16_t>(CLIX_BUILTIN_LISTS + lists.size());

  std::vector<uint16_t> builtinEntries(2u * n);
  ClixListDesc builtins[CLIX_BUILTIN_LISTS];
  std::memcpy(builtins, current.data() + from.listStart, sizeof(builtins));
  std::memcpy(builtinEntries.data(), current.data() + from.listStart + builtins[CLIX_RECENT_LIST].entriesOff,
              n * sizeof(uint16_t));
  std::memcpy(builtinEntries.data() + n, current.data() + from.listStart + builtins[CLIX_AUTHOR_LIST].entriesOff,
              n * sizeof(uint16_t));

  std::vector<ClixListDesc> table(listCount);
  builtinListDescs(table.data(), n, listCount);
  std::vector<uint16_t> entries;
  std::string labels;
  const uint32_t entriesStart = listCount * sizeof(ClixListDesc) + builtinListEntryBytes(n);
  for (size_t i = 0; i < lists.size(); i++) {
    std::vector<uint16_t> values = lists[i].children;
    for (const auto& path : lists[i].books) values.push_back(ordinalOf(index, path));
    table[CLIX_BUILTIN_LISTS + i] = {lists[i].kind,
                                     CLIX_ROLE_EXTERNAL,
                                     lists[i].flags,
                                     static_cast<uint8_t>(lists[i].label.size()),
                                     static_cast<uint16_t>(values.size()),
                                     lists[i].icon,
                                     0,
                                     static_cast<uint32_t>(entriesStart + entries.size() * sizeof(uint16_t)),
                                     static_cast<uint32_t>(labels.size())};
    entries.insert(entries.end(), values.begin(), values.end());
    labels += lists[i].label;
  }
  const uint32_t labelsStart = entriesStart + static_cast<uint32_t>(entries.size() * sizeof(uint16_t));
  for (size_t i = 0; i < lists.size(); i++) table[CLIX_BUILTIN_LISTS + i].labelOff += labelsStart;

  ClixHeader to = from;
  to.listCount = listCount;
  layoutSections(to, from.folderLen, labelsStart + static_cast<uint32_t>(labels.size()), from.nameLen);
  std::vector<uint8_t> bytes(to.selfSize, 0);
  std::memcpy(bytes.data(), &to, sizeof(to));
  std::copy_n(current.begin() + from.folderStart, from.listStart - from.folderStart, bytes.begin() + to.folderStart);
  uint8_t* section = bytes.data() + to.listStart;
  std::memcpy(section, table.data(), table.size() * sizeof(ClixListDesc));
  std::memcpy(section + table[CLIX_RECENT_LIST].entriesOff, builtinEntries.data(), builtinEntries.size() * 2);
  std::memcpy(section + entriesStart, entries.data(), entries.size() * sizeof(uint16_t));
  std::memcpy(section + labelsStart, labels.data(), labels.size());
  std::copy_n(current.begin() + from.nameStart, from.nameLen, bytes.begin() + to.nameStart);
  return bytes;
}

// A Books list's paths, or a Groups list's child ids as strings.
std::vector<std::string> listContents(LibraryIndexFile& index, const uint16_t id, std::string* label = nullptr) {
  std::vector<std::string> out;
  ClixListDesc list{};
  if (!index.readList(id, list)) return {"<invalid>"};
  if (label) index.readListLabel(list, *label);
  for (uint16_t row = 0; row < list.entryCount; row++) {
    const uint16_t entry = index.entryAt(id, list, row, false);
    if (list.kind == CLIX_LIST_GROUPS) {
      out.push_back(std::to_string(entry));
      continue;
    }
    if (list.kind == CLIX_LIST_MIXED && entry != 0xFFFF && (entry & CLIX_ENTRY_LIST_BIT) != 0) {
      out.push_back("list " + std::to_string(entry & ~CLIX_ENTRY_LIST_BIT));
      continue;
    }
    ClixRecord record{};
    std::string path;
    out.push_back(index.readRecord(entry, record) && index.readPath(record, path) ? path : "<bad>");
  }
  return out;
}

class LibraryBuilderTest : public ::testing::Test {
 protected:
  BuildStats stats;

  void SetUp() override {
    fake::reset();
    bookMetadata.clear();
    fake::add("/a.epub");
    fake::add("/b.epub");
  }

  void initial() { ASSERT_TRUE(buildLibraryIndex("/", stats, true)); }
};

}  // namespace

TEST_F(LibraryBuilderTest, UnchangedRebuildReusesMetadataAndDoesNotReplaceIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 0u);
  EXPECT_EQ(stats.parsed, 0);
  EXPECT_EQ(stats.metadataReused, 2);
  EXPECT_FALSE(stats.indexReplaced);
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, PreviousFormatIndexIsRewrittenKeepingArrivalHistoryAndMetadata) {
  initial();
  fake::files[INDEX]->bytes = asPreviousFormat(fake::files[INDEX]->bytes);
  fake::parses = 0;

  // Nothing changed on the card, but the index still has to move to the
  // current format.
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_TRUE(stats.indexReplaced);
  EXPECT_EQ(fake::parses, 0u);
  EXPECT_EQ(stats.metadataReused, 2);

  fake::files[INDEX]->bytes = asPreviousFormat(fake::files[INDEX]->bytes);
  fake::add("/c.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.unchanged, 2);
  EXPECT_EQ(stats.added, 1);
  EXPECT_EQ(fake::parses, 1u);

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.header().formatVersion, CLIX_FORMAT_VERSION);
  EXPECT_EQ(firstSeenOf(index, "/a.epub"), 0);
  EXPECT_EQ(firstSeenOf(index, "/b.epub"), 1);
  EXPECT_EQ(firstSeenOf(index, "/c.epub"), 2);
}

TEST_F(LibraryBuilderTest, FolderHeavyUnchangedReconciliationIoScalesLinearly) {
  const auto measure = [this](const unsigned count) {
    fake::reset();
    bookMetadata.clear();
    for (unsigned i = 0; i < count; i++) {
      fake::add("/folder" + numbered("", i) + "/book.txt");
    }
    if (!buildLibraryIndex("/", stats, false)) {
      ADD_FAILURE() << "initial build failed for " << count << " books";
      return 0u;
    }
    fake::resetIoCounters();
    if (!buildLibraryIndex("/", stats, false)) {
      ADD_FAILURE() << "unchanged build failed for " << count << " books";
      return 0u;
    }
    EXPECT_EQ(stats.metadataReused, count);
    EXPECT_FALSE(stats.indexReplaced);
    return fake::reads + fake::seeks;
  };

  const unsigned smallIo = measure(128);
  const unsigned largeIo = measure(256);
  EXPECT_LT(largeIo, smallIo * 3u);
}

TEST_F(LibraryBuilderTest, DirectoryEntriesAreEnumeratedOnce) {
  fake::add("/folder/c.txt");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));

  EXPECT_EQ(fake::directoryEntriesByPath["/a.epub"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/b.epub"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/folder"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/folder/c.txt"], 1u);
}

TEST_F(LibraryBuilderTest, DirectoryResumeFailureRetainsPreviousIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;
  fake::add("/aa-folder/c.txt");
  fake::failDirectorySeek = true;

  EXPECT_FALSE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, StagingAndIndexWritesAreBatched) {
  fake::reset();
  for (unsigned i = 0; i < 128; i++) fake::add("/book" + numbered("", i) + ".txt");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));

  EXPECT_LT(fake::writesByPath["/.crosspoint/library.stage"], 64u);
  EXPECT_LT(fake::writesByPath["/.crosspoint/library.new"], 32u);
}

TEST_F(LibraryBuilderTest, ParentDuplicateTrackingSurvivesDirectoryRecursion) {
  fake::add("/folder/c.txt");
  fake::duplicateDirectoryEntry("/a.epub");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));

  EXPECT_EQ(stats.books, 3);
  EXPECT_EQ(stats.duplicatesDropped, 1);
}

TEST_F(LibraryBuilderTest, TimestampAndSizeChangesParseOnlyTheChangedBook) {
  initial();
  fake::files["/a.epub"]->time++;
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 1);

  fake::files["/b.epub"]->bytes.push_back('x');
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 1);
}

TEST_F(LibraryBuilderTest, ZeroTimestampAndFailedExtractionAreNeverFresh) {
  fake::files["/a.epub"]->time = 0;
  bookMetadata["/b.epub"].success = false;
  initial();
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 2u);
  EXPECT_EQ(stats.metadataReused, 0);
  EXPECT_TRUE(stats.indexReplaced);
}

TEST_F(LibraryBuilderTest, MetadataModeChangesInvalidateCachedMetadata) {
  initial();
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(fake::parses, 0u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.header().metadataEnabled, 0);
  index.close();

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 2u);
}

TEST_F(LibraryBuilderTest, RebuildVotesFromSourceAuthorInsteadOfPriorCanonicalAuthor) {
  fake::add("/c.epub");
  bookMetadata["/a.epub"].author = "Victor Hugo";
  bookMetadata["/b.epub"].author = "Hugo Victor";
  bookMetadata["/c.epub"].author = "Hugo Victor";
  initial();
  ASSERT_TRUE(Storage.remove("/b.epub"));
  ASSERT_TRUE(Storage.remove("/c.epub"));
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 0u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ClixRecord record{};
  std::string author;
  ASSERT_TRUE(index.readRecord(0, record));
  ASSERT_TRUE(index.readAuthor(record, author));
  EXPECT_EQ(author, "Victor Hugo");
}

TEST_F(LibraryBuilderTest, EqualBasenamesInDifferentFoldersReconcileIndependently) {
  fake::add("/one/same.epub");
  fake::add("/two/same.epub");
  bookMetadata["/one/same.epub"].title = "One";
  bookMetadata["/two/same.epub"].title = "Two";
  initial();
  fake::files["/two/same.epub"]->time++;
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 3);
}

TEST_F(LibraryBuilderTest, ArrivalOrderFollowsModificationTimeOverDiscoveryOrder) {
  // a and b exist with the default time; c lands with an older timestamp and d
  // with the newest, so file times, not walk or firstSeen order, decide.
  fake::add("/c.epub", "book c", /*time=*/0);
  fake::add("/d.epub", "book d", /*time=*/9);
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/c.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 3), "/d.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentDesc, 0), "/d.epub");
}

TEST_F(LibraryBuilderTest, AddedRemovedMovedAndRenamedBooksKeepArrivalOrder) {
  initial();
  fake::add("/c.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/c.epub");
  index.close();

  ASSERT_TRUE(Storage.remove("/b.epub"));
  ASSERT_TRUE(Storage.rename("/a.epub", "/moved.epub"));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.removed, 1);
  EXPECT_EQ(stats.renamed, 1);
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/moved.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/c.epub");
  index.close();

  ASSERT_TRUE(Storage.rename("/moved.epub", "/renamed.epub"));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.renamed, 1);
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/renamed.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/c.epub");
}

TEST_F(LibraryBuilderTest, WholeFolderRenameWithUniqueSizePreservesArrivalOrder) {
  fake::add("/old/unique.epub", "a uniquely sized book");
  initial();
  ASSERT_TRUE(Storage.mkdir("/new"));
  ASSERT_TRUE(Storage.rename("/old/unique.epub", "/new/unique.epub"));
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(stats.renamed, 1);
  EXPECT_EQ(stats.removed, 0);
  EXPECT_EQ(fake::parses, 1u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/new/unique.epub");
}

TEST_F(LibraryBuilderTest, DuplicateDetectionRemainsBoundedAndFindsTrackedKeysAfterTheCap) {
  fake::duplicateDirectoryEntry("/a.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.books, 2);
  EXPECT_EQ(stats.duplicatesDropped, 1);
  EXPECT_FALSE(stats.dedupDegraded);

  fake::reset();
  bookMetadata.clear();
  for (unsigned i = 0; i <= LIBRARY_MAX_DEDUP_KEYS; i++) {
    fake::add("/book" + numbered("", i) + ".txt");
  }
  fake::duplicateDirectoryEntry("/book0000.txt");
  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(stats.books, LIBRARY_MAX_DEDUP_KEYS + 1);
  EXPECT_EQ(stats.duplicatesDropped, 1);
  EXPECT_TRUE(stats.dedupDegraded);
  EXPECT_LT(fake::delays, 2000u);
}

TEST_F(LibraryBuilderTest, ReadWriteCloseAndAllocationFailuresRetainPreviousIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;

  fake::failRead = 0;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failRead = -1;

  fake::files["/a.epub"]->time++;
  fake::failWrite = 0;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failWrite = -1;

  fake::failWritePath = "/.crosspoint/library.new";
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);

  fake::failClosePath = "/.crosspoint/library.new";
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);

  fake::failAlloc = 3;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failAlloc = -1;

  fake::failRename = 1;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, TruncatedPersistedPathHashAbortsAndRetainsTheLiveIndex) {
  initial();
  auto& bytes = fake::files[INDEX]->bytes;
  ClixHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  ClixRecord record{};
  std::memcpy(&record, bytes.data() + recordOffset(header, 0), sizeof(record));
  record.nameOff = header.nameLen - 4;
  std::memcpy(bytes.data() + recordOffset(header, 0), &record, sizeof(record));
  const auto corrupted = bytes;

  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, corrupted);
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.stage"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.stage.f"));
}

TEST_F(LibraryBuilderTest, LibrariesPastOldGateAndAtFormatCeilingKeepAllOrders) {
  for (const unsigned count : {513u, static_cast<unsigned>(CLIX_MAX_RECORDS)}) {
    fake::reset();
    bookMetadata.clear();
    std::vector<unsigned> authorOrder(count);
    std::iota(authorOrder.begin(), authorOrder.end(), 0u);
    for (unsigned i = 0; i < count; i++) {
      const std::string path = "/book" + numbered("", i) + ".epub";
      fake::add(path);
      bookMetadata[path].title = numbered("Title ", count - 1 - i);
      bookMetadata[path].author = numbered("Writer ", (i * (count == 513 ? 257u : 2053u)) % count);
    }

    ASSERT_TRUE(buildLibraryIndex("/", stats, true)) << count;
    ASSERT_EQ(stats.books, count);
    EXPECT_FALSE(stats.ranksDegraded);

    if (count == CLIX_MAX_RECORDS) {
      const auto old = fake::files[INDEX]->bytes;
      fake::parses = 0;
      fake::resetIoCounters();
      ASSERT_TRUE(buildLibraryIndex("/", stats, true));
      EXPECT_EQ(fake::parses, 0u);
      EXPECT_EQ(stats.metadataReused, CLIX_MAX_RECORDS);
      EXPECT_FALSE(stats.indexReplaced);
      EXPECT_EQ(fake::files[INDEX]->bytes, old);
      EXPECT_LT(fake::delays, 10000u);
    }

    std::sort(authorOrder.begin(), authorOrder.end(), [count](const unsigned a, const unsigned b) {
      return (a * (count == 513 ? 257u : 2053u)) % count < (b * (count == 513 ? 257u : 2053u)) % count;
    });
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    for (uint16_t row = 0; row < count; row++) {
      EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, row), "/book" + numbered("", row) + ".epub") << count << ':' << row;
      EXPECT_EQ(pathAt(index, SortOrder::TitleAsc, row), "/book" + numbered("", count - 1 - row) + ".epub")
          << count << ':' << row;
      EXPECT_EQ(pathAt(index, SortOrder::AuthorAsc, row), "/book" + numbered("", authorOrder[row]) + ".epub")
          << count << ':' << row;
    }
  }
}

TEST_F(LibraryBuilderTest, SortAllocationFailureProducesValidDegradedIndex) {
  fake::reset();
  for (unsigned i = 0; i < 513; i++) fake::add("/book" + numbered("", i) + ".txt");
  fake::failAlloc = 6;

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_TRUE(fake::failureTriggered);
  EXPECT_TRUE(stats.ranksDegraded);
  EXPECT_TRUE(stats.indexReplaced);

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.bookCount(), 513);
}

TEST_F(LibraryBuilderTest, TitleSortOrdersRecordsWhileBlobKeepsShownTitle) {
  bookMetadata["/a.epub"].title = "The Hobbit";
  bookMetadata["/a.epub"].titleSort = "Hobbit, The";
  bookMetadata["/b.epub"].title = "Middlemarch";
  initial();

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::TitleAsc, 0), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::TitleAsc, 1), "/b.epub");

  ClixRecord record{};
  ASSERT_TRUE(index.readRecord(0, record));
  EXPECT_EQ(std::string(record.fold, record.foldLen), "hobbit the");
  std::string author;
  std::string title;
  ASSERT_TRUE(index.readAuthorAndTitle(record, author, title));
  EXPECT_EQ(title, "The Hobbit");
  EXPECT_EQ(author, "Author");
}

TEST_F(LibraryBuilderTest, AuthorFileAsOrdersAuthorShelfOverSurnameGuess) {
  // The last-word guess files Lu Xun under X, after Morrison.
  bookMetadata["/a.epub"].author = "Lu Xun";
  bookMetadata["/a.epub"].authorSort = "Lu, Xun";
  bookMetadata["/b.epub"].author = "Toni Morrison";
  initial();

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::AuthorAsc, 0), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::AuthorAsc, 1), "/b.epub");
}

TEST_F(LibraryBuilderTest, AuthorGroupWithPartialFileAsStaysWhole) {
  fake::add("/c.epub");
  bookMetadata["/a.epub"].author = "Lu Xun";
  bookMetadata["/b.epub"].author = "Toni Morrison";
  bookMetadata["/c.epub"].author = "Lu Xun";
  bookMetadata["/c.epub"].authorSort = "Lu, Xun";
  initial();

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  const std::string first = pathAt(index, SortOrder::AuthorAsc, 0);
  const std::string second = pathAt(index, SortOrder::AuthorAsc, 1);
  EXPECT_TRUE((first == "/a.epub" && second == "/c.epub") || (first == "/c.epub" && second == "/a.epub"));
  EXPECT_EQ(pathAt(index, SortOrder::AuthorAsc, 2), "/b.epub");
}

TEST_F(LibraryBuilderTest, ReusedRecordsKeepSortKeysWithoutParsing) {
  bookMetadata["/a.epub"].title = "The Hobbit";
  bookMetadata["/a.epub"].titleSort = "Hobbit, The";
  bookMetadata["/a.epub"].author = "Lu Xun";
  bookMetadata["/a.epub"].authorSort = "Lu, Xun";
  bookMetadata["/b.epub"].title = "Middlemarch";
  bookMetadata["/b.epub"].author = "Toni Morrison";
  initial();
  // Changing /b.epub forces a new index while /a.epub is reused from the old one.
  fake::files["/b.epub"]->time++;
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 1);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::TitleAsc, 0), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::AuthorAsc, 0), "/a.epub");
  ClixRecord record{};
  std::string authorSort;
  ASSERT_TRUE(index.readRecord(0, record));
  ASSERT_TRUE(index.readAuthorSort(record, authorSort));
  EXPECT_EQ(authorSort, "Lu, Xun");

  RebuildFields fields;
  ASSERT_TRUE(index.readRebuildFields(record, fields));
  EXPECT_EQ(fields.title, "The Hobbit");
  EXPECT_EQ(fields.sourceAuthor, "Lu Xun");
  EXPECT_EQ(fields.authorSort, "Lu, Xun");
}

TEST_F(LibraryBuilderTest, SearchMatchesTitleSortShownTitleAndAuthorInDisplayOrder) {
  fake::add("/c.epub");
  bookMetadata["/a.epub"].title = "The Hobbit";
  bookMetadata["/a.epub"].titleSort = "Hobbit, The";
  bookMetadata["/a.epub"].author = "J. R. R. Tolkien";
  bookMetadata["/b.epub"].title = "Middlemarch";
  bookMetadata["/b.epub"].author = "George Eliot";
  bookMetadata["/c.epub"].title = "The Silmarillion";
  bookMetadata["/c.epub"].author = "J. R. R. Tolkien";
  initial();

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  uint16_t rows[3] = {};
  const auto paths = [&](const SortOrder order, const uint16_t count) {
    std::vector<std::string> out;
    for (uint16_t i = 0; i < count; i++) out.push_back(pathAt(index, order, rows[i]));
    return out;
  };

  // Stored fold ("hobbit the") and shown title ("the hobbit") both reach it.
  EXPECT_EQ(paths(SortOrder::TitleAsc, filterRows(index, SortOrder::TitleAsc, "hob", rows)),
            std::vector<std::string>{"/a.epub"});
  EXPECT_EQ(paths(SortOrder::TitleAsc, filterRows(index, SortOrder::TitleAsc, "The Hob", rows)),
            std::vector<std::string>{"/a.epub"});
  // Author match, returned in the order's display order.
  EXPECT_EQ(paths(SortOrder::TitleAsc, filterRows(index, SortOrder::TitleAsc, "tolkien", rows)),
            (std::vector<std::string>{"/a.epub", "/c.epub"}));
  EXPECT_EQ(filterRows(index, SortOrder::AuthorAsc, "zqxj", rows), 0);
  EXPECT_EQ(filterRows(index, SortOrder::RecentDesc, "", rows), 3);
}

TEST_F(LibraryBuilderTest, BlobFieldsLongerThanTheFirstReadChunkAreReadWhole) {
  const std::string longTitle(200, 't');
  const std::string longAuthor = "Author " + std::string(100, 'a');
  const std::string longSort = std::string(100, 'a') + ", Author";
  bookMetadata["/a.epub"].title = longTitle;
  bookMetadata["/a.epub"].author = longAuthor;
  bookMetadata["/a.epub"].authorSort = longSort;
  bookMetadata["/b.epub"].title = "Zzz";
  initial();

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ClixRecord record{};
  ASSERT_TRUE(index.readRecord(0, record));
  RebuildFields fields;
  ASSERT_TRUE(index.readRebuildFields(record, fields));
  EXPECT_EQ(fields.title, longTitle);
  EXPECT_EQ(fields.sourceAuthor, longAuthor);
  EXPECT_EQ(fields.authorSort, longSort);

  std::string title;
  std::string author;
  ASSERT_TRUE(index.readAuthorAndTitle(record, author, title));
  EXPECT_EQ(author, longAuthor);
  EXPECT_EQ(title, longTitle);
}

TEST_F(LibraryBuilderTest, ExternalListsLoseRemovedBooksFollowRenamesAndDropEmptiedGroups) {
  fake::reset();
  // Distinct sizes, so the rename below is recognised by size.
  fake::add("/a.epub", "a");
  fake::add("/b.epub", "bb");
  fake::add("/c.epub", "ccc");
  fake::add("/d.epub", "dddd");
  bookMetadata["/a.epub"].title = "Alpha";
  bookMetadata["/b.epub"].title = "Bravo";
  bookMetadata["/c.epub"].title = "Charlie";
  bookMetadata["/d.epub"].title = "Delta";
  initial();
  fake::files[INDEX]->bytes = withExternalLists({
      {CLIX_LIST_GROUPS, CLIX_LIST_TOP_LEVEL, "Tags", {}, {4, 5}},
      {CLIX_LIST_BOOKS, 0, "Fiction", {"/d.epub", "/a.epub", "/c.epub"}, {}},
      {CLIX_LIST_BOOKS, 0, "Poetry", {"/b.epub"}, {}},
  });

  ASSERT_TRUE(Storage.remove("/b.epub"));
  ASSERT_TRUE(Storage.rename("/c.epub", "/z.epub"));
  bookMetadata["/z.epub"].title = "Zulu";  // moves the renamed book to the last record
  fake::add("/e.epub", "eeeee");
  bookMetadata["/e.epub"].title = "Echo";
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_TRUE(stats.indexReplaced);
  EXPECT_FALSE(stats.listsDropped);
  EXPECT_EQ(stats.renamed, 1);
  EXPECT_EQ(stats.removed, 1);

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ASSERT_EQ(index.listCount(), 6);
  EXPECT_EQ(index.header().flags & CLIX_FLAG_LISTS_DROPPED, 0);
  std::string label;
  EXPECT_EQ(listContents(index, 3, &label), std::vector<std::string>{"4"}) << "Poetry lost its only book";
  EXPECT_EQ(label, "Tags");
  EXPECT_EQ(listContents(index, 4, &label), (std::vector<std::string>{"/d.epub", "/a.epub", "/z.epub"}));
  EXPECT_EQ(label, "Fiction");
  EXPECT_TRUE(listContents(index, 5, &label).empty());
  EXPECT_EQ(label, "Poetry");
  ClixListDesc tags{};
  ASSERT_TRUE(index.readList(3, tags));
  EXPECT_NE(tags.flags & CLIX_LIST_TOP_LEVEL, 0);
  // The built-in lists still name every book, the new one included.
  EXPECT_EQ(pathAt(index, SortOrder::TitleAsc, 3), "/z.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentDesc, 0), "/e.epub");
}

TEST_F(LibraryBuilderTest, UnchangedRebuildKeepsExternalListsByteForByte) {
  bookMetadata["/a.epub"].title = "Alpha";
  bookMetadata["/b.epub"].title = "Bravo";
  initial();
  fake::files[INDEX]->bytes =
      withExternalLists({{CLIX_LIST_BOOKS, CLIX_LIST_TOP_LEVEL, "Favourites", {"/b.epub"}, {}}});
  const auto withLists = fake::files[INDEX]->bytes;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_FALSE(stats.indexReplaced);
  EXPECT_EQ(fake::files[INDEX]->bytes, withLists);

  fake::add("/c.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_TRUE(stats.indexReplaced);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(listContents(index, 3), std::vector<std::string>{"/b.epub"});
}

TEST_F(LibraryBuilderTest, MalformedExternalListIsCarriedBackEmpty) {
  bookMetadata["/a.epub"].title = "Alpha";
  bookMetadata["/b.epub"].title = "Bravo";
  initial();
  auto bytes = withExternalLists({{CLIX_LIST_BOOKS, CLIX_LIST_TOP_LEVEL, "Broken", {"/a.epub"}, {}},
                                  {CLIX_LIST_BOOKS, CLIX_LIST_TOP_LEVEL, "Kept", {"/b.epub"}, {}}});
  ClixHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  const uint16_t overrun = 0xFFFF;
  std::memcpy(bytes.data() + header.listStart + 3 * sizeof(ClixListDesc) + offsetof(ClixListDesc, entryCount), &overrun,
              sizeof(overrun));
  fake::files[INDEX]->bytes = bytes;
  fake::add("/c.epub");

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_FALSE(stats.listsDropped);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ASSERT_EQ(index.listCount(), 5);
  EXPECT_TRUE(listContents(index, 3).empty());
  EXPECT_EQ(listContents(index, 4), std::vector<std::string>{"/b.epub"});
}

TEST_F(LibraryBuilderTest, ExternalListsThatCannotBeCarriedAreDroppedWithoutFailingTheBuild) {
  initial();
  const auto withLists = withExternalLists({{CLIX_LIST_BOOKS, CLIX_LIST_TOP_LEVEL, "Favourites", {"/a.epub"}, {}}});

  // Fail each allocation in turn: the build either fails and keeps the old
  // index, succeeds with the lists, or succeeds without them and says so.
  bool dropped = false;
  for (int k = 0; k < 64 && !dropped; k++) {
    fake::files[INDEX]->bytes = withLists;
    fake::files["/a.epub"]->time = 10 + k;  // a real rebuild every time
    fake::failureTriggered = false;
    fake::failAlloc = k;
    const bool ok = buildLibraryIndex("/", stats, true);
    fake::failAlloc = -1;
    if (!fake::failureTriggered) break;
    if (!ok) {
      EXPECT_EQ(fake::files[INDEX]->bytes, withLists) << k;
      continue;
    }
    if (!stats.listsDropped) continue;
    dropped = true;
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX)) << k;
    EXPECT_EQ(index.listCount(), CLIX_BUILTIN_LISTS);
    EXPECT_NE(index.header().flags & CLIX_FLAG_LISTS_DROPPED, 0);
    EXPECT_EQ(index.bookCount(), 2);
  }
  EXPECT_TRUE(dropped);
}

TEST_F(LibraryBuilderTest, SeriesAndTagsAreKeptForUnchangedBooks) {
  bookMetadata["/a.epub"] = {"Alpha", "A", "", "", "Saga", "1.5", "Fiction\nPoetry"};
  initial();
  fake::add("/c.epub");
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 1u) << "only the new book is parsed";

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ClixRecord record{};
  ASSERT_TRUE(index.readRecord(ordinalOf(index, "/a.epub"), record));
  RebuildFields fields;
  ASSERT_TRUE(index.readRebuildFields(record, fields));
  EXPECT_EQ(fields.series, "Saga");
  EXPECT_EQ(fields.seriesIndex, "1.5");
  EXPECT_EQ(fields.tags, "Fiction\nPoetry");
}

TEST_F(LibraryBuilderTest, SeriesListsAreAlphabeticalAndInSeriesOrder) {
  fake::add("/c.epub");
  fake::add("/d.epub");
  bookMetadata["/a.epub"] = {"Alpha", "Asimov", "", "", "Foundation", "2", ""};
  bookMetadata["/b.epub"] = {"Bravo", "Asimov", "", "", "foundation", "1", ""};
  bookMetadata["/c.epub"] = {"Charlie", "Herbert", "", "", "Dune", "1", ""};
  bookMetadata["/d.epub"] = {"Delta", "Nobody", "", "", "", "", ""};
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, CLIX_OPTIONS_DEFAULT | CLIX_OPTION_SERIES));
  EXPECT_FALSE(stats.listsIncomplete);

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.header().listOptions, CLIX_OPTIONS_DEFAULT | CLIX_OPTION_SERIES);
  ASSERT_EQ(index.listCount(), 6);
  ClixListDesc series{};
  ASSERT_TRUE(index.readList(3, series));
  EXPECT_EQ(series.role, CLIX_ROLE_SERIES);
  EXPECT_EQ(series.icon, CLIX_ICON_SERIES);
  EXPECT_NE(series.flags & CLIX_LIST_TOP_LEVEL, 0);
  ClixListDesc oneSeries{};
  ASSERT_TRUE(index.readList(4, oneSeries));
  EXPECT_EQ(oneSeries.icon, CLIX_ICON_SERIES_ENTRY);
  EXPECT_EQ(listContents(index, 3), (std::vector<std::string>{"4", "5"}));
  std::string label;
  EXPECT_EQ(listContents(index, 4, &label), std::vector<std::string>{"/c.epub"});
  EXPECT_EQ(label, "Dune");
  // One series whatever the case, in series order rather than title order.
  EXPECT_EQ(listContents(index, 5, &label), (std::vector<std::string>{"/b.epub", "/a.epub"}));
  EXPECT_EQ(label, "Foundation");
}

TEST_F(LibraryBuilderTest, TagListsMergeSpellingsAndListBooksInTitleOrder) {
  fake::add("/c.epub");
  bookMetadata["/a.epub"] = {"Alpha", "A", "", "", "", "", "Fiction\nFantasy"};
  bookMetadata["/b.epub"] = {"Bravo", "B", "", "", "", "", "fiction"};
  bookMetadata["/c.epub"] = {"Charlie", "C", "", "", "", "", "Poetry"};
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, CLIX_OPTIONS_DEFAULT | CLIX_OPTION_TAGS));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ASSERT_EQ(index.listCount(), 7);
  EXPECT_EQ(listContents(index, 3), (std::vector<std::string>{"4", "5", "6"}));
  std::string label;
  EXPECT_EQ(listContents(index, 4, &label), std::vector<std::string>{"/a.epub"});
  EXPECT_EQ(label, "Fantasy");
  EXPECT_EQ(listContents(index, 5, &label), (std::vector<std::string>{"/a.epub", "/b.epub"}));
  EXPECT_EQ(label, "Fiction");
  EXPECT_EQ(listContents(index, 6, &label), std::vector<std::string>{"/c.epub"});
  EXPECT_EQ(label, "Poetry");
  ClixListDesc list{};
  ASSERT_TRUE(index.readList(3, list));
  EXPECT_EQ(list.icon, CLIX_ICON_TAGS);
  ASSERT_TRUE(index.readList(6, list));
  EXPECT_EQ(list.icon, CLIX_ICON_TAG);
}

TEST_F(LibraryBuilderTest, OnlyTheMostUsedTagsGetLists) {
  fake::reset();
  bookMetadata.clear();
  // Tags 0..MAX_TAGS-1 are on two books each; two more are on one book each.
  unsigned book = 0;
  const auto addBook = [&book](const std::string& tag) {
    const std::string path = "/book" + numbered("", book++) + ".epub";
    fake::add(path);
    bookMetadata[path].title = path;
    bookMetadata[path].tags = tag;
  };
  for (unsigned t = 0; t < MAX_TAGS; t++) {
    addBook(numbered("tag", t));
    addBook(numbered("tag", t));
  }
  addBook("rare1");
  addBook("rare2");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, CLIX_OPTIONS_DEFAULT | CLIX_OPTION_TAGS));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ASSERT_EQ(index.listCount(), CLIX_BUILTIN_LISTS + 1 + MAX_TAGS);
  std::string label;
  listContents(index, 4, &label);
  EXPECT_EQ(label, "tag0000");
  listContents(index, CLIX_BUILTIN_LISTS + MAX_TAGS, &label);
  EXPECT_EQ(label, numbered("tag", MAX_TAGS - 1));
}

TEST_F(LibraryBuilderTest, FolderListMirrorsTheCardSubfoldersFirst) {
  fake::add("/Books/c.epub");
  fake::add("/Books/Author/d.epub");
  fake::add("/Zed/e.epub");
  for (const char* path : {"/a.epub", "/b.epub", "/Books/c.epub", "/Books/Author/d.epub", "/Zed/e.epub"}) {
    bookMetadata[path].title = path;
  }
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, CLIX_OPTIONS_DEFAULT | CLIX_OPTION_FOLDERS));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ASSERT_EQ(index.listCount(), 7);
  ClixListDesc root{};
  ASSERT_TRUE(index.readList(3, root));
  EXPECT_EQ(root.kind, CLIX_LIST_MIXED);
  EXPECT_EQ(root.role, CLIX_ROLE_FOLDERS);
  EXPECT_EQ(root.icon, CLIX_ICON_FOLDER_TREE);
  ClixListDesc folder{};
  ASSERT_TRUE(index.readList(6, folder));
  EXPECT_EQ(folder.icon, CLIX_ICON_FOLDER);
  // Breadth first: root 3, then its folders Books 4 and Zed 5, then Author 6.
  std::string label;
  EXPECT_EQ(listContents(index, 3), (std::vector<std::string>{"list 4", "list 5", "/a.epub", "/b.epub"}));
  EXPECT_EQ(listContents(index, 4, &label), (std::vector<std::string>{"list 6", "/Books/c.epub"}));
  EXPECT_EQ(label, "Books");
  EXPECT_EQ(listContents(index, 5, &label), std::vector<std::string>{"/Zed/e.epub"});
  EXPECT_EQ(label, "Zed");
  EXPECT_EQ(listContents(index, 6, &label), std::vector<std::string>{"/Books/Author/d.epub"});
  EXPECT_EQ(label, "Author");
}

TEST_F(LibraryBuilderTest, GeneratedListsThatCannotBeAllocatedAreLeftOutWithoutFailingTheBuild) {
  fake::add("/Books/c.epub");
  bookMetadata["/a.epub"] = {"Alpha", "A", "", "", "Saga", "1", "Fiction"};
  bookMetadata["/b.epub"] = {"Bravo", "B", "", "", "Saga", "2", "Poetry"};
  initial();
  const auto old = fake::files[INDEX]->bytes;

  bool leftOut = false;
  for (int k = 0; k < 96; k++) {
    fake::files[INDEX]->bytes = old;
    fake::failureTriggered = false;
    fake::failAlloc = k;
    const bool ok = buildLibraryIndex("/", stats, true, CLIX_OPTIONS_ALL);
    fake::failAlloc = -1;
    if (!fake::failureTriggered) break;
    if (!ok) {
      EXPECT_EQ(fake::files[INDEX]->bytes, old) << k;
      continue;
    }
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX)) << k;
    for (uint16_t id = 0; id < index.listCount(); id++) {
      ClixListDesc list{};
      EXPECT_TRUE(index.readList(id, list)) << k << ':' << id;
    }
    if (stats.listsIncomplete) {
      leftOut = true;
      EXPECT_NE(index.header().flags & CLIX_FLAG_LISTS_INCOMPLETE, 0);
    }
  }
  EXPECT_TRUE(leftOut);
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.gen"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.gen.e"));
}

TEST_F(LibraryBuilderTest, ListOptionsHideBuiltinListsAndRebuildWhenChanged) {
  initial();
  fake::parses = 0;
  constexpr uint8_t TITLE_ONLY = CLIX_OPTION_TITLE;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, TITLE_ONLY));
  EXPECT_TRUE(stats.indexReplaced) << "same books, other options";
  EXPECT_EQ(fake::parses, 0u);

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ClixListDesc list{};
  ASSERT_TRUE(index.readList(CLIX_RECENT_LIST, list));
  EXPECT_EQ(list.flags & CLIX_LIST_TOP_LEVEL, 0);
  ASSERT_TRUE(index.readList(CLIX_TITLE_LIST, list));
  EXPECT_NE(list.flags & CLIX_LIST_TOP_LEVEL, 0);
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/a.epub") << "Recent is still written for search and home";
  index.close();

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, TITLE_ONLY));
  EXPECT_FALSE(stats.indexReplaced);
}

TEST_F(LibraryBuilderTest, ExternalListsMoveAfterGeneratedListsWithTheirChildren) {
  bookMetadata["/a.epub"] = {"Alpha", "A", "", "", "Saga", "1", ""};
  bookMetadata["/b.epub"] = {"Bravo", "B", "", "", "", "", ""};
  initial();
  fake::files[INDEX]->bytes = withExternalLists({
      {CLIX_LIST_GROUPS, CLIX_LIST_TOP_LEVEL, "Mine", {}, {4}},
      {CLIX_LIST_BOOKS, 0, "Favourites", {"/b.epub"}, {}, CLIX_ICON_HEART},
  });

  ASSERT_TRUE(buildLibraryIndex("/", stats, true, CLIX_OPTIONS_DEFAULT | CLIX_OPTION_SERIES));
  EXPECT_FALSE(stats.listsDropped);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  // Series 3 and Saga 4, then the external lists renumbered to 5 and 6.
  ASSERT_EQ(index.listCount(), 7);
  ClixListDesc favourites{};
  ASSERT_TRUE(index.readList(6, favourites));
  EXPECT_EQ(favourites.icon, CLIX_ICON_HEART) << "an external list keeps its icon";
  std::string label;
  EXPECT_EQ(listContents(index, 4, &label), std::vector<std::string>{"/a.epub"});
  EXPECT_EQ(label, "Saga");
  EXPECT_EQ(listContents(index, 5, &label), std::vector<std::string>{"6"});
  EXPECT_EQ(label, "Mine");
  EXPECT_EQ(listContents(index, 6, &label), std::vector<std::string>{"/b.epub"});
  EXPECT_EQ(label, "Favourites");
  index.close();

  // A later build regenerates Series and carries only the external lists.
  fake::add("/c.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, CLIX_OPTIONS_DEFAULT | CLIX_OPTION_SERIES));
  ASSERT_TRUE(index.open(INDEX));
  ASSERT_EQ(index.listCount(), 7);
  EXPECT_EQ(listContents(index, 5, &label), std::vector<std::string>{"6"});
  EXPECT_EQ(label, "Mine");
}
