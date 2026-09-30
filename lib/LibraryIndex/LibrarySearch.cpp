#include "LibrarySearch.h"

#include <string>

#include "LibraryText.h"

namespace library {

uint16_t filterRows(LibraryIndexFile& index, const SortOrder order, const std::string_view query, uint16_t* out) {
  const std::string needle = fold(query);
  const uint16_t total = index.bookCount();
  uint16_t count = 0;
  std::string author;
  std::string title;
  author.reserve(128);
  title.reserve(2 * 256);
  for (uint16_t row = 0; row < total; row++) {
    const uint16_t ordinal = index.ordinalForRow(order, row);
    ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    if (matchesQuery(std::string_view(record.fold, record.foldLen), needle)) {
      out[count++] = row;
      continue;
    }
    // The stored fold is the title SORT ("Hobbit, The"), so the shown title and
    // the author are read and folded here. The author is the search most worth
    // having: the reader who knows the author usually also knows where the book
    // is, while "emily" finding Alice Hunter is the case the shelf exists to answer.
    if (!index.readAuthorAndTitle(record, author, title)) continue;
    if ((!title.empty() && matchesQuery(fold(title), needle)) ||
        (!author.empty() && matchesQuery(fold(author), needle))) {
      out[count++] = row;
    }
  }
  return count;
}

}  // namespace library
