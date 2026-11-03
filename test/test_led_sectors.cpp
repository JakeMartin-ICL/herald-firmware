#include "led-sectors.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <iostream>
#include <vector>

struct Sector {
  int color;
  int count;
};

static std::vector<int> render(const std::vector<Sector>& sectors, int ledCount = 24,
                               bool repeat = false) {
  std::vector<int> leds(ledCount, 0);
  int writes = 0;
  paintLedSectors(sectors, ledCount, repeat,
    [](Sector sector) { return sector.count; },
    [&](int start, int count, Sector sector) {
      assert(start >= 0 && count > 0 && start + count <= ledCount);
      for (int i = start; i < start + count; i++) leds.at(i) = sector.color;
      writes += count;
    });
  assert(writes <= ledCount);
  return leds;
}

int main() {
  // Existing commands draw once and leave the unused tail off.
  const std::vector<Sector> pattern = {{1, 1}, {2, 3}};
  const auto once = render(pattern);
  assert((std::vector<int>(once.begin(), once.begin() + 4) == std::vector<int>{1, 2, 2, 2}));
  assert(std::all_of(once.begin() + 4, once.end(), [](int color) { return color == 0; }));

  // Repeat around rings of different sizes, including a partial final cycle.
  for (int size : {0, 1, 2, 4, 23, 24, 25}) {
    const auto leds = render(pattern, size, true);
    for (int i = 0; i < size; i++) assert(leds.at(i) == (i % 4 == 0 ? 1 : 2));
  }

  // Arbitrary ratios and more than two colours use the same expansion.
  const auto multi = render({{1, 2}, {2, 1}, {3, 3}}, 24, true);
  const std::vector<int> cycle = {1, 1, 2, 3, 3, 3};
  for (int i = 0; i < 24; i++) assert(multi.at(i) == cycle.at(i % 6));

  // Full-ring sector commands retain their old behaviour.
  const std::vector<Sector> thirds = {{1, 8}, {2, 8}, {3, 8}};
  assert(render(thirds) == render(thirds, 24, true));

  // Zero/negative lengths are skipped. Empty/non-progressing patterns terminate.
  assert(render({}, 24, true) == std::vector<int>(24, 0));
  assert(render({{1, 0}, {2, -1}, {3, INT_MIN}}, 24, true) == std::vector<int>(24, 0));
  assert(render({{1, 0}, {2, 1}, {3, -1}}, 24, true) == std::vector<int>(24, 2));

  // Large counts clip without overflowing or writing beyond the LED buffer.
  assert(render({{1, INT_MAX}, {2, 1}}) == std::vector<int>(24, 1));
  assert(render({{1, 1}, {2, INT_MAX}}, 24, true) ==
         render({{1, 1}, {2, 23}}));
  std::cout << "LED sector tests passed\n";
}
