#pragma once
#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "./FileBrowserActivity.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "components/CoverGridHomeUi.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

class HomeActivity final : public Activity {
  std::unique_ptr<CoverGridHomeUi> coverGridUi;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  // The Plugins slot (index 2) only appears when a plugin is installed.
  bool hasPlugins = false;
  bool hasContinueReading = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  std::vector<RecentBook> recentBooks;

  // Card home (theme hasCardHome): recentBooks holds only the books that get
  // a card, followed in selectorIndex order by the icon bar's CARD_MENU items.
  bool cardHome = false;
  CardHomeLayout cardLayout;
  std::vector<int> cardProgress;  // read percentage per card, -1 unknown
  // Per-card cover snapshots (a few KB each) so selection repaints restore
  // covers instead of re-reading thumbnails from SD.
  std::array<std::unique_ptr<uint8_t[]>, CardHomeLayout::MAX_CARDS> cardCovers;
  std::array<size_t, CardHomeLayout::MAX_CARDS> cardCoverSizes{};
  static constexpr HomeMenuItem CARD_MENU[CardHomeLayout::ICON_COUNT] = {
      HomeMenuItem::SETTINGS_MENU, HomeMenuItem::FILE_TRANSFER, HomeMenuItem::LIBRARY};

  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;

  // Convert HomeMenuItem to menu index (used in onEnter)
  static int menuItemToIndex(HomeMenuItem item, bool hasPlugins) {
    int i = 0;
    if (item == HomeMenuItem::FILE_BROWSER) return i;
    ++i;
    if (item == HomeMenuItem::LIBRARY) return i;
    ++i;
    if (item == HomeMenuItem::PLUGINS) return hasPlugins ? i : 0;
    if (hasPlugins) ++i;
    if (item == HomeMenuItem::FILE_TRANSFER) return i;
    ++i;
    if (item == HomeMenuItem::SETTINGS_MENU) return i;
    return 0;
  }

  // Convert menu index to HomeMenuItem (used in loop)
  static HomeMenuItem indexToMenuItem(int idx, bool hasPlugins) {
    int i = 0;
    if (idx == i++) return HomeMenuItem::FILE_BROWSER;
    if (idx == i++) return HomeMenuItem::LIBRARY;
    if (hasPlugins && idx == i++) return HomeMenuItem::PLUGINS;
    if (idx == i++) return HomeMenuItem::FILE_TRANSFER;
    if (idx == i) return HomeMenuItem::SETTINGS_MENU;
    return HomeMenuItem::NONE;
  }
  int menuIndexOf(HomeMenuItem item) const;
  HomeMenuItem menuItemAt(int idx) const;

  void renderCardHome();
  bool handleCardHomeInput();
  void activateSelection();
  void drawCardCovers();
  void freeCardCovers();

  void onSelectBook(const std::string& path);
  void onFileBrowserOpen();
  void onLibraryOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onPluginsOpen();

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);
  void fillCoverGridFromLibrary();
  void resolveGridCoverPaths();
  void loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE, bool cleanInitialRefresh = false)
      : Activity("Home", renderer, mappedInput),
        initialMenuItem(initialMenuItemValue),
        cleanInitialRefresh(cleanInitialRefresh) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
};
