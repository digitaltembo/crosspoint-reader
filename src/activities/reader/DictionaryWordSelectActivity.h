#pragma once

#include <BoardConfig.h>
#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <vector>

#include "activities/Activity.h"
#include "util/DictionaryLookup.h"

// Word selection over the current reader page: Left/Right step through words
// in reading order, Up/Down jump rows, Confirm looks the word up and opens
// DictionaryDefinitionActivity, Back returns to the reader. On touch devices a
// tap on another word moves the highlight, a tap on the highlighted word or its
// "Look Up" bubble looks it up, and a tap away from any word returns to the
// reader.
class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop) {}

  // Pre-selects the word under a screen point (the reader's long-press) before
  // the activity starts. False when no word is there.
  bool selectWordAt(int x, int y);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Screen box of one selectable word. `text` points into the owned Page's
  // TextBlock arena (NUL-terminated), valid for this activity's lifetime.
  struct WordBox {
    int16_t x;
    int16_t y;
    int16_t width;
    uint16_t row;
    const char* text;
    EpdFontFamily::Style style;
  };

  enum class Popup : uint8_t { None, Busy, NotFound, Error };

  // Tappable "Look Up" callout above the highlighted word, or below it when
  // the word sits too close to the top of the screen. pointerX is the tip of
  // the arrow, over the word's center.
  struct Bubble {
    int x;
    int y;
    int width;
    int height;
    int pointerX;
    bool below;
  };

  // A framebuffer rectangle saved into `snapshot` at `offset`.
  struct SavedRegion {
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;
    uint16_t offset = 0;
  };

  void prepareWords();
  void extractWords();
  int closestInRow(uint16_t row, int centerX) const;
  int wordAt(int x, int y) const;
  void moveVertical(int direction);
  void performLookup();
  bool drawHighlightWithSnapshot();
  Bubble lookupBubble() const;
  bool bubbleHit(int x, int y) const;
  void drawBubble(const Bubble& bubble) const;
  void drawHints() const;

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  int fontId = 0;
  int lineHeight = 0;

  std::vector<WordBox> words;
  int selected = 0;
  bool wordsReady = false;
  bool preselected = false;
  // Touch boards show a tappable "Look Up" bubble over the highlight.
  const bool showBubble = BoardConfig::hasTouch();
  uint16_t rowCount = 0;
  unsigned long lastHorizontalMoveTime = 0;

  // Shared with the definition viewer, which may switch it to another
  // dictionary; prepare() reopens the configured one on the next lookup.
  DictionaryLookup dictLookup;

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;

  // Differential highlight repaint: the pixels under the current highlight
  // box (and bubble, when shown), so a cursor move restores them and repaints
  // only the affected boxes instead of re-running the full two-pass page
  // render (which also reloads every SD-font glyph on the page). snapshotIdx
  // is the word whose under-pixels are saved; -1 means the framebuffer no
  // longer holds a clean page (popup drawn, sub-activity shown) and the next
  // render must be full. The bubble region is saved after the highlight is
  // drawn, so it is restored first.
  static constexpr size_t SNAPSHOT_CAPACITY = 4096;
  std::unique_ptr<uint8_t[]> snapshot;
  SavedRegion highlightRegion;
  SavedRegion bubbleRegion;
  bool bubbleSaved = false;
  int snapshotIdx = -1;
};
