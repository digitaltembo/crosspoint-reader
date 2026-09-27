#include "LibraryGenerated.h"

#include <Arduino.h>
#include <BufferedFile.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <string_view>

#include "LibraryCustomLists.h"
#include "LibraryFormat.h"
#include "LibraryText.h"

namespace library {
namespace {

constexpr uint16_t NONE = 0xFFFF;
constexpr size_t GEN_IO_BUFFER = 1024;
constexpr size_t LABEL_KEY_BYTES = 8;

// Same cadence as the builder: let the idle task run during long passes.
void service(uint32_t& units) {
  if ((++units & 0x1Fu) == 0) delay(1);
}

uint32_t hash32(const std::string_view text) {
  uint32_t hash = 2166136261u;  // FNV-1a 32
  for (const char c : text) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 16777619u;
  }
  return hash;
}

// The first folded bytes of a label: enough to order lists alphabetically
// without holding every label in RAM.
void labelKey(const std::string_view label, char* key) {
  const std::string folded = fold(label);
  memset(key, 0, LABEL_KEY_BYTES);
  memcpy(key, folded.data(), std::min(folded.size(), LABEL_KEY_BYTES));
}

// Walks the '\n'-separated tags of one book.
struct TagCursor {
  const std::string& joined;
  size_t at = 0;
  bool next(size_t& offset, size_t& len) {
    while (at < joined.size()) {
      size_t end = joined.find('\n', at);
      if (end == std::string::npos) end = joined.size();
      offset = at;
      len = end - at;
      at = end + 1;
      if (len > 0) return true;
    }
    return false;
  }
};

class Writer {
 public:
  Writer(HalFile& metaFile, HalFile& entriesFile)
      : meta(metaFile, GEN_IO_BUFFER), entries(entriesFile, GEN_IO_BUFFER) {}

  void begin(const uint8_t kind, const uint8_t role, const uint8_t flags, const uint8_t icon) {
    desc = ClixListDesc{kind, role, flags, 0, 0, icon, 0, entryCursor, 0};
  }
  void entry(const uint16_t value) {
    entries.write(&value, sizeof(value));
    desc.entryCount++;
    entryCursor += sizeof(value);
  }
  void end(const std::string_view label) {
    const int len = utf8SafeTruncateBuffer(label.data(), static_cast<int>(std::min<size_t>(label.size(), UINT8_MAX)));
    desc.labelLen = static_cast<uint8_t>(len);
    desc.labelOff = len > 0 ? labelCursor : 0;
    labelCursor += static_cast<uint32_t>(len);
    meta.write(&desc, sizeof(desc));
    if (len > 0) meta.write(label.data(), static_cast<size_t>(len));
    lists++;
  }
  bool flush() {
    const bool metaOk = meta.flush();
    const bool entriesOk = entries.flush();
    return metaOk && entriesOk;
  }

  uint16_t lists = 0;
  uint32_t entryCursor = 0;
  uint32_t labelCursor = 0;

 private:
  serialization::BufferedFileWriter meta;
  serialization::BufferedFileWriter entries;
  ClixListDesc desc{};
};

struct SeriesKey {
  uint32_t hash;
  uint16_t index;
  uint16_t ordinal;
};

struct SeriesGroup {
  char key[LABEL_KEY_BYTES];
  uint16_t first;
  uint16_t count;
  uint16_t labelOrdinal;  // spelled as the series' first book in title order spells it
};

// Groups of one Books list per series, alphabetical, each in series order.
bool generateSeries(GeneratedListSource& source, const uint16_t books, Writer& writer, const uint16_t firstId,
                    const uint16_t budget, bool& incomplete, uint32_t& units) {
  std::string name;
  std::string index;
  uint16_t count = 0;
  for (uint16_t t = 0; t < books; t++) {
    service(units);
    if (!source.readSeries(t, name, index)) return false;
    if (!fold(name).empty()) count++;
  }

  std::unique_ptr<SeriesKey[]> keys;
  if (count > 0) {
    keys = makeUniqueNoThrow<SeriesKey[]>(count);
    if (!keys) {
      LOG_ERR("LIBGEN", "OOM: %u-byte series keys", static_cast<unsigned>(count * sizeof(SeriesKey)));
      incomplete = true;
      return true;
    }
  }
  uint16_t filled = 0;
  for (uint16_t t = 0; t < books && filled < count; t++) {
    service(units);
    if (!source.readSeries(t, name, index)) return false;
    const std::string folded = fold(name);
    if (folded.empty()) continue;
    keys[filled++] = SeriesKey{hash32(folded), seriesIndexKey(index), t};
  }
  count = filled;
  std::sort(keys.get(), keys.get() + count, [](const SeriesKey& a, const SeriesKey& b) {
    if (a.hash != b.hash) return a.hash < b.hash;
    if (a.index != b.index) return a.index < b.index;
    return a.ordinal < b.ordinal;
  });

  uint16_t groupCount = 0;
  for (uint16_t i = 0; i < count; i++) {
    if (i == 0 || keys[i].hash != keys[i - 1].hash) groupCount++;
  }
  std::unique_ptr<SeriesGroup[]> groups;
  if (groupCount > 0) {
    groups = makeUniqueNoThrow<SeriesGroup[]>(groupCount);
    if (!groups) {
      LOG_ERR("LIBGEN", "OOM: %u-byte series groups", static_cast<unsigned>(groupCount * sizeof(SeriesGroup)));
      incomplete = true;
      return true;
    }
  }
  uint16_t g = 0;
  for (uint16_t i = 0; i < count; i++) {
    if (i > 0 && keys[i].hash == keys[i - 1].hash) {
      SeriesGroup& group = groups[g - 1];
      group.count++;
      group.labelOrdinal = std::min(group.labelOrdinal, keys[i].ordinal);
      continue;
    }
    groups[g++] = SeriesGroup{{}, i, 1, keys[i].ordinal};
  }
  for (uint16_t i = 0; i < groupCount; i++) {
    service(units);
    if (!source.readSeries(groups[i].labelOrdinal, name, index)) return false;
    labelKey(name, groups[i].key);
  }
  std::sort(groups.get(), groups.get() + groupCount, [&keys](const SeriesGroup& a, const SeriesGroup& b) {
    const int cmp = memcmp(a.key, b.key, LABEL_KEY_BYTES);
    return cmp != 0 ? cmp < 0 : keys[a.first].hash < keys[b.first].hash;
  });

  if (1u + groupCount > budget) {
    LOG_ERR("LIBGEN", "%u series exceed the list limit", static_cast<unsigned>(groupCount));
    incomplete = true;
    return true;
  }
  writer.begin(CLIX_LIST_GROUPS, CLIX_ROLE_SERIES, CLIX_LIST_TOP_LEVEL, CLIX_ICON_SERIES);
  for (uint16_t i = 0; i < groupCount; i++) writer.entry(static_cast<uint16_t>(firstId + 1 + i));
  writer.end({});
  for (uint16_t i = 0; i < groupCount; i++) {
    service(units);
    writer.begin(CLIX_LIST_BOOKS, CLIX_ROLE_GENERATED, 0, CLIX_ICON_SERIES_ENTRY);
    for (uint16_t k = 0; k < groups[i].count; k++) writer.entry(keys[groups[i].first + k].ordinal);
    if (!source.readSeries(groups[i].labelOrdinal, name, index)) return false;
    writer.end(name);
  }
  return true;
}

struct TagCandidate {
  uint32_t hash;
  uint16_t count;
  uint16_t ordinal;  // a book carrying the tag, for its spelling
  uint8_t offset;
  uint8_t len;
};

struct TagSlot {
  char key[LABEL_KEY_BYTES];
  uint32_t hash;
  uint16_t ordinal;
  uint8_t offset;
  uint8_t len;
};

struct HashSlot {
  uint32_t hash;
  uint16_t slot;
};

// Slot of the tag at `offset` in `joined`, or NONE when it is not listed.
uint16_t findSlot(const HashSlot* bySlotHash, const uint16_t slots, const std::string& joined, const size_t offset,
                  const size_t len) {
  const uint32_t hash = hash32(fold(std::string_view(joined).substr(offset, len)));
  const HashSlot* end = bySlotHash + slots;
  const HashSlot* found =
      std::lower_bound(bySlotHash, end, hash, [](const HashSlot& s, const uint32_t h) { return s.hash < h; });
  return found != end && found->hash == hash ? found->slot : NONE;
}

// Groups of one Books list per tag, alphabetical, each in title order.
bool generateTags(GeneratedListSource& source, const uint16_t books, Writer& writer, const uint16_t firstId,
                  const uint16_t budget, bool& incomplete, uint32_t& units) {
  auto candidates = makeUniqueNoThrow<TagCandidate[]>(MAX_TAG_CANDIDATES);
  if (!candidates) {
    LOG_ERR("LIBGEN", "OOM: %u-byte tag table", static_cast<unsigned>(MAX_TAG_CANDIDATES * sizeof(TagCandidate)));
    incomplete = true;
    return true;
  }
  uint16_t candidateCount = 0;
  bool capped = false;
  std::string joined;
  size_t offset = 0;
  size_t len = 0;
  for (uint16_t t = 0; t < books; t++) {
    service(units);
    if (!source.readTags(t, joined)) return false;
    TagCursor cursor{joined};
    while (cursor.next(offset, len)) {
      if (offset > UINT8_MAX || len > UINT8_MAX) break;
      const std::string folded = fold(std::string_view(joined).substr(offset, len));
      if (folded.empty()) continue;
      const uint32_t hash = hash32(folded);
      TagCandidate* end = candidates.get() + candidateCount;
      TagCandidate* at = std::lower_bound(candidates.get(), end, hash,
                                          [](const TagCandidate& c, const uint32_t h) { return c.hash < h; });
      if (at != end && at->hash == hash) {
        if (at->count < UINT16_MAX) at->count++;
      } else if (candidateCount < MAX_TAG_CANDIDATES) {
        memmove(at + 1, at, static_cast<size_t>(end - at) * sizeof(TagCandidate));
        *at = TagCandidate{hash, 1, t, static_cast<uint8_t>(offset), static_cast<uint8_t>(len)};
        candidateCount++;
      } else {
        capped = true;
      }
    }
  }
  if (capped) LOG_INF("LIBGEN", "more than %u distinct tags; the rest are not counted", MAX_TAG_CANDIDATES);

  // The most used tags, then alphabetically.
  const uint16_t slots = std::min(candidateCount, MAX_TAGS);
  std::partial_sort(candidates.get(), candidates.get() + slots, candidates.get() + candidateCount,
                    [](const TagCandidate& a, const TagCandidate& b) {
                      return a.count != b.count ? a.count > b.count : a.hash < b.hash;
                    });
  std::unique_ptr<TagSlot[]> tags;
  if (slots > 0) {
    tags = makeUniqueNoThrow<TagSlot[]>(slots);
    if (!tags) {
      LOG_ERR("LIBGEN", "OOM: %u-byte tag slots", static_cast<unsigned>(slots * sizeof(TagSlot)));
      incomplete = true;
      return true;
    }
  }
  for (uint16_t s = 0; s < slots; s++) {
    service(units);
    const TagCandidate& c = candidates[s];
    if (!source.readTags(c.ordinal, joined)) return false;
    tags[s] = TagSlot{{}, c.hash, c.ordinal, c.offset, c.len};
    labelKey(std::string_view(joined).substr(c.offset, c.len), tags[s].key);
  }
  candidates.reset();
  std::sort(tags.get(), tags.get() + slots, [](const TagSlot& a, const TagSlot& b) {
    const int cmp = memcmp(a.key, b.key, LABEL_KEY_BYTES);
    return cmp != 0 ? cmp < 0 : a.hash < b.hash;
  });
  if (1u + slots > budget) {
    LOG_ERR("LIBGEN", "%u tags exceed the list limit", static_cast<unsigned>(slots));
    incomplete = true;
    return true;
  }

  std::unique_ptr<HashSlot[]> byHash;
  std::unique_ptr<uint32_t[]> fill;
  if (slots > 0) {
    byHash = makeUniqueNoThrow<HashSlot[]>(slots);
    fill = makeUniqueNoThrow<uint32_t[]>(slots + 1u);
    if (!byHash || !fill) {
      LOG_ERR("LIBGEN", "OOM: tag lookup for %u tags", static_cast<unsigned>(slots));
      incomplete = true;
      return true;
    }
    for (uint16_t s = 0; s < slots; s++) byHash[s] = HashSlot{tags[s].hash, s};
    std::sort(byHash.get(), byHash.get() + slots, [](const HashSlot& a, const HashSlot& b) { return a.hash < b.hash; });
    for (uint16_t s = 0; s <= slots; s++) fill[s] = 0;
  }

  // Count, then fill, each book's listed tags; a tag spelled twice in one
  // book counts once.
  constexpr uint8_t MAX_SEEN = 16;
  const auto visitBook = [&](const uint16_t t, uint16_t* entries) {
    if (!source.readTags(t, joined)) return false;
    uint16_t seen[MAX_SEEN];
    uint8_t seenCount = 0;
    TagCursor cursor{joined};
    while (cursor.next(offset, len)) {
      const uint16_t slot = findSlot(byHash.get(), slots, joined, offset, len);
      if (slot == NONE || std::find(seen, seen + seenCount, slot) != seen + seenCount) continue;
      if (seenCount < MAX_SEEN) seen[seenCount++] = slot;
      if (entries) {
        entries[fill[slot]++] = t;
      } else {
        fill[slot + 1]++;
      }
    }
    return true;
  };
  std::unique_ptr<uint16_t[]> entries;
  uint32_t total = 0;
  if (slots > 0) {
    for (uint16_t t = 0; t < books; t++) {
      service(units);
      if (!visitBook(t, nullptr)) return false;
    }
    for (uint16_t s = 0; s < slots; s++) fill[s + 1] += fill[s];
    total = fill[slots];
    if (total > 0) {
      entries = makeUniqueNoThrow<uint16_t[]>(total);
      if (!entries) {
        LOG_ERR("LIBGEN", "OOM: %u-byte tag entries", static_cast<unsigned>(total * sizeof(uint16_t)));
        incomplete = true;
        return true;
      }
      for (uint16_t t = 0; t < books; t++) {
        service(units);
        if (!visitBook(t, entries.get())) return false;
      }
    }
  }

  // fill[s] now ends slot s, so slot s spans [s ? fill[s - 1] : 0, fill[s]).
  writer.begin(CLIX_LIST_GROUPS, CLIX_ROLE_TAGS, CLIX_LIST_TOP_LEVEL, CLIX_ICON_TAGS);
  for (uint16_t s = 0; s < slots; s++) writer.entry(static_cast<uint16_t>(firstId + 1 + s));
  writer.end({});
  for (uint16_t s = 0; s < slots; s++) {
    service(units);
    writer.begin(CLIX_LIST_BOOKS, CLIX_ROLE_GENERATED, 0, CLIX_ICON_TAG);
    for (uint32_t k = s == 0 ? 0 : fill[s - 1]; k < fill[s]; k++) writer.entry(entries[k]);
    if (!source.readTags(tags[s].ordinal, joined)) return false;
    writer.end(std::string_view(joined).substr(tags[s].offset, tags[s].len));
  }
  return true;
}

struct FolderNode {
  uint32_t hash;     // of the node's path
  uint32_t pathOff;  // into the folder section, a path this node is a prefix of
  uint16_t parent;
  uint16_t books;
  uint8_t prefixLen;   // the node's own path is pathOff's first prefixLen bytes
  uint8_t labelStart;  // and its name starts here
  char key[LABEL_KEY_BYTES];
};

// A Mixed tree of the card's folders: each node lists its subfolders, then
// its books in title order. Folders that hold only other folders are nodes too.
bool generateFolders(GeneratedListSource& source, const uint16_t books, const uint16_t folderCount, Writer& writer,
                     const uint16_t firstId, const uint16_t budget, bool& incomplete, uint32_t& units) {
  // Every node is a prefix of a book folder's path, and paths are at most
  // LIBRARY_MAX_DEPTH+1 components deep below the root.
  constexpr uint32_t MAX_COMPONENTS = 8;
  const uint16_t capacity =
      static_cast<uint16_t>(std::min<uint32_t>(MAX_FOLDER_NODES, 1u + folderCount * MAX_COMPONENTS));
  auto nodes = makeUniqueNoThrow<FolderNode[]>(capacity);
  auto byHash = makeUniqueNoThrow<uint16_t[]>(capacity);
  auto folderNode = makeUniqueNoThrow<uint16_t[]>(folderCount == 0 ? 1 : folderCount);
  if (!nodes || !byHash || !folderNode) {
    LOG_ERR("LIBGEN", "OOM: folder tree for %u folders", static_cast<unsigned>(folderCount));
    incomplete = true;
    return true;
  }
  nodes[0] = FolderNode{hash32({}), 0, NONE, 0, 0, 0, {}};
  byHash[0] = 0;
  uint16_t nodeCount = 1;

  std::string path;
  uint32_t offset = 0;
  for (uint16_t f = 0; f < folderCount; f++) {
    service(units);
    uint8_t pathLen = 0;
    if (!source.readFolders(offset, &pathLen, 1)) return false;
    path.resize(pathLen);
    if (pathLen > 0 && !source.readFolders(offset + 1, path.data(), pathLen)) return false;
    uint16_t node = 0;
    size_t start = 1;  // past the leading '/'
    while (start < path.size()) {
      size_t end = path.find('/', start);
      if (end == std::string::npos) end = path.size();
      if (end > start) {
        const uint32_t hash = hash32(std::string_view(path).substr(0, end));
        uint16_t* const last = byHash.get() + nodeCount;
        uint16_t* at = std::lower_bound(byHash.get(), last, hash,
                                        [&nodes](const uint16_t n, const uint32_t h) { return nodes[n].hash < h; });
        if (at != last && nodes[*at].hash == hash) {
          node = *at;
        } else {
          if (nodeCount == capacity) {
            LOG_ERR("LIBGEN", "more than %u folders; Folders list left out", static_cast<unsigned>(capacity));
            incomplete = true;
            return true;
          }
          FolderNode& created = nodes[nodeCount];
          created = FolderNode{hash, offset + 1, node, 0, static_cast<uint8_t>(end), static_cast<uint8_t>(start), {}};
          labelKey(std::string_view(path).substr(start, end - start), created.key);
          memmove(at + 1, at, static_cast<size_t>(last - at) * sizeof(uint16_t));
          *at = nodeCount;
          node = nodeCount++;
        }
      }
      start = end + 1;
    }
    folderNode[f] = node;
    offset += 1u + pathLen;
  }
  byHash.reset();

  // Books per node, bucketed in title order.
  auto bookStart = makeUniqueNoThrow<uint16_t[]>(nodeCount + 1u);
  auto bookEntries = makeUniqueNoThrow<uint16_t[]>(books == 0 ? 1 : books);
  if (!bookStart || !bookEntries) {
    LOG_ERR("LIBGEN", "OOM: folder book lists for %u books", static_cast<unsigned>(books));
    incomplete = true;
    return true;
  }
  uint16_t folderId = 0;
  for (uint16_t t = 0; t < books; t++) {
    service(units);
    if (!source.readFolderId(t, folderId)) return false;
    if (folderId < folderCount) nodes[folderNode[folderId]].books++;
  }
  bookStart[0] = 0;
  for (uint16_t n = 0; n < nodeCount; n++) bookStart[n + 1] = static_cast<uint16_t>(bookStart[n] + nodes[n].books);
  for (uint16_t n = 0; n < nodeCount; n++) nodes[n].books = 0;  // reused as each node's fill cursor
  for (uint16_t t = 0; t < books; t++) {
    service(units);
    if (!source.readFolderId(t, folderId)) return false;
    if (folderId >= folderCount) continue;
    FolderNode& node = nodes[folderNode[folderId]];
    bookEntries[bookStart[folderNode[folderId]] + node.books++] = t;
  }
  folderNode.reset();

  // Subfolders alphabetically, then ids breadth first: every parent precedes
  // its children, and one node's children take consecutive ids.
  auto byParent = makeUniqueNoThrow<uint16_t[]>(nodeCount);
  auto childStart = makeUniqueNoThrow<uint16_t[]>(nodeCount);
  auto childCount = makeUniqueNoThrow<uint16_t[]>(nodeCount);
  auto queue = makeUniqueNoThrow<uint16_t[]>(nodeCount);
  auto newId = makeUniqueNoThrow<uint16_t[]>(nodeCount);
  if (!byParent || !childStart || !childCount || !queue || !newId) {
    LOG_ERR("LIBGEN", "OOM: folder order for %u folders", static_cast<unsigned>(nodeCount));
    incomplete = true;
    return true;
  }
  for (uint16_t n = 0; n + 1 < nodeCount; n++) byParent[n] = static_cast<uint16_t>(n + 1);
  std::sort(byParent.get(), byParent.get() + nodeCount - 1, [&nodes](const uint16_t a, const uint16_t b) {
    if (nodes[a].parent != nodes[b].parent) return nodes[a].parent < nodes[b].parent;
    const int cmp = memcmp(nodes[a].key, nodes[b].key, LABEL_KEY_BYTES);
    return cmp != 0 ? cmp < 0 : nodes[a].hash < nodes[b].hash;
  });
  for (uint16_t n = 0; n < nodeCount; n++) childCount[n] = 0;
  for (uint16_t i = 0; i + 1 < nodeCount; i++) {
    const uint16_t parent = nodes[byParent[i]].parent;
    if (childCount[parent]++ == 0) childStart[parent] = i;
  }
  uint16_t tail = 1;
  queue[0] = 0;
  for (uint16_t head = 0; head < tail; head++) {
    const uint16_t node = queue[head];
    newId[node] = static_cast<uint16_t>(firstId + head);
    for (uint16_t c = 0; c < childCount[node]; c++) queue[tail++] = byParent[childStart[node] + c];
  }
  if (nodeCount > budget) {
    LOG_ERR("LIBGEN", "%u folders exceed the list limit", static_cast<unsigned>(nodeCount));
    incomplete = true;
    return true;
  }

  std::string label;
  for (uint16_t q = 0; q < nodeCount; q++) {
    service(units);
    const uint16_t node = queue[q];
    const bool root = node == 0;
    writer.begin(CLIX_LIST_MIXED, root ? CLIX_ROLE_FOLDERS : CLIX_ROLE_GENERATED, root ? CLIX_LIST_TOP_LEVEL : 0,
                 root ? CLIX_ICON_FOLDER_TREE : CLIX_ICON_FOLDER);
    for (uint16_t c = 0; c < childCount[node]; c++) {
      writer.entry(static_cast<uint16_t>(newId[byParent[childStart[node] + c]] | CLIX_ENTRY_LIST_BIT));
    }
    for (uint16_t b = bookStart[node]; b < bookStart[node + 1]; b++) writer.entry(bookEntries[b]);
    label.clear();
    if (node != 0) {
      const FolderNode& n = nodes[node];
      label.resize(n.prefixLen - n.labelStart);
      if (!source.readFolders(n.pathOff + n.labelStart, label.data(), label.size())) return false;
    }
    writer.end(label);
  }
  return true;
}

// One Groups list per custom list, of one Books list per non-empty sublist in
// file order, each in title order. One pass counts every sublist's books and
// a second fills them, however many custom lists there are.
bool generateCustom(GeneratedListSource& source, const uint16_t books, Writer& writer, const uint16_t firstId,
                    const uint16_t budget, bool& incomplete, uint32_t& units) {
  CustomLists custom;
  const CustomListsResult loaded = loadCustomLists(CUSTOM_LISTS_PATH, custom);
  if (loaded == CustomListsResult::Missing) return true;
  if (loaded != CustomListsResult::Ok) {
    incomplete = true;
    return loaded != CustomListsResult::ReadError;
  }
  incomplete = incomplete || custom.truncated;
  const uint16_t slots = custom.subCount;
  if (slots == 0) return true;

  auto byHash = makeUniqueNoThrow<HashSlot[]>(slots);
  auto fill = makeUniqueNoThrow<uint32_t[]>(slots + 1u);
  if (!byHash || !fill) {
    LOG_ERR("LIBGEN", "OOM: custom list lookup for %u sublists", static_cast<unsigned>(slots));
    incomplete = true;
    return true;
  }
  for (uint16_t s = 0; s < slots; s++) byHash[s] = HashSlot{custom.subs[s].tagHash, s};
  std::sort(byHash.get(), byHash.get() + slots,
            [](const HashSlot& a, const HashSlot& b) { return a.hash != b.hash ? a.hash < b.hash : a.slot < b.slot; });
  for (uint16_t s = 0; s <= slots; s++) fill[s] = 0;

  // Count, then fill, the sublists each book's tags match. Several sublists may
  // name one tag; a book matching a sublist twice is listed once.
  std::string joined;
  size_t offset = 0;
  size_t len = 0;
  constexpr uint8_t MAX_SEEN = 32;
  const auto visitBook = [&](const uint16_t t, uint16_t* entries) {
    if (!source.readTags(t, joined)) return false;
    uint16_t seen[MAX_SEEN];
    uint8_t seenCount = 0;
    TagCursor cursor{joined};
    while (cursor.next(offset, len)) {
      const uint32_t hash = foldedTagHash(std::string_view(joined).substr(offset, len));
      const HashSlot* first = byHash.get();
      const HashSlot* end = first + slots;
      for (const HashSlot *at =
               std::lower_bound(first, end, hash, [](const HashSlot& s, const uint32_t h) { return s.hash < h; });
           at != end && at->hash == hash; ++at) {
        if (std::find(seen, seen + seenCount, at->slot) != seen + seenCount) continue;
        if (seenCount < MAX_SEEN) seen[seenCount++] = at->slot;
        if (entries) {
          entries[fill[at->slot]++] = t;
        } else {
          fill[at->slot + 1]++;
        }
      }
    }
    return true;
  };
  for (uint16_t t = 0; t < books; t++) {
    service(units);
    if (!visitBook(t, nullptr)) return false;
  }
  for (uint16_t s = 0; s < slots; s++) fill[s + 1] += fill[s];
  const uint32_t total = fill[slots];
  std::unique_ptr<uint16_t[]> entries;
  if (total > 0) {
    entries = makeUniqueNoThrow<uint16_t[]>(total);
    if (!entries) {
      LOG_ERR("LIBGEN", "OOM: %u-byte custom list entries", static_cast<unsigned>(total * sizeof(uint16_t)));
      incomplete = true;
      return true;
    }
    for (uint16_t t = 0; t < books; t++) {
      service(units);
      if (!visitBook(t, entries.get())) return false;
    }
  }

  // fill[s] now ends slot s, so slot s spans [s ? fill[s - 1] : 0, fill[s]).
  const auto begins = [&fill](const uint16_t s) { return s == 0 ? 0u : fill[s - 1]; };
  uint16_t used = 0;
  for (uint8_t l = 0; l < custom.listCount; l++) {
    const CustomLists::List& list = custom.lists[l];
    uint16_t children = 0;
    for (uint16_t s = list.firstSub; s < list.firstSub + list.subCount; s++) children += fill[s] > begins(s);
    if (children == 0) continue;
    if (used + 1u + children > budget) {
      LOG_ERR("LIBGEN", "custom lists exceed the list limit");
      incomplete = true;
      return true;
    }
    const uint16_t id = static_cast<uint16_t>(firstId + used);
    writer.begin(CLIX_LIST_GROUPS, CLIX_ROLE_CUSTOM, CLIX_LIST_TOP_LEVEL, CLIX_ICON_TAGS);
    for (uint16_t c = 0; c < children; c++) writer.entry(static_cast<uint16_t>(id + 1 + c));
    writer.end(custom.label(list.labelOff, list.labelLen));
    for (uint16_t s = list.firstSub; s < list.firstSub + list.subCount; s++) {
      if (fill[s] == begins(s)) continue;
      service(units);
      writer.begin(CLIX_LIST_BOOKS, CLIX_ROLE_GENERATED, 0, CLIX_ICON_TAG);
      for (uint32_t k = begins(s); k < fill[s]; k++) writer.entry(entries[k]);
      writer.end(custom.label(custom.subs[s].labelOff, custom.subs[s].labelLen));
    }
    used = static_cast<uint16_t>(used + 1 + children);
  }
  return true;
}

}  // namespace

uint16_t seriesIndexKey(const std::string& index) {
  uint32_t whole = 0;
  uint32_t tenth = 0;
  bool digits = false;
  size_t i = 0;
  for (; i < index.size() && index[i] >= '0' && index[i] <= '9'; i++) {
    whole = std::min<uint32_t>(whole * 10 + static_cast<uint32_t>(index[i] - '0'), 6553);
    digits = true;
  }
  if (i + 1 < index.size() && index[i] == '.' && index[i + 1] >= '0' && index[i + 1] <= '9') {
    tenth = static_cast<uint32_t>(index[i + 1] - '0');
    digits = true;
  }
  return digits ? static_cast<uint16_t>(whole * 10 + tenth) : UINT16_MAX;
}

bool generateLists(GeneratedListSource& source, const uint16_t books, const uint16_t folderCount, const uint8_t options,
                   const uint16_t firstId, const uint16_t maxLists, const char* metaPath, const char* entriesPath,
                   GeneratedLists& out) {
  out = GeneratedLists{};
  HalFile metaFile;
  HalFile entriesFile;
  if (!Storage.openFileForWrite("LIBGEN", metaPath, metaFile) ||
      !Storage.openFileForWrite("LIBGEN", entriesPath, entriesFile)) {
    LOG_ERR("LIBGEN", "cannot open list staging files");
    return false;
  }
  uint32_t units = 0;
  bool ok = true;
  {
    Writer writer(metaFile, entriesFile);
    const auto remaining = [&writer, maxLists] {
      return static_cast<uint16_t>(maxLists > writer.lists ? maxLists - writer.lists : 0);
    };
    if (ok && (options & CLIX_OPTION_SERIES)) {
      ok = generateSeries(source, books, writer, static_cast<uint16_t>(firstId + writer.lists), remaining(),
                          out.incomplete, units);
    }
    if (ok && (options & CLIX_OPTION_TAGS)) {
      ok = generateTags(source, books, writer, static_cast<uint16_t>(firstId + writer.lists), remaining(),
                        out.incomplete, units);
    }
    if (ok && (options & CLIX_OPTION_FOLDERS)) {
      ok = generateFolders(source, books, folderCount, writer, static_cast<uint16_t>(firstId + writer.lists),
                           remaining(), out.incomplete, units);
    }
    if (ok && (options & CLIX_OPTION_CUSTOM)) {
      ok = generateCustom(source, books, writer, static_cast<uint16_t>(firstId + writer.lists), remaining(),
                          out.incomplete, units);
    }
    ok = writer.flush() && ok;
    out.listCount = writer.lists;
    out.entryBytes = writer.entryCursor;
    out.labelBytes = writer.labelCursor;
  }
  const bool metaClosed = metaFile.close();
  const bool entriesClosed = entriesFile.close();
  return ok && metaClosed && entriesClosed;
}

}  // namespace library
