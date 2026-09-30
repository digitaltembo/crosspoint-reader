#pragma once

// The custom lists file: tag-defined lists, written by the user.
//
//   { "<list label>": { "<sublist label>": "<tag>", ... } | "<tag>", ... }
//
// A member whose value is an object becomes a top-level list with one sublist
// per pair, in file order, each holding the books that carry that tag. A member
// whose value is a tag becomes a top-level list of those books directly.
// Members of any other shape are skipped. The file is read as a stream, so no
// JSON document is held in RAM.

#include <cstdint>
#include <memory>
#include <string_view>

namespace library {

inline constexpr char CUSTOM_LISTS_PATH[] = "/.crosspoint/customlists.json";
inline constexpr uint8_t MAX_CUSTOM_LISTS = 16;
inline constexpr uint16_t MAX_CUSTOM_SUBLISTS = 256;
inline constexpr uint16_t MAX_CUSTOM_LABEL_BYTES = 8192;

struct CustomLists {
  struct List {
    uint16_t labelOff;
    uint8_t labelLen;
    uint16_t firstSub;
    uint16_t subCount;
    // Named a tag rather than sublists: its one sublist is unlabelled, and
    // its books are listed directly.
    bool direct;
  };
  struct Sub {
    uint32_t tagHash;  // foldedTagHash of its tag
    uint16_t labelOff;
    uint8_t labelLen;
  };
  std::unique_ptr<char[]> labels;
  std::unique_ptr<List[]> lists;
  std::unique_ptr<Sub[]> subs;
  uint16_t labelBytes = 0;
  uint8_t listCount = 0;
  uint16_t subCount = 0;
  // Lists, sublists or labels past the limits above were left out.
  bool truncated = false;

  std::string_view label(const uint16_t off, const uint8_t len) const { return {labels.get() + off, len}; }
};

enum class CustomListsResult : uint8_t { Missing, Ok, Malformed, OutOfMemory, ReadError };

CustomListsResult loadCustomLists(const char* path, CustomLists& out);

// FNV-1a over the file's bytes, never 0; 0 when there is no file.
uint32_t customListsHash(const char* path);

// How a tag is matched: its fold, hashed. Shared with the Tags lists so both
// merge spellings alike.
uint32_t foldedTagHash(std::string_view tag);

}  // namespace library
