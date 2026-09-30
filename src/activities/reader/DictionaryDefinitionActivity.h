#pragma once

#include <Epub/Page.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"
#include "util/DictionaryLookup.h"
#include "util/DictionaryRegistry.h"

// Paged viewer for one dictionary definition. HTML definitions are laid out
// through the EPUB chapter parser into styled Pages; anything else (plain
// text, or HTML too damaged to parse) is word-wrapped once on entry and each
// page renders spans of the original string, so no per-line copies are held.
// With more than one dictionary installed, a tab per dictionary looks the same
// word up in that dictionary (tap a tab, or Confirm to cycle).
class DictionaryDefinitionActivity final : public Activity {
 public:
  // `dictLookup` is the caller's open dictionary, reused (and switched) by the
  // tabs; it must outlive this activity. `lookupWord` is the text that was
  // looked up and `dictionaryName` the folder that produced `definition`.
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        DictionaryLookup& dictLookup, std::string lookupWord,
                                        const char* dictionaryName, std::string headword, std::string definition,
                                        bool htmlDefinition = false)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        dictLookup(dictLookup),
        lookupWord(std::move(lookupWord)),
        initialDictionary(dictionaryName),
        headword(std::move(headword)),
        definition(std::move(definition)),
        htmlDefinition(htmlDefinition) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // One wrapped display line: a byte span of `definition`. Wrapping keeps
  // lines under the screen width, so uint16_t length is ample.
  struct Line {
    uint32_t start;
    uint16_t len;
  };

  // Usable body-text area between the header and the button hints.
  struct BodyArea {
    int width;
    int height;
  };

  bool showTabs() const { return dictionaries.size() > 1; }
  // Content column (right of a landscape hint gutter, below an inverted hint
  // bar) and the tab band inside it.
  Rect contentRect() const;
  Rect tabBarRect() const;
  void tabSlot(const Rect& bar, int index, int& x, int& width) const;
  int tabAt(int x, int y) const;
  // Y of the first body line, below the header and any tab band.
  int bodyTop() const;
  void switchTab(int index);
  void layoutDefinition();
  void clearContent();

  BodyArea bodyArea() const;
  bool layoutHtmlPages();
  void wrapText();
  int measureSpan(int fontId, const char* text, size_t len) const;
  void drawBody(int fontId, int x, int startY) const;
  void drawTabs(Rect rect) const;

  DictionaryLookup& dictLookup;
  // Not `word`: Arduino.h defines a word() macro.
  const std::string lookupWord;
  const std::string initialDictionary;
  std::vector<DictionaryEntry> dictionaries;
  int activeTab = 0;

  std::string headword;
  // onEnter() normalizes embedded NULs (StarDict multi-type separators) to
  // newlines so C-string APIs see the whole text.
  std::string definition;
  bool htmlDefinition;
  // Set when the active tab's dictionary has no definition: the body shows
  // this message instead.
  bool hasMessage = false;
  StrId message = StrId::STR_DICT_NOT_FOUND;
  // "Looking up…" / "Indexing…" popup while a tab switch blocks on SD.
  bool busy = false;
  StrId busyMessage = StrId::STR_DICT_LOOKING_UP;
  // Styled path: reader-identical Pages laid out from the HTML definition.
  // Empty means the plain-text span path below is active.
  std::vector<std::unique_ptr<Page>> pages;
  std::vector<Line> lines;
  int currentPage = 0;
  int totalPages = 1;
  int linesPerPage = 1;
  ButtonNavigator buttonNavigator;
};
