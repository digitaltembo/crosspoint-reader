#include "LibraryListActivity.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibrarySearch.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/icons/headerIcons.h"
#include "components/icons/libraryListIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/search32.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr int SIDE_PADDING = 12;
constexpr unsigned long LONG_PRESS_MS = 1000;

constexpr uint16_t NO_LIST = 0xFFFF;

uint32_t labelHash(const std::string& label) {
  uint32_t hash = 2166136261u;  // FNV-1a 32
  for (const char c : label) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 16777619u;
  }
  return hash;
}

constexpr bool isDescending(const library::SortOrder order) {
  return order == library::SortOrder::RecentDesc || order == library::SortOrder::TitleDesc ||
         order == library::SortOrder::AuthorDesc;
}

constexpr bool isRecentSort(const library::SortOrder order) {
  return order == library::SortOrder::RecentAsc || order == library::SortOrder::RecentDesc;
}

constexpr bool isAuthorSort(const library::SortOrder order) {
  return order == library::SortOrder::AuthorAsc || order == library::SortOrder::AuthorDesc;
}

constexpr library::SortOrder orderForList(const uint16_t id, const uint8_t descendingBuiltins) {
  const bool descending = (descendingBuiltins & (1u << id)) != 0;
  if (id == library::CLIX_TITLE_LIST) return descending ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
  if (id == library::CLIX_AUTHOR_LIST) {
    return descending ? library::SortOrder::AuthorDesc : library::SortOrder::AuthorAsc;
  }
  return descending ? library::SortOrder::RecentDesc : library::SortOrder::RecentAsc;
}

// Lists the firmware names: the built-in ones and the generated top-level
// ones. Every other list carries its own label.
const char* fixedLabel(const uint8_t role) {
  switch (role) {
    case library::CLIX_ROLE_RECENT:
      return tr(STR_LIBRARY_TAB_RECENT);
    case library::CLIX_ROLE_TITLE:
      return tr(STR_LIBRARY_TAB_TITLE);
    case library::CLIX_ROLE_AUTHOR:
      return tr(STR_LIBRARY_TAB_AUTHOR);
    case library::CLIX_ROLE_SERIES:
      return tr(STR_LIBRARY_TAB_SERIES);
    case library::CLIX_ROLE_TAGS:
      return tr(STR_LIBRARY_TAB_TAGS);
    case library::CLIX_ROLE_FOLDERS:
      return tr(STR_LIBRARY_TAB_FOLDERS);
    default:
      return nullptr;
  }
}

fui::BitmapRef listIconBitmap(const library::ClixListIcon icon) {
  switch (icon) {
    case library::CLIX_ICON_FOLDER:
      return fui::bitmapFromIcon(icon_folder_32);
    case library::CLIX_ICON_FOLDER_TREE:
      return fui::bitmapFromIcon(icon_folder_tree_32);
    case library::CLIX_ICON_BOOK:
      return fui::bitmapFromIcon(icon_book_32);
    case library::CLIX_ICON_BOOKS:
      return fui::bitmapFromIcon(icon_library_32);
    case library::CLIX_ICON_RECENT:
      return fui::bitmapFromIcon(icon_history_32);
    case library::CLIX_ICON_TITLE:
      return fui::bitmapFromIcon(icon_arrow_down_a_z_32);
    case library::CLIX_ICON_AUTHOR:
      return fui::bitmapFromIcon(icon_user_32);
    case library::CLIX_ICON_SERIES:
      return fui::bitmapFromIcon(icon_library_big_32);
    case library::CLIX_ICON_SERIES_ENTRY:
      return fui::bitmapFromIcon(icon_book_copy_32);
    case library::CLIX_ICON_TAGS:
      return fui::bitmapFromIcon(icon_tags_32);
    case library::CLIX_ICON_TAG:
      return fui::bitmapFromIcon(icon_tag_32);
    case library::CLIX_ICON_BOOKMARK:
      return fui::bitmapFromIcon(icon_bookmark_32);
    case library::CLIX_ICON_STAR:
      return fui::bitmapFromIcon(icon_star_32);
    case library::CLIX_ICON_HEART:
      return fui::bitmapFromIcon(icon_heart_32);
    default:
      return fui::bitmapFromIcon(icon_list_32);
  }
}

const char* builtinLabel(const uint16_t id) { return fixedLabel(static_cast<uint8_t>(id + 1)); }

}  // namespace

LibraryListActivity::LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("Library", renderer, mappedInput, true) {
  // Three short tab labels: a full-slot pill would stretch across a third of
  // the screen, so cap it at the label plus padding (slots stay put).
  tabPillMaxPad = 16;
}

void LibraryListActivity::onEnter() {
  // One lock across the base lifecycle AND the data phase: the base onEnter
  // schedules a paint, and the render task must not read the index or the
  // filter before they are in place. The rebuild also needs the lock: the
  // render task's SD-loaded fonts read glyph data at draw time, and the walk
  // needs the card to itself.
  RenderLock lock(*this);

  // Recent is backed by the resident store. Prune before opening the index so
  // its persistence write never overlaps the long-lived index reader.
  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();

  // Rebuild when the index is missing, invalid, or was built with other
  // library settings. Otherwise entering the screen stays instant. This runs
  // before the base onEnter because the index decides how many tabs there are.
  const bool readMetadata = SETTINGS.libraryUseMetadata != 0;
  const bool rebuildNeeded = library::isLibraryIndexDirty() || !index.open(library::libraryIndexPath()) ||
                             index.header().metadataEnabled != readMetadata ||
                             index.header().listOptions != (SETTINGS.libraryLists & library::CLIX_OPTIONS_ALL);
  if (rebuildNeeded) {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    rebuildIndex();
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  }
  loadTopLists();

  UiTabListActivity::onEnter();
  app.on(ACTION_SEARCH, &LibraryListActivity::searchActionTrampoline, this);
  app.on(ACTION_REBUILD, &LibraryListActivity::rebuildActionTrampoline, this);
  app.on(ACTION_BACK, &LibraryListActivity::backActionTrampoline, this);

  degraded = index.isOpen() && index.ranksDegraded();
  if (index.isOpen() && index.dedupDegraded()) {
    LOG_ERR("LIB", "index was built without duplicate detection");
  }
  resolvePinned();
  // Selects the last list's row, now that the base has reset the navigation.
  if (pickerOpen) openPicker();

  // Entered while Confirm was still held (typical when launched from the home
  // menu): ignore its release, or we would open whatever sits at row 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate(true);
}

void LibraryListActivity::onExit() {
  index.close();
  topLists.reset();
  Activity::onExit();
}

void LibraryListActivity::loadTopLists() {
  topLists.reset();
  topCount = 0;
  depth = 0;
  const uint16_t total = index.listCount();
  if (total > 0) topLists = makeUniqueNoThrow<uint16_t[]>(total);
  if (!topLists) {
    // No index, or no memory for the table: the built-in tabs still work
    // (topListAt maps them by position), reading nothing past them.
    if (total > 0) LOG_ERR("LIB", "OOM: %u-byte list table", static_cast<unsigned>(total * sizeof(uint16_t)));
    pickerMode = false;
    pickerOpen = false;
    topCount = library::CLIX_BUILTIN_LISTS;
    for (uint16_t i = 0; i < topCount; i++) tabLabels[i].clear();
    activeTabIndex = 0;
    openList(topListAt(0), false);
    return;
  }
  for (uint16_t id = 0; id < total; id++) {
    library::ClixListDesc list{};
    if (index.readList(id, list) && (list.flags & library::CLIX_LIST_TOP_LEVEL) != 0) topLists[topCount++] = id;
  }
  // The settings keep one list shown; an index that hides every list still
  // shows Title.
  if (topCount == 0) topLists[topCount++] = library::CLIX_TITLE_LIST;
  pickerMode = topCount > MAX_LIST_TABS;
  pickerOpen = pickerMode;
  activeTabIndex = 0;
  if (!pickerMode) {
    for (uint16_t i = 0; i < topCount; i++) {
      tabLabels[i].clear();
      library::ClixListDesc list{};
      if (topLists[i] >= library::CLIX_BUILTIN_LISTS && index.readList(topLists[i], list)) {
        listLabel(list, tabLabels[i]);
      }
    }
    openList(topLists[0], false);
    return;
  }

  // The picker opens on the list chosen last time when it is still the same
  // list.
  openList(topLists[0], false);
  const uint16_t saved = APP_STATE.libraryListId;
  for (uint16_t i = 0; i < topCount; i++) {
    if (topLists[i] != saved) continue;
    openList(saved, false);
    if (labelHash(currentLabel) != APP_STATE.libraryListLabelHash) openList(topLists[0], false);
    break;
  }
}

void LibraryListActivity::listLabel(const library::ClixListDesc& list, std::string& out) {
  const char* fixed = fixedLabel(list.role);
  if (fixed) {
    out = fixed;
  } else if (!index.readListLabel(list, out) || out.empty()) {
    out = tr(STR_LIBRARY_UNKNOWN_TITLE);
  }
}

void LibraryListActivity::openList(const uint16_t id, const bool descending) {
  currentList = id;
  currentDescending = descending;
  currentDesc = library::ClixListDesc{};
  currentLabel.clear();
  if (!index.readList(id, currentDesc)) {
    // A list that no longer validates shows as empty rather than as garbage.
    currentDesc = library::ClixListDesc{library::CLIX_LIST_BOOKS, 0, 0, 0, 0, 0, 0, 0};
  }
  if (builtinView()) {
    currentLabel = builtinLabel(id);
    sortOrder = orderForList(id, descendingBuiltins);
  } else {
    listLabel(currentDesc, currentLabel);
  }
}

void LibraryListActivity::openPicker() {
  const uint16_t top = depth > 0 ? levels[0].listId : currentList;
  pickerOpen = true;
  depth = 0;
  query.clear();
  applyFilter();
  auto& nav = activeNav();
  nav.reset(1);
  for (uint16_t i = 0; i < topCount; i++) {
    if (topListAt(i) == top) nav.reset(i + 1);
  }
  requestUpdate();
}

void LibraryListActivity::chooseTopList(const int entry) {
  if (entry < 0 || entry >= topCount) return;
  const uint16_t id = topListAt(entry);
  pickerOpen = false;
  depth = 0;
  openList(id, false);
  const uint32_t hash = labelHash(currentLabel);
  if (APP_STATE.libraryListId != id || APP_STATE.libraryListLabelHash != hash) {
    APP_STATE.libraryListId = id;
    APP_STATE.libraryListLabelHash = hash;
    // The index handle is the card's one reader; release it around the write,
    // with the render task kept off the closed index.
    RenderLock lock(*this);
    index.close();
    APP_STATE.saveToFile();
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
  }
  applyFilter();
  refreshOverlap();
  activeNav().reset(1);
  requestUpdate();
}

void LibraryListActivity::pushList(const uint16_t child) {
  if (depth >= MAX_LIST_DEPTH) {
    LOG_ERR("LIB", "list nesting deeper than %u levels not shown", static_cast<unsigned>(MAX_LIST_DEPTH));
    return;
  }
  levels[depth++] = Level{currentList, currentDescending, activeNav()};
  openList(child, false);
  activeNav().reset(1);
  requestUpdate();
}

void LibraryListActivity::popList() {
  if (depth == 0) return;
  const Level level = levels[--depth];
  openList(level.listId, level.descending);
  activeNav() = level.nav;
  activeNav().followOnBuild = true;
  requestUpdate();
}

uint16_t LibraryListActivity::listIdAt(const int entry) {
  if (entry < 0) return NO_LIST;
  if (pickerView()) return entry < topCount ? topListAt(entry) : NO_LIST;
  const uint16_t value = index.entryAt(currentList, currentDesc, static_cast<uint16_t>(entry), currentDescending);
  if (currentDesc.kind != library::CLIX_LIST_MIXED) return value;
  return value != NO_LIST && (value & library::CLIX_ENTRY_LIST_BIT) != 0
             ? static_cast<uint16_t>(value & ~library::CLIX_ENTRY_LIST_BIT)
             : NO_LIST;
}

bool LibraryListActivity::rowIsList(const int entry) {
  if (listRowsView()) return true;
  if (!mixedView() || entry < 0) return false;
  const uint16_t value = index.entryAt(currentList, currentDesc, static_cast<uint16_t>(entry), currentDescending);
  return value != NO_LIST && (value & library::CLIX_ENTRY_LIST_BIT) != 0;
}

bool LibraryListActivity::mixedView() const {
  return !pickerOpen && query.empty() && !builtinView() && currentDesc.kind == library::CLIX_LIST_MIXED;
}

library::ClixListIcon LibraryListActivity::listRowText(const uint16_t id, std::string& label, std::string& subtitle) {
  label.clear();
  subtitle.clear();
  library::ClixListDesc list{};
  if (id == NO_LIST || !index.readList(id, list)) {
    label = tr(STR_LIBRARY_UNKNOWN_TITLE);
    return library::CLIX_ICON_LIST;
  }
  listLabel(list, label);
  // A folder's size mixes subfolders and books, so it is left unsaid.
  if (list.kind != library::CLIX_LIST_MIXED) {
    char count[32];
    snprintf(count, sizeof(count),
             list.kind == library::CLIX_LIST_GROUPS ? tr(STR_LIBRARY_LIST_COUNT) : tr(STR_LIBRARY_BOOK_COUNT),
             static_cast<int>(list.entryCount));
    subtitle = count;
  }
  return library::listIcon(list);
}

bool LibraryListActivity::groupsView() const {
  return !pickerOpen && query.empty() && !builtinView() && currentDesc.kind == library::CLIX_LIST_GROUPS;
}

bool LibraryListActivity::recentView() const { return !pickerOpen && builtinView() && isRecentSort(sortOrder); }

bool LibraryListActivity::authorView() const { return !pickerOpen && builtinView() && isAuthorSort(sortOrder); }

bool LibraryListActivity::viewDescending() const { return builtinView() ? isDescending(sortOrder) : currentDescending; }

library::SortOrder LibraryListActivity::lookupOrder() const {
  return !pickerOpen && builtinView() ? sortOrder : library::SortOrder::TitleAsc;
}

uint16_t LibraryListActivity::ordinalAt(const int entry) {
  const uint16_t row = static_cast<uint16_t>(rowFor(entry));
  if (!query.empty() || (!pickerOpen && builtinView())) return index.ordinalForRow(lookupOrder(), row);
  if (pickerOpen || currentDesc.kind == library::CLIX_LIST_GROUPS) return NO_LIST;
  const uint16_t value = index.entryAt(currentList, currentDesc, row, currentDescending);
  // A Mixed list's list entries are not books.
  if (currentDesc.kind == library::CLIX_LIST_MIXED && value != NO_LIST && (value & library::CLIX_ENTRY_LIST_BIT) != 0) {
    return NO_LIST;
  }
  return value;
}

bool LibraryListActivity::rebuildIndex() {
  library::BuildStats stats;
  const bool ok = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0, SETTINGS.libraryLists);
  if (!ok) {
    LOG_ERR("LIB", "index build failed");
    return false;
  }
  LOG_INF("LIB", "reconciled: %u unchanged, %u added, %u renamed, %u removed, %u enriched (%u dup, %u unreadable)",
          static_cast<unsigned>(stats.unchanged), static_cast<unsigned>(stats.added),
          static_cast<unsigned>(stats.renamed), static_cast<unsigned>(stats.removed),
          static_cast<unsigned>(stats.enriched), static_cast<unsigned>(stats.duplicatesDropped),
          static_cast<unsigned>(stats.unreadableSkipped));
  if (stats.dedupDegraded) LOG_ERR("LIB", "rebuild completed without duplicate detection");
  return true;
}

void LibraryListActivity::swallowHeldReleases() {
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  lockNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
}

int LibraryListActivity::selectedEntry() const {
  const int entry = ringPos() - 1;
  return entry < 0 ? 0 : entry;
}

// The pinned overlay applies only to the shelf that reads as "what am I up
// to": the unfiltered Recent sort, newest first. A search result is a flat
// list the reader narrowed down on purpose, and the ascending toggle asks for
// oldest-first, which pinned fresh reads would contradict.
int LibraryListActivity::pinnedCount() const {
  if (!recentView() || !query.empty() || !isDescending(sortOrder)) return 0;
  return pinnedTotal;
}

void LibraryListActivity::resolvePinned() {
  const auto& books = RECENT_BOOKS.getBooks();
  pinnedTotal = static_cast<uint8_t>(std::min<size_t>(books.size(), RecentBooksStore::MAX_RECENT_BOOKS));
  for (int i = 0; i < pinnedTotal; i++) pinnedAscRows[i] = 0xFFFF;
  if (pinnedTotal > 0 && index.isOpen()) {
    library::BookIdentity identities[RecentBooksStore::MAX_RECENT_BOOKS];
    for (int i = 0; i < pinnedTotal; i++) {
      const std::string& path = books[static_cast<size_t>(i)].path;
      identities[i].pathHash = library::clixPathHash(path.data(), path.size());
      // Size is only a lookup prefilter; 0 (stat failed, e.g. the index handle
      // is the card's one open reader) falls back to hash-only matching.
      identities[i].fileSize = 0;
      HalFile file;
      if (Storage.openFileForRead("LIB", path.c_str(), file)) {
        identities[i].fileSize = static_cast<uint32_t>(file.fileSize());
      }
    }
    if (!index.recentRowsFor(identities, pinnedTotal, pinnedAscRows)) {
      // Without the match the overlay would duplicate every pinned book that is
      // also in the index; better to drop the pins than to show doubles.
      LOG_ERR("LIB", "recent-book lookup failed; overlay disabled");
      pinnedTotal = 0;
    }
  }
  refreshOverlap();
}

void LibraryListActivity::refreshOverlap() {
  overlapCount = 0;
  const int total = static_cast<int>(index.bookCount());
  for (int i = 0; i < pinnedTotal; i++) {
    if (pinnedAscRows[i] == 0xFFFF || pinnedAscRows[i] >= total) continue;
    const uint16_t row =
        isDescending(sortOrder) ? static_cast<uint16_t>(total - 1 - pinnedAscRows[i]) : pinnedAscRows[i];
    overlapRows[overlapCount++] = row;
  }
  std::sort(overlapRows, overlapRows + overlapCount);
}

void LibraryListActivity::openSelectedBook() {
  std::string path;
  if (selectedEntry() < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (selectedEntry() >= static_cast<int>(books.size())) return;
    path = books[static_cast<size_t>(selectedEntry())].path;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = ordinalAt(selectedEntry());
    if (ordinal == 0xFFFF) return;

    library::ClixRecord record{};
    if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
      LOG_ERR("LIB", "cannot resolve path for row %d", selectedEntry());
      return;
    }
  }
  openBookByPath(path);
}

// Shared by row activation and the options menu: the reader screen this opens
// has its own surfaces; a lingering tap flash would gray an unrelated element
// there. The index handle is released first — on hardware only one reader can
// hold a file open at a time, and the reader is about to open files of its own.
void LibraryListActivity::openBookByPath(const std::string& path) {
  app.clearTapFlash();
  index.close();
  onSelectBook(path);
}

void LibraryListActivity::activateIndex(const int index) {
  if (pickerView()) {
    chooseTopList(index);
  } else if (groupsCollapsed) {
    expandGroup(index);
  } else if (rowIsList(index)) {
    const uint16_t child = listIdAt(index);
    if (child != NO_LIST) pushList(child);
  } else {
    openSelectedBook();
  }
}

// Row long-press prompts delete wherever grouping does not own the gesture:
// an active search is already a flat list the reader narrowed down on purpose
// ("find it, hold it, delete it"). Unfiltered Title/Author lists keep
// collapse-to-groups. The Recent shelf always opens the row options menu.
bool LibraryListActivity::deleteEligible() const {
  return !groupsCollapsed && !listRowsView() && (!query.empty() || !groupable());
}

void LibraryListActivity::onRowLongPress(const int index) {
  if (rowIsList(index)) {
    activateIndex(index);
  } else if (recentView()) {
    showRecentBookOptions(index);
  } else if (deleteEligible()) {
    promptDeleteBook(index);
  } else if (!groupsCollapsed && groupable()) {
    collapseGroups(index);
  } else {
    activateIndex(index);
  }
}

// Recent-shelf long-press menu (button hold and touch long-press). The first
// rows may come from RecentBooksStore; the rest are index rows sorted by
// modification time. Only store rows can be removed from recents.
void LibraryListActivity::showRecentBookOptions(const int entry) {
  if (entry < 0 || entry >= listCount()) return;

  std::string path;
  std::string title;
  const bool isStoreRow = entry < pinnedCount();
  if (isStoreRow) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry >= static_cast<int>(books.size())) return;
    path = books[static_cast<size_t>(entry)].path;
    title = books[static_cast<size_t>(entry)].title;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = ordinalAt(entry);
    library::ClixRecord record{};
    std::string author;
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path) ||
        !rowTextFor(entry, title, author)) {
      LOG_ERR("LIB", "cannot resolve Recent row %d", entry);
      return;
    }
  }

  const char* STORE_OPTIONS[] = {tr(STR_OPEN), tr(STR_REMOVE_FROM_RECENTS), tr(STR_DELETE), tr(STR_LIBRARY_REBUILD)};
  const char* INDEX_OPTIONS[] = {tr(STR_OPEN), tr(STR_DELETE), tr(STR_LIBRARY_REBUILD)};
  app.clearTapFlash();
  optionPopup.show(tr(STR_LIBRARY), title.c_str(), isStoreRow ? STORE_OPTIONS : INDEX_OPTIONS, isStoreRow ? 4 : 3, 0,
                   [this, path, title, isStoreRow](const int choice) {
                     swallowHeldReleases();
                     switch (choice) {
                       case 0:
                         openBookByPath(path);
                         break;
                       case 1:
                         if (isStoreRow) {
                           promptRemoveRecentBook(path, title);
                         } else {
                           promptDeleteBookByPath(path, title);
                         }
                         break;
                       case 2:
                         if (isStoreRow)
                           promptDeleteBookByPath(path, title);
                         else
                           promptRebuildIndex();
                         break;
                       case 3:
                         if (isStoreRow) promptRebuildIndex();
                         break;
                       default:
                         break;
                     }
                   });
  requestUpdate();
}

// Manual index refresh, same card discipline as the onEnter rebuild: the walk
// wants the card to itself, and the render task must not read the index (or
// the filter) around it.
void LibraryListActivity::promptRebuildIndex() {
  RenderLock lock(*this);
  GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
  index.close();
  rebuildIndex();
  if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  resetAfterRebuild();
  requestUpdate(true);
}

void LibraryListActivity::resetAfterRebuild() {
  // List ids survive a rebuild unless the external lists were dropped. Counts
  // change, so every level's descriptor is reread; a vanished list returns
  // to the top level.
  bool listsKept = currentList < index.listCount() || !index.isOpen() || builtinView();
  for (uint8_t i = 0; i < depth; i++) listsKept = listsKept && levels[i].listId < index.listCount();
  if (listsKept) {
    openList(currentList, currentDescending);
  } else {
    depth = 0;
    pickerOpen = pickerMode;
    openList(topListAt(0), false);
  }
  // Sort positions, group starts, and pinned rows all point into the old order.
  applyFilter();
  resolvePinned();
  auto& nav = activeNav();
  const int count = listCount();
  if (count == 0) {
    nav.selected = 0;
  } else if (nav.selected > count) {
    nav.selected = count;
  }
  nav.followOnBuild = true;
}

void LibraryListActivity::promptRemoveRecentBook(const std::string& path, const std::string& title) {
  const bool reopenIndex = index.isOpen();
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: recent removal confirmation");
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path, reopenIndex](const ActivityResult& result) {
    swallowHeldReleases();
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    if (!result.isCancelled && RECENT_BOOKS.removeByPath(path)) {
      resolvePinned();
      closeRouting();
      auto& nav = activeNav();
      const int count = listCount();
      if (count == 0) {
        nav.selected = 0;
      } else if (nav.selected > count) {
        nav.selected = count;
      }
      nav.followOnBuild = true;
    }
  });
}

void LibraryListActivity::promptDeleteBook(const int entry) {
  if (!index.isOpen() || entry < 0 || entry >= bookRowCount()) return;
  const uint16_t ordinal = ordinalAt(entry);
  if (ordinal == 0xFFFF) return;

  std::string path;
  library::ClixRecord record{};
  if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
    LOG_ERR("LIB", "cannot resolve path for row %d", entry);
    return;
  }
  std::string title;
  std::string author;
  rowTextFor(entry, title, author);
  promptDeleteBookByPath(path, title);
}

void LibraryListActivity::promptDeleteBookByPath(const std::string& path, const std::string& title) {
  // The dialog and the delete both want the card; reopen when we resume.
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE) + std::string("? "), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: delete confirmation");
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path](const ActivityResult& result) {
    swallowHeldReleases();
    {
      // Same lock rationale as onEnter: the walk wants the card to itself, and
      // the render task must not read the index (or the filter) around the
      // rebuild.
      RenderLock lock(*this);
      if (!result.isCancelled) {
        LOG_DBG("LIB", "deleting %s", path.c_str());
        clearBookCache(path);
        if (!Storage.remove(path.c_str())) LOG_ERR("LIB", "cannot delete %s", path.c_str());
        if (RECENT_BOOKS.removeByPath(path)) RECENT_BOOKS.saveToFile();
        GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
        rebuildIndex();
      }
      if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
      if (!result.isCancelled) {
        resetAfterRebuild();
      }
    }
    if (!result.isCancelled) {
      closeRouting();
      requestUpdate(true);
    }
  });
}

void LibraryListActivity::openSearch() {
  app.clearTapFlash();
  // No key filtering here on purpose. Greying out the letters that lead nowhere
  // was built, tested on device and removed: a letter you can see but cannot
  // reach reads as a broken keyboard, and the eye keeps returning to it.
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LIBRARY_SEARCH), query, 48,
                                                           InputType::Text);
  if (!keyboard) {
    LOG_ERR("LIB", "OOM: search keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    swallowHeldReleases();
    if (result.isCancelled) return;
    query = std::get<KeyboardResult>(result.data).text;
    applyFilter();
    auto& nav = activeNav();
    if (!query.empty() && filteredCount == 0 && !degraded) {
      // Up from the tab bar reopens Search even with no results.
      nav.selected = 0;
    } else {
      // A non-empty result belongs to the list: land on
      // its first surviving row, not on the strip.
      nav.selected = 1;
    }
    nav.top = 0;
    requestUpdate();
  });
}

void LibraryListActivity::stepTab(const int direction) {
  // The picker's single tab has nowhere to step to.
  if (pickerMode) return;
  const int next = (activeTab() + (direction > 0 ? 1 : topCount - 1)) % topCount;
  selectTab(next, false);
}

void LibraryListActivity::onTabAction(const int index) {
  app.clearTapFlash();
  // In picker mode the only "tab" is the header title.
  if (pickerMode) {
    toggleSortDirection();
    return;
  }
  selectTab(index, true);
}

// Choosing another tab opens its list at the top level. The active tab toggles
// the current list's direction, or first returns to the tab's own list when a
// child list is open.
void LibraryListActivity::selectTab(const int index, const bool toggleIfActive) {
  if (pickerMode || index < 0 || index >= topCount) return;
  if (index != activeTab() || depth > 0) {
    depth = 0;
    openList(topListAt(index), false);
  } else if (toggleIfActive) {
    if (builtinView()) {
      descendingBuiltins ^= static_cast<uint8_t>(1u << currentList);
      sortOrder = orderForList(currentList, descendingBuiltins);
    } else {
      currentDescending = !currentDescending;
    }
  }
  // The filter and the overlap rows hold positions in the old order, so they
  // must be rebuilt.
  applyFilter();
  activeTabIndex = index;
  refreshOverlap();
  // Tab changes happen only while the bar owns focus. A tab's remembered row
  // must not pull focus back into the list after the switch.
  auto& nav = activeNav();
  nav.selected = 0;
  nav.top = 0;
  requestUpdate();
}

void LibraryListActivity::toggleSortDirection() {
  if (!pickerMode) {
    selectTab(activeTab(), true);
    return;
  }
  if (!sortTitleActive()) return;
  if (builtinView()) {
    descendingBuiltins ^= static_cast<uint8_t>(1u << currentList);
    sortOrder = orderForList(currentList, descendingBuiltins);
  } else {
    currentDescending = !currentDescending;
  }
  applyFilter();
  refreshOverlap();
  auto& nav = activeNav();
  nav.selected = 0;
  nav.top = 0;
  requestUpdate();
}

int LibraryListActivity::tabCount() const { return pickerMode ? 1 : topCount; }

int LibraryListActivity::activeTab() const { return pickerMode ? 0 : activeTabIndex; }

const char* LibraryListActivity::tabLabel(const int index) const {
  // Picker mode draws no strip; its title is buildTitleControl's.
  if (pickerMode) return titleText.c_str();
  const uint16_t id = topListAt(index);
  return id < library::CLIX_BUILTIN_LISTS ? builtinLabel(id) : tabLabels[index].c_str();
}

fui::TabIndicator LibraryListActivity::tabIndicator(const int index) const {
  if (index != activeTab() || pickerOpen || groupsView()) return fui::TabIndicator::None;
  return viewDescending() ? fui::TabIndicator::Down : fui::TabIndicator::Up;
}

int LibraryListActivity::bookRowCount() const {
  if (!query.empty()) return static_cast<int>(filteredCount);
  if (pickerOpen) return topCount;
  if (!builtinView()) return currentDesc.entryCount;
  // Pinned books already in the index are skipped below the pins, not doubled;
  // pinned books the index missed still show, so the difference stays split.
  const int pinned = pinnedCount();
  return static_cast<int>(index.bookCount()) + (pinned > 0 ? pinned - overlapCount : 0);
}

int LibraryListActivity::listCount() const { return groupsCollapsed ? static_cast<int>(groupCount) : bookRowCount(); }

// Entry position on screen to row position in the sort order. Identity while
// unfiltered and unpinned, so the shelf costs nothing when nothing is typed.
// With pins active, entries below pinnedCount() belong to the store and must
// not reach this; the rest walk past the pinned books' own sort rows.
int LibraryListActivity::rowFor(const int entry) const {
  if (!query.empty()) {
    if (entry < 0 || entry >= static_cast<int>(filteredCount) || !filtered) return 0;
    return filtered[entry];
  }
  const int pinned = pinnedCount();
  if (pinned == 0) return entry;
  int row = entry - pinned;
  for (int i = 0; i < overlapCount; i++) {
    if (overlapRows[i] <= row) row++;
  }
  return row;
}

// Grouping reads the order of the built-in Title and Author lists; external
// lists show flat.
bool LibraryListActivity::groupable() const {
  return !degraded && !pickerOpen && builtinView() && !isRecentSort(sortOrder) && bookRowCount() > 0;
}

uint32_t LibraryListActivity::titleInitialFor(const int entry) {
  const uint16_t ordinal = ordinalAt(entry);
  library::ClixRecord record{};
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) return 0;
  return library::foldedGroupInitial(std::string_view(record.fold, record.foldLen));
}

bool LibraryListActivity::buildGroupStarts() {
  const int count = bookRowCount();
  if (count <= 0) return false;
  if (groupCapacity < count) {
    auto starts = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(count));
    if (!starts) {
      LOG_ERR("LIB", "cannot allocate %u-byte group map", static_cast<unsigned>(count * sizeof(uint16_t)));
      return false;
    }
    groupStarts = std::move(starts);
    groupCapacity = static_cast<uint16_t>(count);
  }

  groupCount = 0;
  uint32_t previousInitial = 0;
  std::string previousAuthor;
  std::string title;
  std::string author;
  previousAuthor.reserve(128);
  title.reserve(128);
  author.reserve(128);
  for (int entry = 0; entry < count; entry++) {
    bool startsGroup = entry == 0;
    if (authorView()) {
      rowTextFor(entry, title, author);
      startsGroup = startsGroup || author != previousAuthor;
      previousAuthor = author;
    } else {
      const uint32_t initial = titleInitialFor(entry);
      startsGroup = startsGroup || initial != previousInitial;
      previousInitial = initial;
    }
    if (startsGroup) groupStarts[groupCount++] = static_cast<uint16_t>(entry);
  }
  LOG_DBG("LIB", "group map: %u groups, %u bytes", static_cast<unsigned>(groupCount),
          static_cast<unsigned>(groupCapacity * sizeof(uint16_t)));
  return groupCount > 0;
}

int LibraryListActivity::groupForBook(const int bookEntry) const {
  int group = 0;
  while (group + 1 < groupCount && groupStarts[group + 1] <= bookEntry) group++;
  return group;
}

bool LibraryListActivity::collapseGroups(const int bookEntry) {
  if (!groupable() || !buildGroupStarts()) return false;
  expandedNav = activeNav();
  groupsCollapsed = true;
  auto& nav = activeNav();
  nav.reset(groupForBook(bookEntry) + 1);
  requestUpdate();
  return true;
}

void LibraryListActivity::expandGroup(const int groupEntry) {
  if (!groupsCollapsed || groupEntry < 0 || groupEntry >= groupCount) return;
  const int bookEntry = groupStarts[groupEntry];
  groupsCollapsed = false;
  activeNav() = expandedNav;
  auto& nav = activeNav();
  nav.selected = bookEntry + 1;
  nav.top = bookEntry;
  nav.followOnBuild = true;
  requestUpdate();
}

void LibraryListActivity::restoreExpandedList() {
  if (!groupsCollapsed) return;
  groupsCollapsed = false;
  activeNav() = expandedNav;
  requestUpdate();
}

// One pass over the sort order, keeping what matches (library::filterRows). The
// result array is allocated once with the exact upper bound and fails back to an
// explicit message rather than letting vector growth abort the firmware.
void LibraryListActivity::applyFilter() {
  groupsCollapsed = false;
  groupCount = 0;
  filtered.reset();
  filteredCount = 0;
  filterFailed = false;
  // The header shows the active query in place of the screen title, so the
  // reader can see what narrowed the list without reopening the keyboard.
  headerSearchTitle = query.empty() ? std::string() : "“" + query + "”";
  if (query.empty()) return;

  const int total = static_cast<int>(index.bookCount());
  if (total <= 0) return;

  auto matches = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(total));
  if (!matches) {
    LOG_ERR("LIB", "cannot allocate %u-byte search result buffer", static_cast<unsigned>(total * sizeof(uint16_t)));
    filterFailed = true;
    return;
  }

  // Search always covers the whole library, whatever list it started from.
  filteredCount = library::filterRows(index, lookupOrder(), query, matches.get());
  filtered = std::move(matches);
}

// Staged back-out, shared by the Back button and the header's back arrow:
// clear the search, expand collapsed groups, leave a child list, return to the
// list picker (or return focus to the tabs), then leave for home.
void LibraryListActivity::handleBackAction() {
  auto& nav = activeNav();
  if (!query.empty()) {
    query.clear();
    applyFilter();
    nav.selected = 0;
    nav.top = 0;
    requestUpdate();
  } else if (groupsCollapsed) {
    restoreExpandedList();
  } else if (depth > 0) {
    popList();
  } else if (pickerMode && !pickerOpen) {
    openPicker();
  } else if (!pickerMode && !tabsFocused() && !degraded) {
    // Keep the current list and viewport while returning focus to the tabs.
    nav.selected = 0;
    requestUpdate();
  } else {
    onGoHome();
  }
}

void LibraryListActivity::searchActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->openSearch();
}

void LibraryListActivity::backActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->handleBackAction();
}

void LibraryListActivity::rebuildActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->promptRebuildIndex();
}

// Title and author for one entry, read straight from the index. Only ever
// called for rows about to be drawn, so at most a screenful of strings exists
// at once.
bool LibraryListActivity::rowTextFor(const int entry, std::string& title, std::string& author, std::string* fileName) {
  title.clear();
  author.clear();
  if (fileName) fileName->clear();
  if (entry < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry < 0 || entry >= static_cast<int>(books.size())) return false;
    const auto& book = books[static_cast<size_t>(entry)];
    title = book.title;
    author = book.author;
    if (fileName) *fileName = book.path;
    return true;
  }
  const uint16_t ordinal = ordinalAt(entry);
  library::ClixRecord record{};
  if (ordinal != 0xFFFF && index.readRecord(ordinal, record)) {
    // The build already decided both fields — from the book's own metadata when
    // it has any, and with one spelling chosen per author across the library.
    // Re-parsing the name here would throw that away, and only works while the
    // name still looks like "Title - Author".
    if (!index.readAuthor(record, author)) author.clear();
    // The stored title when the book gave one, the filename otherwise.
    if (!index.readTitle(record, title) || title.empty()) index.readName(record, title);
    if (fileName) index.readName(record, *fileName);
  }
  if (title.empty()) title = tr(STR_LIBRARY_UNKNOWN_TITLE);
  return true;
}

bool LibraryListActivity::handleCustomInput() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;

  if (lockNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lockNextConfirmRelease = false;
    return true;
  }
  if (lockNextBackRelease && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockNextBackRelease = false;
    return true;
  }

  return false;
}

bool LibraryListActivity::handleButtons() {
  const int count = listCount();

  // Every hold action fires at the threshold, mid-hold, including the ones
  // that open a dialog (remove-recent, delete). The release that follows is
  // armed as suppressed by wasLongPressed() and consumed globally by
  // ActivityManager::loop() before any activity runs, so it cannot land in
  // the freshly opened confirmation and select its default.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LONG_PRESS_MS)) {
    if (tabsFocused()) {
      if (!degraded) toggleSortDirection();
    } else if (count > 0 && rowIsList(selectedEntry())) {
      activateIndex(selectedEntry());
    } else if (recentView()) {
      showRecentBookOptions(selectedEntry());
    } else if (deleteEligible()) {
      if (count > 0) promptDeleteBook(selectedEntry());
    } else if (!groupsCollapsed && groupable()) {
      collapseGroups(selectedEntry());
    } else {
      activateIndex(selectedEntry());
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    handleBackAction();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (tabsFocused()) {
      if (pickerMode) {
        toggleSortDirection();
      } else {
        stepTab(1);
      }
      return true;
    }
    if (count > 0) activateIndex(selectedEntry());
    return true;
  }

  return false;
}

void LibraryListActivity::navigateButtons() {
  const int count = listCount();
  auto& nav = activeNav();
  buttonNavigator.onNextRelease([this, count] {
    if (count > 0) moveRingTo(ringPos() == count ? 1 : ringPos() + 1);
  });
  buttonNavigator.onPreviousRelease([this, count] {
    if (tabsFocused() && !degraded) {
      openSearch();
    } else if (count > 0) {
      moveRingTo(ringPos() <= 1 ? count : ringPos() - 1);
    }
  });
  // A held button steps tabs while the strip has focus (the base behaviour
  // Settings keeps) and page-jumps once the selection is down in the rows,
  // where fast travel through a long shelf is what a hold means.
  buttonNavigator.onNextContinuous([this, count, &nav] {
    if (tabsFocused()) {
      stepTab(1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::nextPageIndex(selectedEntry(), count, nav.pageRows()) + 1);
    }
  });
  buttonNavigator.onPreviousContinuous([this, count, &nav] {
    if (tabsFocused()) {
      stepTab(-1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::previousPageIndex(selectedEntry(), count, nav.pageRows()) + 1);
    }
  });
}

void LibraryListActivity::buildRows(UiScreen& screen) {
  auto& nav = activeNav();
  const int count = listCount();
  const bool listRows = listRowsView();
  const bool authorGrouped = authorView();
  const bool grouped = !pickerOpen && builtinView() && !isRecentSort(sortOrder);

  fui::ListProps props;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  // Breathing room between rows; the dense theme default packs the two-line
  // rows edge-to-edge.
  props.rowGap = std::max<int16_t>(screen.theme().listRowGap, 6);
  props.headerUnderline = false;
  syncTabListViewport(screen, props);

  // Keep one extra entry in the reusable window for a clipped trailing row.
  const size_t cap = static_cast<size_t>(nav.visibleRows > 0 ? nav.visibleRows : 1) + 1;
  if (winTitles.size() < cap) winTitles.resize(cap);
  if (winAuthors.size() < cap) winAuthors.resize(cap);
  if (!groupsCollapsed && winHeaders.size() < cap) winHeaders.resize(cap);
  winItems.clear();
  if (winItems.capacity() < cap) winItems.reserve(cap);

  int rows = 0;
  int headers = 0;
  uint32_t previousInitial = 0;
  std::string rowFile;
  rowFile.reserve(128);
  // Capture this after syncTabListViewport(), which may clamp nav.top.
  const int windowStart = static_cast<int>(props.topIndex);
  for (int entry = windowStart; entry < count && rows < static_cast<int>(cap); entry++) {
    std::string& title = winTitles[static_cast<size_t>(rows)];
    std::string& author = winAuthors[static_cast<size_t>(rows)];
    fui::ListItem item;
    if (groupsCollapsed) {
      const int bookEntry = groupStarts[entry];
      if (authorGrouped) {
        rowTextFor(bookEntry, title, author);
        formatAuthorHeading(author, title);
      } else {
        formatInitialHeading(titleInitialFor(bookEntry), title);
      }
    } else if (listRows || rowIsList(entry)) {
      // A list row: the list's name over its size.
      const library::ClixListIcon icon = listRowText(listIdAt(entry), title, author);
      if (!author.empty()) item.subtitle = author.c_str();
      item.icon = listIconBitmap(icon);
      rowFile.clear();
    } else {
      if (!rowTextFor(entry, title, author, &rowFile)) continue;
      uint32_t initial = 0;
      bool startsGroup = false;
      if (authorGrouped) {
        startsGroup = rows == 0 || author != winAuthors[static_cast<size_t>(rows - 1)];
      } else if (grouped) {
        initial = titleInitialFor(entry);
        startsGroup = rows == 0 || initial != previousInitial;
        previousInitial = initial;
      }
      if (startsGroup) {
        std::string& heading = winHeaders[static_cast<size_t>(headers++)];
        if (authorGrouped)
          formatAuthorHeading(author, heading);
        else
          formatInitialHeading(initial, heading);
        item.sectionHeading = heading.c_str();
      }
      if (!authorGrouped && !author.empty()) item.subtitle = author.c_str();
    }

    item.label = title.c_str();
    // Group headings stay bare; every book row gets its file-type icon.
    if (!groupsCollapsed && !listRows && !rowFile.empty()) item.icon = listIconFor(UITheme::getFileIcon(rowFile), 32);
    item.actionValue = static_cast<int16_t>(entry);
    winItems.push_back(item);
    rows++;
  }

  props.items = winItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(winItems.size());
  screen.list(props);
  const int next = nav.drawnRows;
  const auto body = screen.body();
  LOG_DBG("LIB", "page tab=%d top=%d full=%d loaded=%d body=%d..%d next=%d title=%s", activeTabIndex, windowStart,
          nav.drawnRows, rows, body.y, body.bottom(),
          next < rows ? winItems[static_cast<size_t>(next)].actionValue : -1,
          next < rows ? winItems[static_cast<size_t>(next)].label : "<none>");
  LOG_DBG("LIB", "page first=%d title=%s", rows > 0 ? winItems[0].actionValue : -1,
          rows > 0 ? winItems[0].label : "<none>");
}

void LibraryListActivity::formatInitialHeading(uint32_t initial, std::string& out) {
  out.clear();
  if (initial == 0) {
    out.push_back('#');
    return;
  }
  if (initial >= 'a' && initial <= 'z') initial -= 'a' - 'A';
  utf8AppendCodepoint(initial, out);
}

void LibraryListActivity::formatAuthorHeading(const std::string& author, std::string& out) const {
  out = author.empty() ? std::string(tr(STR_LIBRARY_UNKNOWN_AUTHOR)) : author;
  if (author.empty()) return;
  const size_t lastSpace = out.find_last_of(' ');
  if (lastSpace != std::string::npos && lastSpace + 1 < out.size()) {
    out = out.substr(lastSpace + 1) + ", " + out.substr(0, lastSpace);
  }
}

void LibraryListActivity::buildHeader(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  fui::HeaderProps header;
  header.title = headerTitle();
  header.titleText = theme.titleText;
  header.titleText.align = theme.headerTitleAlign;
  header.sidePadding = theme.headerSidePadding;
  header.minTouchSize = theme.minTouchSize;
  header.styles = theme.popup;
  if (header.styles.normal.border.kind == fui::PaintKind::None && theme.headerUnderline > 0) {
    header.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    header.styles.normal.borderWidth = theme.headerUnderline;
  }
  header.trailingStyles = fui::plainStyles(fui::Paint::solid(fui::Color::Black));
  header.borderEdges = fui::EdgeBottom;
  // Same battery/clock band as every GUI.drawHeader screen; the header
  // heights are unified across themes, so the buttons derive from the band.
  GUI.applyHeaderStatus(renderer, header);
  if (mappedInput.hasTouch()) {
    header.leadingIcon = fui::bitmapFromIcon(icon_header_back_32);
    header.leadingAction = ACTION_BACK;
  }
  if (!degraded) {
    // Keep both touch actions together on the right; button boards reach
    // rebuild through the row options menu.
    header.trailingIcon = fui::bitmapFromIcon(icon_search_32);
    header.trailingAction = ACTION_SEARCH;
    if (mappedInput.hasTouch()) {
      header.trailingAdjacentIcon = fui::bitmapFromIcon(icon_refresh_cw_32);
      header.trailingAdjacentAction = ACTION_REBUILD;
    }
    // Vertical placement comes from applyHeaderStatus: buttons center on the
    // unified band.
  }
  const auto frameRect = screen.frame().screen();
  // Header and tabs share a screen-relative boundary, independent of bezel insets.
  const fui::Rect band{frameRect.x, static_cast<int16_t>(metrics.topPadding), frameRect.width,
                       static_cast<int16_t>(metrics.headerHeight)};
  const bool titleControl = pickerMode && !degraded && query.empty();
  if (titleControl) header.title = nullptr;
  fui::header(screen.frame(), band, header);
  if (titleControl) buildTitleControl(screen, band, header);
}

// Picker mode has no tab strip: the header title names the open list, with
// its direction, and takes the strip's place as ring 0. It is drawn as a
// one-tab bar so it gets the tab's arrow, tap target and focus pill.
void LibraryListActivity::buildTitleControl(UiScreen& screen, const fui::Rect& band, const fui::HeaderProps& header) {
  const auto& theme = screen.theme();
  if (pickerOpen) {
    titleText = tr(STR_LIBRARY);
  } else if (depth > 0) {
    titleText = currentLabel;
  } else {
    char buf[160];
    snprintf(buf, sizeof(buf), tr(STR_LIBRARY_LIST_TITLE), currentLabel.c_str());
    titleText = buf;
  }

  fui::TabItem tab;
  tab.label = titleText.c_str();
  tab.selected = true;
  if (sortTitleActive()) tab.indicator = viewDescending() ? fui::TabIndicator::Down : fui::TabIndicator::Up;

  fui::TabBarProps props;
  props.tabs = &tab;
  props.count = 1;
  props.action = ACTION_TAB;
  props.inputMask = fui::InputTouch;
  props.text = theme.titleText;
  props.tabInset = fui::Insets{0, 0, 0, 0};
  props.contentInset = fui::Insets{2, 10, 2, 10};
  props.indicatorSize = 10;
  props.indicatorGap = 8;
  props.minTouchSize = theme.minTouchSize;
  fui::StyleSet styles;
  styles.explicitlySet = true;
  styles.normal.foreground = fui::Paint::solid(fui::Color::Black);
  styles.selected.foreground = fui::Paint::solid(fui::Color::Black);
  if (tabsFocused()) {
    // Button boards reach the title on the ring; show where focus is.
    styles.selected.background = fui::Paint::solid(fui::Color::Black);
    styles.selected.foreground = fui::Paint::solid(fui::Color::White);
    styles.selected.radius = theme.listRowRadius;
  }
  styles.focused = styles.selected;
  styles.active = styles.selected;
  props.tabStyles = styles;

  // The space between the header's buttons, laid out as fui::header lays out
  // its title: tucked after the back arrow, or centered on the band.
  const bool touch = mappedInput.hasTouch();
  const int16_t leading = touch ? static_cast<int16_t>(4 + (header.leadingSize + header.leadingIcon.width) / 2 + 6)
                                : static_cast<int16_t>(header.sidePadding);
  int16_t trailing = static_cast<int16_t>(12 + header.trailingSize + 8);
  if (touch) trailing = static_cast<int16_t>(trailing + 4 + header.trailingSize);
  const bool centered = theme.headerTitleAlign == fui::TextAlign::Center;
  const int16_t left = centered ? std::max(leading, trailing) : leading;
  const int16_t right = centered ? left : trailing;
  const int16_t height = static_cast<int16_t>(screen.target().lineHeight(props.text.font) + 12);
  const fui::Rect rect{static_cast<int16_t>(band.x + left),
                       static_cast<int16_t>(band.y + header.titleOffsetY + (band.height - height) / 2),
                       static_cast<int16_t>(band.width - left - right), height};
  if (!centered) props.layout = fui::TabBarLayout::ContentWidth;
  fui::tabBar(screen.frame(), rect, props);
}

void LibraryListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // The position readout owns the line above the hints; rows must not overlap
  // it.
  const int16_t readoutReserved = static_cast<int16_t>(renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing);
  buildHeader(screen);
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight + readoutReserved), 0});

  if (pickerMode) {
    screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  } else if (!degraded) {
    buildTabBar(screen);
  }
  if (bookRowCount() == 0) {
    const char* message = tr(STR_LIBRARY_NO_RESULTS);
    if (filterFailed) {
      message = tr(STR_LIBRARY_SEARCH_UNAVAILABLE);
    } else if (query.empty()) {
      message = builtinView() || index.bookCount() == 0 ? tr(STR_LIBRARY_EMPTY) : tr(STR_LIBRARY_LIST_EMPTY);
    }
    screen.centeredText(message);
    return;
  }
  buildRows(screen);
}

// "12/69 books" at the bottom right: which book is selected, out of how many.
//
// NOT a page count. How many rows fit varies with the view (author headings
// consume band height), so a page total grows and shrinks as you scroll. The
// book position is stable by construction, and it answers the question the
// reader actually has: how far in am I, and how much is left.
void LibraryListActivity::drawPositionReadout() const {
  const int count = listCount();
  if (count <= 0) return;

  char buf[32];
  const char* positionFormat =
      groupsCollapsed || listRowsView() ? tr(STR_LIBRARY_GROUP_POSITION) : tr(STR_LIBRARY_POSITION);
  snprintf(buf, sizeof(buf), positionFormat, selectedEntry() + 1, count);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getTextWidth(SMALL_FONT_ID, buf);
  const int x = renderer.getScreenWidth() - width - SIDE_PADDING;
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawText(SMALL_FONT_ID, x, y, buf, true);
}

const char* LibraryListActivity::headerTitle() const {
  if (!headerSearchTitle.empty()) return headerSearchTitle.c_str();
  if (degraded) return tr(STR_LIBRARY_TITLE_UNSORTED);
  // Inside a child list the header names it; the tab still names its root.
  return depth > 0 ? currentLabel.c_str() : tr(STR_LIBRARY);
}

void LibraryListActivity::drawHoldHelp() const {
  if (mappedInput.hasTouch() || groupsCollapsed || listRowsView()) return;
  const char* help = nullptr;
  if (tabsFocused() && !degraded)
    help = pickerMode ? nullptr : tr(STR_LIBRARY_HOLD_SORT);
  else if (!tabsFocused() && recentView() && listCount() > 0)
    help = tr(STR_LIBRARY_HOLD_OPTIONS);  // recent rows: hold opens the row menu
  else if (!tabsFocused() && deleteEligible() && listCount() > 0)
    help = tr(STR_HOLD_OPEN_TO_DELETE);
  else if (!tabsFocused() && groupable())
    help = tr(STR_LIBRARY_HOLD_GROUPS);
  if (!help) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - lineHeight;
  GUI.drawHelpText(renderer, Rect{SIDE_PADDING, y, renderer.getScreenWidth() / 2 - SIDE_PADDING, lineHeight}, help);
}

// OptionPopup is a self-contained modal: it owns the whole frame (hints
// included) whenever it is up, mirroring the FileBrowser pattern.
void LibraryListActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiTabListActivity::render(std::move(lock));
}

void LibraryListActivity::drawFooter() {
  drawPositionReadout();
  drawHoldHelp();

  const bool backGoesHome =
      !groupsCollapsed && query.empty() && depth == 0 && (pickerMode ? pickerOpen : tabsFocused());
  const char* backLabel = backGoesHome ? tr(STR_HOME) : tr(STR_BACK);
  const char* confirmLabel = groupsCollapsed || listRowsView() ? tr(STR_SELECT) : tr(STR_OPEN);
  const bool canSearch = tabsFocused() && !degraded;
  // Confirm on the tabs steps them; on the title it reverses the list. The
  // picker's title does nothing.
  const char* tabConfirm = pickerMode ? (sortTitleActive() ? tr(STR_TOGGLE) : "") : tr(STR_TOGGLE);
  const auto labels = mappedInput.mapLabels(backLabel, tabsFocused() ? tabConfirm : confirmLabel,
                                            canSearch ? tr(STR_SEARCH) : tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
