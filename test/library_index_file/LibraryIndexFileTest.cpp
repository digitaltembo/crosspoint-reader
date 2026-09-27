#include <gtest/gtest.h>

#include <cstring>
#include <utility>
#include <vector>

#include "LibraryIndexFile.h"

namespace library {

std::string joinLibraryPath(const std::string_view folder, const std::string_view name) {
  return std::string(folder) + "/" + std::string(name);
}

}  // namespace library

namespace {

std::vector<uint8_t> makeBlob(const uint64_t pathHash, const std::initializer_list<uint8_t> fields) {
  std::vector<uint8_t> blob(sizeof(pathHash) + fields.size());
  std::memcpy(blob.data(), &pathHash, sizeof(pathHash));
  std::copy(fields.begin(), fields.end(), blob.begin() + sizeof(pathHash));
  return blob;
}

// A valid index image for `books` records: the built-in lists (Recent and
// Author in identity order unless given) and `extraListBytes` of list section
// after them for a test's external lists. `listCount` includes those lists.
struct Image {
  library::ClixHeader header{};
  std::vector<uint8_t> bytes;

  uint8_t* list(const uint32_t offset) { return bytes.data() + header.listStart + offset; }
  // First byte of the list section after the table and the built-in entries.
  uint32_t externalStart() const {
    return header.listCount * sizeof(library::ClixListDesc) + library::builtinListEntryBytes(header.bookCount);
  }
  void setList(const uint16_t id, const library::ClixListDesc& desc) {
    std::memcpy(list(id * sizeof(desc)), &desc, sizeof(desc));
  }
  void setEntries(const uint32_t offset, const std::vector<uint16_t>& entries) {
    std::memcpy(list(offset), entries.data(), entries.size() * sizeof(uint16_t));
  }
};

Image makeImage(const uint16_t books, const uint32_t folderBytes = 0, const uint32_t nameBytes = 0,
                const std::vector<uint16_t>& recent = {}, const std::vector<uint16_t>& author = {},
                const uint16_t listCount = library::CLIX_BUILTIN_LISTS, const uint32_t extraListBytes = 0) {
  Image image;
  auto& h = image.header;
  std::memcpy(h.magic, library::CLIX_MAGIC, sizeof(h.magic));
  h.formatVersion = library::CLIX_FORMAT_VERSION;
  h.foldVersion = library::CLIX_FOLD_VERSION;
  h.bookCount = books;
  h.listCount = listCount;
  library::layoutSections(
      h, folderBytes,
      listCount * sizeof(library::ClixListDesc) + library::builtinListEntryBytes(books) + extraListBytes, nameBytes);
  image.bytes.assign(h.selfSize, 0);
  std::memcpy(image.bytes.data(), &h, sizeof(h));

  library::ClixListDesc lists[library::CLIX_BUILTIN_LISTS];
  library::builtinListDescs(lists, books, listCount);
  for (uint16_t id = 0; id < library::CLIX_BUILTIN_LISTS; id++) image.setList(id, lists[id]);
  std::vector<uint16_t> identity(books);
  for (uint16_t i = 0; i < books; i++) identity[i] = i;
  image.setEntries(lists[library::CLIX_RECENT_LIST].entriesOff, recent.empty() ? identity : recent);
  image.setEntries(lists[library::CLIX_AUTHOR_LIST].entriesOff, author.empty() ? identity : author);
  return image;
}

}  // namespace

TEST(LibraryIndexFile, MissingIndexDoesNotCloseAnUninitializedHandle) {
  Storage.clearFile();
  HalFile::resetInvalidCloseCount();

  {
    library::LibraryIndexFile index;
    EXPECT_FALSE(index.open("/missing.clx"));
  }

  EXPECT_EQ(HalFile::invalidCloseCount(), 0);
}

TEST(LibraryIndexFile, ReadsEveryStoredOrderInBothDirections) {
  Storage.setFile("/library.clx", makeImage(3, 0, 0, {1, 2, 0}, {2, 0, 1}).bytes);

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));

  const auto expectOrder = [&](const library::SortOrder order, const uint16_t a, const uint16_t b, const uint16_t c) {
    EXPECT_EQ(index.ordinalForRow(order, 0), a);
    EXPECT_EQ(index.ordinalForRow(order, 1), b);
    EXPECT_EQ(index.ordinalForRow(order, 2), c);
    EXPECT_EQ(index.ordinalForRow(order, 3), 0xFFFF);
  };
  expectOrder(library::SortOrder::RecentAsc, 1, 2, 0);
  expectOrder(library::SortOrder::RecentDesc, 0, 2, 1);
  expectOrder(library::SortOrder::TitleAsc, 0, 1, 2);
  expectOrder(library::SortOrder::TitleDesc, 2, 1, 0);
  expectOrder(library::SortOrder::AuthorAsc, 2, 0, 1);
  expectOrder(library::SortOrder::AuthorDesc, 1, 0, 2);
}

TEST(LibraryIndexFile, ResolvesRecentRowsByIdentity) {
  // One 8-byte path hash blob per record.
  Image image = makeImage(3, 0, 3 * sizeof(uint64_t), {1, 2, 0});
  const auto& header = image.header;

  // Ordinals 0 and 2 share a size, so only the hash can tell them apart.
  constexpr uint64_t HASHES[] = {11, 22, 33};
  constexpr uint32_t SIZES[] = {100, 200, 100};
  for (uint16_t ordinal = 0; ordinal < 3; ordinal++) {
    library::ClixRecord record{};
    record.fileSize = SIZES[ordinal];
    record.nameOff = ordinal * sizeof(uint64_t);
    std::memcpy(image.bytes.data() + library::recordOffset(header, ordinal), &record, sizeof(record));
    std::memcpy(image.bytes.data() + header.nameStart + record.nameOff, &HASHES[ordinal], sizeof(uint64_t));
  }
  Storage.setFile("/library.clx", std::move(image.bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  const library::BookIdentity books[] = {
      {HASHES[0], SIZES[0]},  // ordinal 0 -> ascending row 2
      {HASHES[2], SIZES[2]},  // same size as ordinal 0, hash picks ordinal 2 -> row 1
      {99, SIZES[0]},         // size matches, hash does not: absent
      {HASHES[1], 999},       // hash matches, size does not: absent
      {HASHES[1], 0},         // size unknown: the hash alone matches -> row 0
  };
  uint16_t rows[5] = {};
  ASSERT_TRUE(index.recentRowsFor(books, 5, rows));
  EXPECT_EQ(rows[0], 2);
  EXPECT_EQ(rows[1], 1);
  EXPECT_EQ(rows[2], 0xFFFF);
  EXPECT_EQ(rows[3], 0xFFFF);
  EXPECT_EQ(rows[4], 0);
}

TEST(LibraryIndexFile, RejectsInvalidPermutationOrdinal) {
  Storage.setFile("/library.clx", makeImage(1, 0, 0, {}, {1}).bytes);

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  EXPECT_EQ(index.ordinalForRow(library::SortOrder::AuthorAsc, 0), 0xFFFF);
}

TEST(LibraryIndexFile, RejectsAMalformedBuiltinList) {
  Image image = makeImage(2);
  library::ClixListDesc recent{};
  std::memcpy(&recent, image.list(0), sizeof(recent));
  recent.entryCount = 1;  // Recent must name every book
  image.setList(library::CLIX_RECENT_LIST, recent);
  Storage.setFile("/library.clx", std::move(image.bytes));

  library::LibraryIndexFile index;
  EXPECT_FALSE(index.open("/library.clx"));
  EXPECT_EQ(index.validity(), library::ClixValidity::ListsInconsistent);
  EXPECT_FALSE(index.openForReconciliation("/library.clx"));
}

TEST(LibraryIndexFile, PreviousFormatOpensOnlyForReconciliationAndHasNoLists) {
  // Version 2: author[N] then arrival[N] where the list section now starts.
  library::ClixHeader header{};
  std::memcpy(header.magic, library::CLIX_MAGIC, sizeof(header.magic));
  header.formatVersion = library::CLIX_PREVIOUS_FORMAT_VERSION;
  header.foldVersion = library::CLIX_FOLD_VERSION;
  header.bookCount = 2;
  library::layoutSections(header, 0, library::builtinListEntryBytes(2), 0);
  header.listLen = 0;
  std::vector<uint8_t> bytes(header.selfSize, 0);
  std::memcpy(bytes.data(), &header, sizeof(header));
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  EXPECT_FALSE(index.open("/library.clx"));
  EXPECT_EQ(index.validity(), library::ClixValidity::UnknownFormatVersion);
  ASSERT_TRUE(index.openForReconciliation("/library.clx"));
  EXPECT_EQ(index.bookCount(), 2);
  EXPECT_EQ(index.listCount(), 0);
  EXPECT_EQ(index.ordinalForRow(library::SortOrder::TitleAsc, 0), 0xFFFF);
  library::ClixListDesc list{};
  EXPECT_FALSE(index.readList(library::CLIX_TITLE_LIST, list));
}

TEST(LibraryIndexFile, ReadsPartialAndNestedExternalLists) {
  // List 3: top-level Groups {4, 5}. List 4: Books {2, 0}. List 5: Books {1}.
  // List 6: a Groups list pointing back at an EARLIER list, which must not be
  // followed.
  constexpr uint16_t LISTS = 7;
  Image image = makeImage(3, 0, 0, {}, {}, LISTS, 64);
  const uint32_t base = image.externalStart();
  image.setList(3, {library::CLIX_LIST_GROUPS, library::CLIX_ROLE_EXTERNAL, library::CLIX_LIST_TOP_LEVEL, 4, 2, 0, base,
                    base + 40});
  image.setList(4, {library::CLIX_LIST_BOOKS, library::CLIX_ROLE_EXTERNAL, 0, 7, 2, 0, base + 4, base + 44});
  image.setList(5, {library::CLIX_LIST_BOOKS, library::CLIX_ROLE_EXTERNAL, 0, 0, 1, 0, base + 8, 0});
  image.setList(6, {library::CLIX_LIST_GROUPS, library::CLIX_ROLE_EXTERNAL, 0, 0, 1, 0, base + 10, 0});
  image.setEntries(base, {4, 5, 2, 0, 1, 3});
  std::memcpy(image.list(base + 40), "TagsFiction", 11);
  Storage.setFile("/library.clx", std::move(image.bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  ASSERT_EQ(index.listCount(), LISTS);

  library::ClixListDesc tags{};
  ASSERT_TRUE(index.readList(3, tags));
  std::string label;
  ASSERT_TRUE(index.readListLabel(tags, label));
  EXPECT_EQ(label, "Tags");
  EXPECT_EQ(index.entryAt(3, tags, 0, false), 4);
  EXPECT_EQ(index.entryAt(3, tags, 0, true), 5);
  EXPECT_EQ(index.entryAt(3, tags, 2, false), 0xFFFF);

  library::ClixListDesc fiction{};
  ASSERT_TRUE(index.readList(4, fiction));
  ASSERT_TRUE(index.readListLabel(fiction, label));
  EXPECT_EQ(label, "Fiction");
  EXPECT_EQ(index.entryAt(4, fiction, 0, false), 2);
  EXPECT_EQ(index.entryAt(4, fiction, 1, false), 0);
  EXPECT_EQ(index.entryAt(4, fiction, 0, true), 0);

  library::ClixListDesc loop{};
  ASSERT_TRUE(index.readList(6, loop));
  EXPECT_EQ(index.entryAt(6, loop, 0, false), 0xFFFF) << "a Groups entry naming an earlier list is a cycle";

  library::ClixListDesc builtin{};
  ASSERT_TRUE(index.readList(library::CLIX_TITLE_LIST, builtin));
  ASSERT_TRUE(index.readListLabel(builtin, label));
  EXPECT_TRUE(label.empty());
  EXPECT_EQ(index.entryAt(library::CLIX_TITLE_LIST, builtin, 2, false), 2);
  EXPECT_FALSE(index.readList(LISTS, builtin));
}

TEST(LibraryIndexFile, RejectsExternalListsThatOverrunTheSection) {
  Image image = makeImage(2, 0, 0, {}, {}, 4, 8);
  const uint32_t base = image.externalStart();
  image.setList(
      3, {library::CLIX_LIST_BOOKS, library::CLIX_ROLE_EXTERNAL, library::CLIX_LIST_TOP_LEVEL, 0, 5, 0, base, 0});
  Storage.setFile("/library.clx", std::move(image.bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx")) << "only the built-in lists gate opening";
  library::ClixListDesc list{};
  EXPECT_FALSE(index.readList(3, list));
}

TEST(LibraryIndexFile, ReadsPathHashAndEveryPublicBlobField) {
  const uint8_t folder[] = {6, '/', 'b', 'o', 'o', 'k', 's'};
  constexpr uint64_t PATH_HASH = 0x0123456789ABCDEFULL;
  const auto blob = makeBlob(PATH_HASH, {'x', 1, 'a', 1, 't', 8, 'O', 'r', 'i', 'g', 'i', 'n', 'a', 'l'});
  Image image = makeImage(1, sizeof(folder), blob.size());
  auto& header = image.header;
  header.folderCount = 1;
  auto& bytes = image.bytes;
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + header.folderStart, folder, sizeof(folder));
  std::memcpy(bytes.data() + header.nameStart, blob.data(), blob.size());
  library::ClixRecord record{};
  record.nameLen = 1;
  Storage.setFile("/library.clx", bytes);

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  uint64_t pathHash = 0;
  ASSERT_TRUE(index.readPathHash(record, pathHash));
  EXPECT_EQ(pathHash, PATH_HASH);
  std::string name;
  ASSERT_TRUE(index.readName(record, name));
  EXPECT_EQ(name, "x");
  std::string author;
  ASSERT_TRUE(index.readAuthor(record, author));
  EXPECT_EQ(author, "a");
  std::string title;
  ASSERT_TRUE(index.readTitle(record, title));
  EXPECT_EQ(title, "t");
  ASSERT_TRUE(index.readSourceAuthor(record, author));
  EXPECT_EQ(author, "Original");
  std::string path;
  ASSERT_TRUE(index.readPath(record, path));
  EXPECT_EQ(path, "/books/x");
  index.close();

  bytes[header.nameStart + sizeof(PATH_HASH) + 5] = 255;
  Storage.setFile("/library.clx", std::move(bytes));
  ASSERT_TRUE(index.open("/library.clx"));
  EXPECT_FALSE(index.readSourceAuthor(record, author));
}

TEST(LibraryIndexFile, RejectsTruncatedAndOverflowingPathHashes) {
  Storage.setFile("/library.clx", makeImage(1, 0, sizeof(uint64_t) - 1).bytes);

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  library::ClixRecord record{};
  uint64_t hash = 1;
  EXPECT_FALSE(index.readPathHash(record, hash));
  EXPECT_EQ(hash, 0u);
  EXPECT_TRUE(index.ioFailed());

  record.nameOff = UINT32_MAX;
  EXPECT_FALSE(index.readPathHash(record, hash));
}

TEST(LibraryIndexFile, RejectsFolderRecordBeyondFolderBlob) {
  const uint8_t folder[] = {5, '/'};
  const auto blob = makeBlob(1, {'x', 0, 0, 0});
  Image image = makeImage(1, sizeof(folder), blob.size());
  const auto& header = image.header;
  auto& bytes = image.bytes;
  std::memcpy(bytes.data() + header.folderStart, folder, sizeof(folder));
  std::memcpy(bytes.data() + header.nameStart, blob.data(), blob.size());
  library::ClixRecord record{};
  record.nameLen = 1;
  Storage.setFile("/library.clx", std::move(bytes));

  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open("/library.clx"));
  std::string path;
  EXPECT_FALSE(index.readPath(record, path));
}
