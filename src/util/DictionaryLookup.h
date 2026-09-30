#pragma once

#include <I18n.h>

#include <string>

#include "util/Dictionary.h"

// One open dictionary plus the open → index → lookup sequence and its
// user-facing failure messages, shared by word selection and the definition
// viewer's dictionary tabs. Reopens only when the requested folder changes.
class DictionaryLookup {
 public:
  // Opens folderName unless it is already open. True when the next lookup
  // must first build the index sidecars (slow), so the caller can show
  // "Indexing…" instead of "Looking up…".
  bool prepare(const char* folderName);

  // Looks word up in the dictionary last passed to prepare(). On a miss or
  // failure returns false with errorOut naming why (STR_DICT_NOT_FOUND for a
  // genuine miss).
  bool lookup(const char* word, std::string& definitionOut, std::string& headwordOut, StrId& errorOut);

  bool definitionsAreHtml() const { return dict.definitionsAreHtml(); }

 private:
  Dictionary dict;
  std::string openName;
  bool opened = false;  // open() has run for openName (success or failure)
  bool openOk = false;
  bool needsIndex = false;
};
