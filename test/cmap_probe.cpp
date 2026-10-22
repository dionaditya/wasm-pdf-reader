#include "cmap.hpp"
#include <cstdio>
#include <cstring>
int main() {
  struct { const char* name; unsigned code; int cid; } cases[] = {
    {"UniJIS-UCS2-H", 0x65E5, -1},
    {"UniGB-UCS2-H", 0x4E2D, -1},
    {"UniCNS-UCS2-H", 0x4E2D, -1},
    {"UniKS-UCS2-H", 0x4E2D, -1},
    {"90ms-RKSJ-H", 0x82A0, -1},
    {"GBK-EUC-H", 0xB0A1, -1},
  };
  for (auto& c : cases) {
    const pdf::CMap* m = pdf::cmapFind(c.name);
    if (!m) { printf("%s MISSING\n", c.name); continue; }
    int n = c.code > 0xFFFF ? 4 : 2;
    if (c.code <= 0xFF) n = 1;
    if (!strcmp(c.name, "90ms-RKSJ-H") || !strcmp(c.name, "GBK-EUC-H")) n = 2;
    printf("%s U+%04X -> CID %d (%zu entries)\n", c.name, c.code, m->lookup(c.code, n), m->entries.size());
  }
  return 0;
}
