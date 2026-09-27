#pragma once

// Search over the CLX1 index, shared by the Library screen and the benchmark.

#include <cstdint>
#include <string_view>

#include "LibraryIndexFile.h"

namespace library {

// Display rows of `order` that match `query`: every query word must prefix a
// word of the book's title sort, shown title, or author (see matchesQuery).
// `out` must hold index.bookCount() rows; returns how many were written, in
// display order. An empty query matches every row.
//
// One pass, no index and no cache: each row costs a record read, plus one blob
// read when the stored fold does not already match.
uint16_t filterRows(LibraryIndexFile& index, SortOrder order, std::string_view query, uint16_t* out);

}  // namespace library
