#pragma once

// The lists the builder generates from the books themselves, as the library
// settings ask: Series (a Groups list of per-series Books lists, each in series
// order), Tags (the same per tag, in title order), Folders (a Mixed tree that
// mirrors the card's folders) and Custom (one Groups list per entry of the
// custom lists file, of per-tag Books lists, or one tag's Books list; see
// LibraryCustomLists.h).
//
// Each kind is computed on its own and its scratch released before the next, so
// the peak is the largest kind rather than their sum. Results are staged to two
// files on the card that the index emit splices into the list section:
//
//   meta     per list, in id order: its ClixListDesc (entriesOff and labelOff
//            relative to the generated entries and labels), then its label
//   entries  every generated list's u16 entries, in id order

#include <cstddef>
#include <cstdint>
#include <string>

namespace library {

// Tags beyond these limits are not listed: the candidates table counts the
// first MAX_TAG_CANDIDATES distinct tags met, and the MAX_TAGS most used of
// those get a list.
inline constexpr uint16_t MAX_TAG_CANDIDATES = 512;
inline constexpr uint16_t MAX_TAGS = 128;
// Folders past this (including the ones that only hold other folders) drop the
// Folders list rather than risk its scratch on a C3.
inline constexpr uint16_t MAX_FOLDER_NODES = 1024;

// What the generated lists read about each book, by title-order ordinal.
class GeneratedListSource {
 public:
  virtual bool readSeries(uint16_t ordinal, std::string& name, std::string& index) = 0;
  // Tags joined with '\n'.
  virtual bool readTags(uint16_t ordinal, std::string& joined) = 0;
  virtual bool readFolderId(uint16_t ordinal, uint16_t& folderId) = 0;
  // `len` bytes of the folder section ([u8 len][path] per folder) from `offset`.
  virtual bool readFolders(uint32_t offset, void* out, size_t len) = 0;

 protected:
  ~GeneratedListSource() = default;
};

struct GeneratedLists {
  uint16_t listCount = 0;
  uint32_t entryBytes = 0;
  uint32_t labelBytes = 0;
  // An enabled kind was left out: its scratch could not be allocated, or it
  // would not fit the list or folder limits.
  bool incomplete = false;
  // customListsHash of the file the Custom lists were asked to read; set by
  // the builder, which records it in the header.
  uint32_t customListsHash = 0;
};

// Stage the kinds `options` (ClixListOption bits) asks for, as ids from
// `firstId`, using at most `maxLists` ids. False only on an I/O failure, which
// fails the build; a kind that cannot be built is left out and reported.
bool generateLists(GeneratedListSource& source, uint16_t books, uint16_t folderCount, uint8_t options, uint16_t firstId,
                   uint16_t maxLists, const char* metaPath, const char* entriesPath, GeneratedLists& out);

// Series index as tenths ("3" -> 30, "1.5" -> 15) for ordering; an absent or
// unreadable index sorts last.
uint16_t seriesIndexKey(const std::string& index);

}  // namespace library
