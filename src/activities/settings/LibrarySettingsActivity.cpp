#include "LibrarySettingsActivity.h"

#include <I18n.h>
#include <LibraryFormat.h>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {

struct ListRow {
  StrId label;
  uint8_t option;
};

constexpr ListRow LIST_OPTIONS[] = {
    {StrId::STR_LIBRARY_TAB_RECENT, library::CLIX_OPTION_RECENT},
    {StrId::STR_LIBRARY_TAB_TITLE, library::CLIX_OPTION_TITLE},
    {StrId::STR_LIBRARY_TAB_AUTHOR, library::CLIX_OPTION_AUTHOR},
    {StrId::STR_LIBRARY_TAB_SERIES, library::CLIX_OPTION_SERIES},
    {StrId::STR_LIBRARY_TAB_TAGS, library::CLIX_OPTION_TAGS},
    {StrId::STR_LIBRARY_LIST_FOLDERS, library::CLIX_OPTION_FOLDERS},
};

}  // namespace

static_assert(sizeof(LIST_OPTIONS) / sizeof(LIST_OPTIONS[0]) == 6, "one row per list option");

void LibrarySettingsActivity::onEnter() {
  UiListActivity::onEnter();
  workingMetadata = SETTINGS.libraryUseMetadata != 0 ? 1 : 0;
  workingLists = SETTINGS.libraryLists & library::CLIX_OPTIONS_ALL;
  if (workingLists == 0) workingLists = library::CLIX_OPTIONS_DEFAULT;
  edited = false;

  rowItems[0].label = tr(STR_LIBRARY_USE_METADATA);
  rowItems[0].actionValue = 0;
  for (int i = 0; i < LIST_ROWS; ++i) {
    rowItems[1 + i].label = I18N.get(LIST_OPTIONS[i].label);
    rowItems[1 + i].actionValue = static_cast<int16_t>(1 + i);
  }
  rowItems[1].sectionHeading = tr(STR_LIBRARY_LISTS);
}

void LibrarySettingsActivity::onExit() {
  if (edited && (workingMetadata != SETTINGS.libraryUseMetadata || workingLists != SETTINGS.libraryLists)) {
    SETTINGS.libraryUseMetadata = workingMetadata;
    SETTINGS.libraryLists = workingLists;
    SETTINGS.saveToFile();
  }
  Activity::onExit();
}

const char* LibrarySettingsActivity::headerTitle() const { return tr(STR_LIBRARY_SETTINGS); }

bool LibrarySettingsActivity::isLocked(const int listRow) const {
  const uint8_t option = LIST_OPTIONS[listRow].option;
  return (workingLists & option) != 0 && (workingLists & ~option & library::CLIX_OPTIONS_ALL) == 0;
}

void LibrarySettingsActivity::activateIndex(const int index) {
  nav.selected = index;
  // The row stays on screen with a new value; a lingering flash would gray an
  // unrelated row on the repaint below.
  app.clearTapFlash();
  if (index == 0) {
    workingMetadata ^= 1;
    edited = true;
  } else if (index > 0 && index <= LIST_ROWS && !isLocked(index - 1)) {
    workingLists ^= LIST_OPTIONS[index - 1].option;
    edited = true;
  }
  requestUpdate();
}

void LibrarySettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band GUI.drawHeader paints.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  rowItems[0].value = workingMetadata ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  for (int i = 0; i < LIST_ROWS; ++i) {
    rowItems[1 + i].value = (workingLists & LIST_OPTIONS[i].option) != 0 ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  }

  fui::ListProps props;
  props.items = rowItems;
  props.count = ROW_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  screen.list(props);
}
