#include "NolanTheme.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "components/icons/library.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"
#include "fontIds.h"

namespace {
constexpr int CARD_GAP = 12;
constexpr int LARGE_CARD_PADDING = 16;
constexpr int SMALL_CARD_PADDING = 10;
constexpr int COVER_TEXT_GAP = 18;
constexpr int CARD_RADIUS = 8;
constexpr int SELECTION_OUTLINE = 3;
constexpr int ICON_BAR_HEIGHT = 80;
constexpr int ICON_SIZE = 32;
constexpr int LARGE_RING_DIAMETER = 60;
constexpr int SMALL_RING_DIAMETER = 48;
// Narrowest title column worth keeping; below it a card drops its ring.
constexpr int MIN_TEXT_WIDTH = 120;

// Left to right; HomeActivity maps the same order to Settings, File Transfer
// and Library.
constexpr const uint8_t* ICONS[CardHomeLayout::ICON_COUNT] = {Settings2Icon, TransferIcon, LibraryIcon};

constexpr int cardPadding(const bool large) { return large ? LARGE_CARD_PADDING : SMALL_CARD_PADDING; }
constexpr int ringDiameter(const bool large) { return large ? LARGE_RING_DIAMETER : SMALL_RING_DIAMETER; }

// Ring filled clockwise from 12 o'clock to `percent`; the unread part is a
// 50% dither track.
void drawProgressRing(const GfxRenderer& renderer, const int cx, const int cy, const int radius, const int thickness,
                      const int percent) {
  const int inner = radius - thickness;
  const int outerSq = radius * radius;
  const int innerSq = inner * inner;
  const float filled = static_cast<float>(std::clamp(percent, 0, 100)) / 100.0f;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const int d = dx * dx + dy * dy;
      if (d > outerSq || d < innerSq) continue;
      float angle = std::atan2(static_cast<float>(dx), static_cast<float>(-dy));
      if (angle < 0) angle += 2.0f * static_cast<float>(M_PI);
      const bool read = angle / (2.0f * static_cast<float>(M_PI)) < filled;
      const bool track = ((cx + dx + cy + dy) & 1) == 0;
      if (read || track) renderer.drawPixel(cx + dx, cy + dy, true);
    }
  }
}

void drawBookCard(GfxRenderer& renderer, const CardHomeLayout& layout, const int index, const RecentBook& book,
                  const int progress, const bool selected) {
  const Rect& card = layout.cards[index];
  const Rect& cover = layout.covers[index];
  const bool large = index == 0;

  // Cards are unframed; only the selection is outlined.
  if (selected) {
    renderer.drawRoundedRect(card.x, card.y, card.width, card.height, SELECTION_OUTLINE, CARD_RADIUS, true);
  }

  const bool showRing = layout.showProgress[index] && progress >= 0;
  const int ring = ringDiameter(large);
  const int textX = cover.x + cover.width + COVER_TEXT_GAP;
  const int textRight =
      card.x + card.width - cardPadding(large) - (layout.showProgress[index] ? ring + COVER_TEXT_GAP : 0);
  const int textWidth = textRight - textX;

  const int titleFont = large ? UI_12_FONT_ID : UI_10_FONT_ID;
  const int authorFont = large ? UI_10_FONT_ID : SMALL_FONT_ID;
  const auto titleLines = renderer.wrappedText(titleFont, book.title.c_str(), textWidth, large ? 3 : 2,
                                               EpdFontFamily::BOLD);
  const std::string author =
      book.author.empty() ? std::string() : renderer.truncatedText(authorFont, book.author.c_str(), textWidth);

  const int titleLh = renderer.getLineHeight(titleFont);
  const int authorLh = author.empty() ? 0 : renderer.getLineHeight(authorFont);
  const int blockHeight = static_cast<int>(titleLines.size()) * titleLh + (authorLh ? authorLh + 4 : 0);
  int y = card.y + (card.height - blockHeight) / 2;
  for (const auto& line : titleLines) {
    renderer.drawText(titleFont, textX, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += titleLh;
  }
  if (!author.empty()) renderer.drawText(authorFont, textX, y + 4, author.c_str(), true);

  if (showRing) {
    const int radius = ring / 2;
    const int cx = card.x + card.width - cardPadding(large) - radius;
    const int cy = card.y + card.height / 2;
    drawProgressRing(renderer, cx, cy, radius, large ? 6 : 5, progress);
    char label[8];
    snprintf(label, sizeof(label), "%d%%", progress);
    const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, label);
    renderer.drawText(SMALL_FONT_ID, cx - labelWidth / 2, cy - renderer.getLineHeight(SMALL_FONT_ID) / 2, label, true);
  }
}
}  // namespace

CardHomeLayout NolanTheme::cardHomeLayout(const GfxRenderer& renderer) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int side = metrics.contentSidePadding;
  const int top = metrics.homeTopPadding;

  CardHomeLayout layout;
  layout.iconBar = Rect{0, height - metrics.buttonHintsHeight - ICON_BAR_HEIGHT, width, ICON_BAR_HEIGHT};
  const int slotWidth = width / CardHomeLayout::ICON_COUNT;
  for (int i = 0; i < CardHomeLayout::ICON_COUNT; ++i) {
    layout.icons[i] = Rect{i * slotWidth + 8, layout.iconBar.y + 8, slotWidth - 16, ICON_BAR_HEIGHT - 16};
  }

  // Sized so portrait fits the large card plus three small ones; shorter
  // (landscape) screens keep a readable minimum and show fewer small cards.
  const int available = layout.iconBar.y - CARD_GAP - top;
  // The large card's extra padding grows the card rather than shrinking its cover.
  const int largeHeight =
      std::clamp(available / 3, 150, 240) + 2 * (LARGE_CARD_PADDING - SMALL_CARD_PADDING);
  const int smallHeight = std::clamp(largeHeight * 55 / 100, 96, 136);
  const int smallCount =
      std::clamp((available - largeHeight) / (smallHeight + CARD_GAP), 0, CardHomeLayout::MAX_CARDS - 1);

  layout.cardCount = 1 + smallCount;
  int y = top;
  for (int i = 0; i < layout.cardCount; ++i) {
    const bool large = i == 0;
    const int cardHeight = large ? largeHeight : smallHeight;
    layout.cards[i] = Rect{side, y, width - 2 * side, cardHeight};
    const int pad = cardPadding(large);
    const int coverHeight = cardHeight - 2 * pad;
    layout.covers[i] = Rect{side + pad, y + pad, coverHeight * 2 / 3, coverHeight};
    const int textWidth = layout.cards[i].width - layout.covers[i].width - 2 * pad - COVER_TEXT_GAP;
    layout.showProgress[i] = textWidth - ringDiameter(large) - COVER_TEXT_GAP >= MIN_TEXT_WIDTH;
    y += cardHeight + CARD_GAP;
  }
  return layout;
}

void NolanTheme::drawCardHome(GfxRenderer& renderer, const CardHomeLayout& layout,
                              const std::vector<RecentBook>& books, const int* progress,
                              const int selectorIndex) const {
  const int bookCount = std::min(static_cast<int>(books.size()), layout.cardCount);
  if (bookCount == 0) {
    drawEmptyRecents(renderer, layout.cards[0]);
  }
  for (int i = 0; i < bookCount; ++i) {
    drawBookCard(renderer, layout, i, books[i], progress[i], selectorIndex == i);
  }

  for (int i = 0; i < CardHomeLayout::ICON_COUNT; ++i) {
    const Rect& slot = layout.icons[i];
    if (selectorIndex == bookCount + i) {
      renderer.fillRoundedRect(slot.x, slot.y, slot.width, slot.height, CARD_RADIUS, Color::LightGray);
    }
    renderer.drawIcon(ICONS[i], slot.x + (slot.width - ICON_SIZE) / 2, slot.y + (slot.height - ICON_SIZE) / 2,
                      ICON_SIZE);
  }
}

void NolanTheme::drawCardHomeCover(const GfxRenderer& renderer, const Rect rect, const std::string& thumbPath) const {
  // Paint the whole rect: HomeActivity snapshots it, so a narrow cover must
  // not capture whatever was drawn there before.
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  bool drawn = false;
  if (!thumbPath.empty()) {
    HalFile file;
    if (Storage.openFileForRead("HOME", thumbPath, file)) {
      Bitmap bitmap(file);
      drawn = bitmap.parseHeaders() == BmpReaderError::Ok && drawCoverThumbFill(renderer, bitmap, rect);
    }
  }
  if (!drawn) {
    renderer.fillRect(rect.x, rect.y + rect.height / 3, rect.width, rect.height * 2 / 3, true);
    renderer.drawIcon(CoverIcon, rect.x + (rect.width - ICON_SIZE) / 2, rect.y + SMALL_CARD_PADDING, ICON_SIZE);
  }
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
}
