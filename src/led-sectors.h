#pragma once

// Visit each visible sector, clipping it to the ring. Keeping the expansion
// independent of Arduino/FastLED lets the same code run in desktop tests.
template <typename Sectors, typename GetCount, typename PaintSector>
void paintLedSectors(const Sectors& sectors, int ledCount, bool repeat,
                     GetCount getCount, PaintSector paintSector) {
  if (ledCount <= 0) return;
  int pos = 0;
  do {
    const int start = pos;
    for (auto sector : sectors) {
      const int requested = getCount(sector);
      if (requested <= 0) continue;
      const int remaining = ledCount - pos;
      const int count = requested < remaining ? requested : remaining;
      paintSector(pos, count, sector);
      pos += count;
      if (pos == ledCount) break;
    }
    // Empty patterns and patterns with no positive counts cannot advance.
    if (pos == start) break;
  } while (repeat && pos < ledCount);
}
