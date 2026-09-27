#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "LibraryFormat.h"

using namespace library;

namespace {

// A header for N books whose sections are laid out consistently, i.e. one that
// validateHeader() must accept. Tests then damage exactly one thing.
uint32_t builtinListBytes(const uint16_t books) {
  return CLIX_BUILTIN_LISTS * sizeof(ClixListDesc) + builtinListEntryBytes(books);
}

ClixHeader makeHeader(const uint16_t books, const uint32_t folderBytes = 300, const uint32_t nameBytes = 0) {
  ClixHeader h{};
  memcpy(h.magic, CLIX_MAGIC, sizeof(CLIX_MAGIC));
  h.formatVersion = CLIX_FORMAT_VERSION;
  h.foldVersion = CLIX_FOLD_VERSION;
  h.bookCount = books;
  h.folderCount = 4;
  h.listCount = CLIX_BUILTIN_LISTS;
  layoutSections(h, folderBytes, builtinListBytes(books), nameBytes == 0 ? books * 80u : nameBytes);
  return h;
}

}  // namespace

TEST(LibraryFormat, StructSizesAreFrozen) {
  // These are the on-disk contract. A compiler that pads them silently would
  // produce an index this build writes and no other build can read.
  EXPECT_EQ(sizeof(ClixHeader), 64u);
  EXPECT_EQ(sizeof(ClixRecord), 128u);
  EXPECT_EQ(sizeof(ClixFolderHeader), 1u);
  EXPECT_EQ(sizeof(ClixListDesc), 16u);
  EXPECT_EQ(CLIX_FORMAT_VERSION, 3u);
}

TEST(LibraryFormat, RecordsTileSectorsExactly) {
  // The whole streaming design rests on this: 32 records fill a 4096-byte
  // buffer with nothing left over, so a scan never has to handle a record split
  // across two reads.
  EXPECT_EQ(4096u % sizeof(ClixRecord), 0u);
  EXPECT_EQ(4096u / sizeof(ClixRecord), 32u);
  EXPECT_EQ(CLIX_ALIGN % sizeof(ClixRecord), 0u);
}

TEST(LibraryFormat, EverySectionStartsOnASectorBoundary) {
  for (const uint16_t n : {uint16_t{0}, uint16_t{1}, uint16_t{3}, uint16_t{60}, uint16_t{200}, uint16_t{2000}}) {
    const ClixHeader h = makeHeader(n, 29u * 4u);
    EXPECT_EQ(h.folderStart % CLIX_ALIGN, 0u) << "n=" << n;
    EXPECT_EQ(h.recordStart % CLIX_ALIGN, 0u) << "n=" << n;
    EXPECT_EQ(h.listStart % CLIX_ALIGN, 0u) << "n=" << n;
    EXPECT_EQ(h.nameStart % CLIX_ALIGN, 0u) << "n=" << n;
  }
}

TEST(LibraryFormat, SectionsDoNotOverlap) {
  const ClixHeader h = makeHeader(200, 29u * 50u);
  EXPECT_GE(h.folderStart, sizeof(ClixHeader));
  EXPECT_GE(h.recordStart, h.folderStart + h.folderLen);
  EXPECT_GE(h.listStart, h.recordStart + 200u * sizeof(ClixRecord));
  EXPECT_GE(h.nameStart, h.listStart + h.listLen);
  EXPECT_EQ(h.selfSize, h.nameStart + h.nameLen);
}

TEST(LibraryFormat, RecordOffsetsAreAlignedAndOrdered) {
  const ClixHeader h = makeHeader(64, 116);
  EXPECT_EQ(recordOffset(h, 0), h.recordStart);
  EXPECT_EQ(recordOffset(h, 1), h.recordStart + 128u);
  EXPECT_EQ(recordOffset(h, 63), h.recordStart + 63u * 128u);
  // Every 4th record starts on a sector boundary, by construction.
  for (uint16_t k = 0; k < 64; k += 4) EXPECT_EQ(recordOffset(h, k) % CLIX_ALIGN, 0u);
}

TEST(LibraryFormat, BuiltinListsAreValidAndTheirEntriesFollowTheWholeTable) {
  const ClixHeader h = makeHeader(100, 116);
  ClixListDesc lists[CLIX_BUILTIN_LISTS];
  builtinListDescs(lists, h.bookCount, h.listCount);
  for (uint16_t id = 0; id < CLIX_BUILTIN_LISTS; id++) {
    EXPECT_TRUE(validateListDesc(h, id, lists[id])) << id;
    EXPECT_NE(lists[id].flags & CLIX_LIST_TOP_LEVEL, 0) << id;
  }
  EXPECT_EQ(lists[CLIX_RECENT_LIST].entriesOff, 48u);
  EXPECT_EQ(lists[CLIX_AUTHOR_LIST].entriesOff, 248u);
  EXPECT_EQ(lists[CLIX_AUTHOR_LIST].entriesOff + 200u, h.listLen);

  // Only the options' built-in lists are top-level; all three are still written.
  builtinListDescs(lists, h.bookCount, h.listCount, CLIX_OPTION_TITLE | CLIX_OPTION_TAGS);
  EXPECT_EQ(lists[CLIX_RECENT_LIST].flags & CLIX_LIST_TOP_LEVEL, 0);
  EXPECT_NE(lists[CLIX_TITLE_LIST].flags & CLIX_LIST_TOP_LEVEL, 0);
  EXPECT_EQ(lists[CLIX_AUTHOR_LIST].flags & CLIX_LIST_TOP_LEVEL, 0);
  for (uint16_t id = 0; id < CLIX_BUILTIN_LISTS; id++) EXPECT_TRUE(validateListDesc(h, id, lists[id])) << id;

  // With external lists the table grows and the built-in entries move past it.
  builtinListDescs(lists, h.bookCount, 10);
  EXPECT_EQ(lists[CLIX_RECENT_LIST].entriesOff, 160u);
  EXPECT_EQ(lists[CLIX_AUTHOR_LIST].entriesOff, 360u);
}

TEST(LibraryFormat, ExternalListDescriptorsAreBoundedByTheListSection) {
  ClixHeader h = makeHeader(100, 116);
  h.listCount = 5;
  h.listLen = 1000;
  const ClixListDesc books{CLIX_LIST_BOOKS, CLIX_ROLE_EXTERNAL, 0, 3, 40, CLIX_ICON_STAR, 0, 900, 997};
  EXPECT_TRUE(validateListDesc(h, 3, books));
  EXPECT_FALSE(validateListDesc(h, 5, books)) << "id past the table";

  ClixListDesc bad = books;
  bad.entryCount = 51;  // 102 bytes from 900 overruns 1000
  EXPECT_FALSE(validateListDesc(h, 3, bad));
  bad = books;
  bad.entriesOff = UINT32_MAX;
  EXPECT_FALSE(validateListDesc(h, 3, bad));
  bad = books;
  bad.labelLen = 4;  // 997 + 4 overruns 1000
  EXPECT_FALSE(validateListDesc(h, 3, bad));
  bad = books;
  bad.role = CLIX_ROLE_AUTHOR;
  EXPECT_FALSE(validateListDesc(h, 3, bad)) << "external ids cannot claim a built-in role";
  bad = books;
  bad.kind = CLIX_LIST_MIXED + 1;
  EXPECT_FALSE(validateListDesc(h, 3, bad));
  bad = books;
  bad.role = CLIX_ROLE_CUSTOM + 1;
  EXPECT_FALSE(validateListDesc(h, 3, bad));

  // Generated lists take the roles past Author, and a Folders tree is Mixed.
  ClixListDesc generated = books;
  for (const uint8_t role :
       {CLIX_ROLE_SERIES, CLIX_ROLE_TAGS, CLIX_ROLE_FOLDERS, CLIX_ROLE_GENERATED, CLIX_ROLE_CUSTOM}) {
    generated.role = role;
    EXPECT_TRUE(validateListDesc(h, 3, generated)) << static_cast<int>(role);
  }
  generated.kind = CLIX_LIST_MIXED;
  EXPECT_TRUE(validateListDesc(h, 3, generated));

  // A Books list may name fewer books than the library holds, but an identity
  // list is the whole record order.
  const ClixListDesc identity{CLIX_LIST_IDENTITY, CLIX_ROLE_EXTERNAL, 0, 0, 100, 0, 0, UINT32_MAX, 0};
  EXPECT_TRUE(validateListDesc(h, 4, identity));
  bad = identity;
  bad.entryCount = 99;
  EXPECT_FALSE(validateListDesc(h, 4, bad));
}

TEST(LibraryFormat, ListIconsFallBackToRoleThenKind) {
  ClixListDesc lists[CLIX_BUILTIN_LISTS];
  builtinListDescs(lists, 100, CLIX_BUILTIN_LISTS);
  EXPECT_EQ(listIcon(lists[CLIX_RECENT_LIST]), CLIX_ICON_RECENT);
  EXPECT_EQ(listIcon(lists[CLIX_TITLE_LIST]), CLIX_ICON_TITLE);
  EXPECT_EQ(listIcon(lists[CLIX_AUTHOR_LIST]), CLIX_ICON_AUTHOR);

  ClixListDesc list{CLIX_LIST_BOOKS, CLIX_ROLE_EXTERNAL, 0, 0, 0, CLIX_ICON_HEART, 0, 0, 0};
  EXPECT_EQ(listIcon(list), CLIX_ICON_HEART) << "a list's own icon wins";
  list.icon = CLIX_ICON_HEART + 1;
  EXPECT_EQ(listIcon(list), CLIX_ICON_LIST) << "an unknown icon falls back to the kind";
  list.icon = CLIX_ICON_DEFAULT;
  list.kind = CLIX_LIST_GROUPS;
  EXPECT_EQ(listIcon(list), CLIX_ICON_FOLDER);
  list.role = CLIX_ROLE_TAGS;
  EXPECT_EQ(listIcon(list), CLIX_ICON_TAGS) << "the role comes before the kind";
  list.role = CLIX_ROLE_FOLDERS;
  list.kind = CLIX_LIST_MIXED;
  EXPECT_EQ(listIcon(list), CLIX_ICON_FOLDER_TREE);
}

TEST(LibraryFormat, BuiltinListsCannotBeReplaced) {
  const ClixHeader h = makeHeader(100, 116);
  ClixListDesc lists[CLIX_BUILTIN_LISTS];
  builtinListDescs(lists, h.bookCount, h.listCount);

  ClixListDesc partialRecent = lists[CLIX_RECENT_LIST];
  partialRecent.entryCount = 99;
  EXPECT_FALSE(validateListDesc(h, CLIX_RECENT_LIST, partialRecent));
  ClixListDesc booksTitle = lists[CLIX_AUTHOR_LIST];
  booksTitle.role = CLIX_ROLE_TITLE;
  EXPECT_FALSE(validateListDesc(h, CLIX_TITLE_LIST, booksTitle));
  EXPECT_FALSE(validateListDesc(h, CLIX_TITLE_LIST, lists[CLIX_AUTHOR_LIST]));
}

TEST(LibraryFormat, SizeArithmeticMatchesTheSpecTable) {
  // The 200-book row: 512 header + 1536 folders + 25600 records + 1024 for the
  // built-in lists (48 table + 800 entries) + 16000 names.
  ClixHeader h{};
  memcpy(h.magic, CLIX_MAGIC, sizeof(CLIX_MAGIC));
  h.formatVersion = CLIX_FORMAT_VERSION;
  h.foldVersion = CLIX_FOLD_VERSION;
  h.bookCount = 200;
  layoutSections(h, 29u * 50u, builtinListBytes(200), 80u * 200u);
  EXPECT_EQ(h.folderStart, 512u);
  EXPECT_EQ(h.recordStart, 2048u);
  EXPECT_EQ(h.listStart, 2048u + 25600u);
  EXPECT_EQ(h.selfSize, 44672u);
}

TEST(LibraryFormatValidation, AcceptsAWellFormedHeader) {
  const ClixHeader h = makeHeader(60, 116);
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::Ok);
}

TEST(LibraryFormatValidation, RejectsBadMagic) {
  ClixHeader h = makeHeader(60, 116);
  h.magic[3] = '2';
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::BadMagic);
  ClixHeader zero{};
  EXPECT_EQ(validateHeader(zero, 0), ClixValidity::BadMagic);
}

TEST(LibraryFormatValidation, RejectsUnknownVersionsSeparately) {
  ClixHeader h = makeHeader(60, 116);
  h.formatVersion = library::CLIX_FORMAT_VERSION + 1;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::UnknownFormatVersion);

  // A fold change is recoverable — firstSeen is preserved across the rebuild —
  // so it must be distinguishable from an unreadable format.
  h = makeHeader(60, 116);
  h.foldVersion = CLIX_FOLD_VERSION + 1;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::StaleFoldVersion);
  EXPECT_EQ(validateHeaderStructure(h, h.selfSize), ClixValidity::Ok);

  // Reconciliation may ignore only the fold version, never damaged layout.
  EXPECT_EQ(validateHeaderStructure(h, h.selfSize - 1), ClixValidity::SizeMismatch);
}

TEST(LibraryFormatValidation, PreviousFormatIsReadableOnlyForReconciliation) {
  // Version 2 kept author[N] then arrival[N] where the list section now sits,
  // and zeros where the list fields now are.
  ClixHeader h = makeHeader(60, 116);
  h.formatVersion = CLIX_PREVIOUS_FORMAT_VERSION;
  h.listLen = 0;
  h.listCount = 0;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::UnknownFormatVersion);
  EXPECT_EQ(validateHeaderStructure(h, h.selfSize), ClixValidity::UnknownFormatVersion);
  EXPECT_EQ(validateHeaderStructure(h, h.selfSize, true), ClixValidity::Ok);
  EXPECT_EQ(validateHeaderStructure(h, h.selfSize - 1, true), ClixValidity::SizeMismatch);
  h.formatVersion = CLIX_PREVIOUS_FORMAT_VERSION - 1;
  EXPECT_EQ(validateHeaderStructure(h, h.selfSize, true), ClixValidity::UnknownFormatVersion);
}

TEST(LibraryFormatValidation, RejectsAListTableThatCannotHoldTheBuiltinLists) {
  ClixHeader h = makeHeader(60, 116);
  h.listCount = CLIX_BUILTIN_LISTS - 1;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::ListsInconsistent);
  h = makeHeader(60, 116);
  h.listCount = CLIX_MAX_LISTS + 1;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::ListsInconsistent);
  h = makeHeader(60, 116);
  h.listCount = static_cast<uint16_t>(h.listLen / sizeof(ClixListDesc) + 1);
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::ListsInconsistent);
  h = makeHeader(60, 116);
  h.listLen = 0xFFFFFF00u;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::SectionsInconsistent);
}

TEST(LibraryFormatValidation, RejectsLengthsBeyondTheFile) {
  // folderLen and nameLen are attacker bytes; near-2^32 values used to wrap
  // the section sums and re-derive the same wrapped layout on both sides.
  ClixHeader h = makeHeader(60, 116);
  h.folderLen = 0xFFFFFF00u;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::SectionsInconsistent);
  h = makeHeader(60, 116);
  h.nameLen = 0xFFFFFF00u;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::SectionsInconsistent);
}

TEST(LibraryFormatValidation, RejectsTruncationInBothDirections) {
  const ClixHeader h = makeHeader(60, 116);
  // Power loss mid-build: the file is short.
  EXPECT_EQ(validateHeader(h, h.selfSize - 1), ClixValidity::SizeMismatch);
  EXPECT_EQ(validateHeader(h, h.selfSize / 2), ClixValidity::SizeMismatch);
  EXPECT_EQ(validateHeader(h, 0), ClixValidity::SizeMismatch);
  // Longer than declared is equally wrong: a stale tail from a previous build.
  EXPECT_EQ(validateHeader(h, h.selfSize + 512), ClixValidity::SizeMismatch);
}

TEST(LibraryFormatValidation, RejectsTamperedOffsets) {
  ClixHeader h = makeHeader(60, 116);
  h.recordStart += CLIX_ALIGN;  // plausible, aligned, and wrong
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::SectionsInconsistent);
}

TEST(LibraryFormatValidation, RejectsAnImpossibleBookCount) {
  ClixHeader h = makeHeader(60, 116);
  h.bookCount = CLIX_MAX_RECORDS + 1;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::CountOutOfRange);
}

TEST(LibraryFormatValidation, RejectsInvalidMetadataMode) {
  ClixHeader h = makeHeader(60, 116);
  h.metadataEnabled = 2;
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::SectionsInconsistent);
}

TEST(LibraryFormatValidation, AcceptsAnEmptyLibrary) {
  // A card with no books must produce a valid index, not a rebuild every boot.
  ClixHeader h = makeHeader(0, 0, 1);
  h.nameLen = 0;
  layoutSections(h, 0, builtinListBytes(0), 0);
  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::Ok);
  // Header sector, then the list table's sector; folders and records are empty.
  EXPECT_EQ(h.listStart, CLIX_ALIGN);
  EXPECT_EQ(h.selfSize, 2 * CLIX_ALIGN);
}

TEST(LibraryHeaderFlags, DedupDegradationIsPersistedWithoutChangingTheLayout) {
  ClixHeader h = makeHeader(60, 116);
  h.flags = CLIX_FLAG_DEDUP_DEGRADED;

  EXPECT_EQ(validateHeader(h, h.selfSize), ClixValidity::Ok);
  EXPECT_NE(h.flags & CLIX_FLAG_DEDUP_DEGRADED, 0);
  EXPECT_EQ(sizeof(ClixHeader), 64u);
}

TEST(LibraryFormat, ByteImageIsStableAcrossBuilds) {
  // Guards the packing itself: if a compiler ever inserts padding, these field
  // offsets move and the on-disk format silently forks.
  EXPECT_EQ(offsetof(ClixRecord, nameOff), 0u);
  EXPECT_EQ(offsetof(ClixRecord, fileSize), 4u);
  EXPECT_EQ(offsetof(ClixRecord, firstSeen), 8u);
  EXPECT_EQ(offsetof(ClixRecord, folderId), 10u);
  EXPECT_EQ(offsetof(ClixRecord, nameLen), 12u);
  EXPECT_EQ(offsetof(ClixRecord, foldLen), 13u);
  EXPECT_EQ(offsetof(ClixRecord, authorKeyLen), 14u);
  EXPECT_EQ(offsetof(ClixRecord, metadataStatus), 15u);
  EXPECT_EQ(offsetof(ClixRecord, fold), 16u);
  EXPECT_EQ(offsetof(ClixRecord, authorKey), 112u);
  EXPECT_EQ(offsetof(ClixRecord, modificationTime), 124u);

  EXPECT_EQ(offsetof(ClixHeader, metadataEnabled), 7u);
  EXPECT_EQ(offsetof(ClixHeader, bookCount), 8u);
  EXPECT_EQ(offsetof(ClixHeader, folderStart), 16u);
  EXPECT_EQ(offsetof(ClixHeader, listStart), 28u);
  EXPECT_EQ(offsetof(ClixHeader, selfSize), 40u);
  EXPECT_EQ(offsetof(ClixHeader, listLen), 44u);
  EXPECT_EQ(offsetof(ClixHeader, listCount), 48u);
  EXPECT_EQ(offsetof(ClixHeader, listOptions), 50u);
  EXPECT_EQ(offsetof(ClixHeader, customListsHash), 51u);

  EXPECT_EQ(offsetof(ClixListDesc, entryCount), 4u);
  EXPECT_EQ(offsetof(ClixListDesc, entriesOff), 8u);
  EXPECT_EQ(offsetof(ClixListDesc, labelOff), 12u);
}
