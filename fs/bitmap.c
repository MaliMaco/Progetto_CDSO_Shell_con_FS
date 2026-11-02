#include "bitmap.h"

BitMapEntryKey BitMap_blockToIndex(int num) {
  BitMapEntryKey k = { num / 8, (char)(num % 8) };
  return k;
}

int BitMap_indexToBlock(int entry, uint8_t bit_num) {
  return entry * 8 + bit_num;
}

// scorre un byte alla volta quando il byte e' tutto "diverso" da status
int BitMap_get(BitMap* bmap, int start, int status) {
  status = !!status;  //in caso di interi, li trasforma in 1
  if (start < 0) start = 0;
  for (int i = start; i < bmap->num_bits; i++) {
    unsigned char byte = (unsigned char)bmap->entries[i >> 3];
    if ((i & 7) == 0 && (status ? byte == 0x00 : byte == 0xFF)) {
      i += 7;
      continue;
    }
    if (((byte >> (i & 7)) & 1) == status) return i;
  }
  return -1;
}

// 0 se ok, -1 se pos fuori range
int BitMap_set(BitMap* bmap, int pos, int status) {
  if (pos < 0 || pos >= bmap->num_bits) return -1;
  BitMapEntryKey k = BitMap_blockToIndex(pos);
  if (status) bmap->entries[k.entry_num] |= (char)(1 << k.bit_num);
  else        bmap->entries[k.entry_num] &= (char)~(1 << k.bit_num);
  return 0;
}
