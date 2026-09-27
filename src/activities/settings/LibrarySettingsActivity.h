#pragma once

#include <GfxRenderer.h>

#include <cstdint>

#include "activities/UiListActivity.h"

class MappedInputManager;

// How the library is indexed: whether book metadata is read, and which lists
// the index holds and shows. Changes are saved on exit; the Library screen
// notices the index was built with other options and rebuilds it.
class LibrarySettingsActivity final : public UiListActivity {
 public:
  explicit LibrarySettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("LibrarySettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;

  // Row 0 is "Use book metadata"; the rest are one list option each.
  static constexpr int LIST_ROWS = 7;

 private:
  static constexpr int ROW_COUNT = 1 + LIST_ROWS;

  int listCount() const override { return ROW_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  // The last shown list cannot be turned off.
  bool isLocked(int listRow) const;

  freeink::ui::ListItem rowItems[ROW_COUNT]{};
  uint8_t workingLists = 0;
  uint8_t workingMetadata = 0;
  bool edited = false;
};
