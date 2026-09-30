#pragma once

#include "components/themes/lyra/Lyra3CoversTheme.h"

class GfxRenderer;

// Forked from Lyra Extended: identical styling off the home screen.
namespace NolanMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = Lyra3CoversMetrics::values;
  v.homeRecentBooksCount = CardHomeLayout::MAX_CARDS;
  return v;
}();
}  // namespace NolanMetrics

// Home: the most recent book as a large card (cover, bold title, author,
// progress ring), the next books as smaller cards, and an icon bar with
// Settings, File Transfer and Library.
class NolanTheme : public LyraTheme {
 public:
  bool hasCardHome() const override { return true; }
  CardHomeLayout cardHomeLayout(const GfxRenderer& renderer) const override;
  void drawCardHome(GfxRenderer& renderer, const CardHomeLayout& layout, const std::vector<RecentBook>& books,
                    const int* progress, int selectorIndex) const override;
  void drawCardHomeCover(const GfxRenderer& renderer, Rect rect, const std::string& thumbPath) const override;
};
