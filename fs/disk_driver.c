#include "disk_driver.h"
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

//Calculates how many blocks are needed to store the Disk Header and the BitMap
static size_t meta_blocks(int num_blocks) {
  size_t bytes = sizeof(DiskHeader) + (size_t)((num_blocks + 7) / 8);
  return (bytes + BLOCK_SIZE - 1) / BLOCK_SIZE;
}

//Creates a BitMap structure from the DiskDriver
static BitMap bitmap_of(DiskDriver* d) {
  BitMap b = { d->header->num_blocks, d->bitmap_data };
  return b;
}

//creates a pointer to the block n in the disk
static char* block_ptr(DiskDriver* d, int n) {
  return (char*)d->header + ((size_t)d->header->bitmap_blocks + (size_t)n) * BLOCK_SIZE;
}

//Checks if the block n is used
static int is_used(DiskDriver* d, int n) {
  BitMapEntryKey k = BitMap_blockToIndex(n);
  return (d->bitmap_data[k.entry_num] >> k.bit_num) & 1;
}

//Checks if the block n is in range of the disk
static int in_range(DiskDriver* d, int n) {
  return n >= 0 && n < d->header->num_blocks;
}

int DiskDriver_init(DiskDriver* disk, const char* filename, int num_blocks) {
  int fd = open(filename, O_RDWR | O_CREAT, 0660);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) < 0) { close(fd); return -1; }

  int is_new = (st.st_size == 0);
  size_t map_size;
  if (is_new) {
    if (num_blocks <= 0) { close(fd); return -1; }
    map_size = (meta_blocks(num_blocks) + (size_t)num_blocks) * BLOCK_SIZE;
    if (ftruncate(fd, (off_t)map_size) < 0) { close(fd); return -1; }
  } 
  else 
  {
    map_size = (size_t)st.st_size;
    if (map_size < sizeof(DiskHeader)) { close(fd); return -1; }
  }

  void* m = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (m == MAP_FAILED) { close(fd); return -1; }
  DiskHeader* h = (DiskHeader*)m;

  if (is_new) {
    //il file è stato appena creato: la bitmap e' libera
    h->num_blocks = num_blocks;
    h->bitmap_entries = (num_blocks + 7) / 8;
    h->bitmap_blocks = (int)meta_blocks(num_blocks);
    h->free_blocks = num_blocks;
    h->first_free_block = 0;
  } 
  else 
  {
    //si controlla che l'header sia coerente con la dimensione del file
    int ok = h->num_blocks > 0 &&
             h->bitmap_entries == (h->num_blocks + 7) / 8 &&
             h->bitmap_blocks == (int)meta_blocks(h->num_blocks) &&
             map_size == ((size_t)h->bitmap_blocks + (size_t)h->num_blocks) * BLOCK_SIZE &&
             h->free_blocks >= 0 && h->free_blocks <= h->num_blocks;
    if (!ok) { munmap(m, map_size); close(fd); return -1; }
  }

  disk->header = h;
  disk->bitmap_data = (char*)m + sizeof(DiskHeader);
  disk->fd = fd;
  disk->map_size = map_size;
  return 0;
}

int DiskDriver_readBlock(DiskDriver* disk, void* dest, int block_num) {
  if (!in_range(disk, block_num) || !is_used(disk, block_num)) return -1;
  memcpy(dest, block_ptr(disk, block_num), BLOCK_SIZE);
  return 0;
}

int DiskDriver_writeBlock(DiskDriver* disk, void* src, int block_num) {
  if (!in_range(disk, block_num)) return -1;
  memcpy(block_ptr(disk, block_num), src, BLOCK_SIZE);
  if (!is_used(disk, block_num)) {
    BitMap b = bitmap_of(disk);
    BitMap_set(&b, block_num, 1);
    disk->header->free_blocks--;
    disk->header->first_free_block = BitMap_get(&b, 0, 0);
  }
  return 0;
}

int DiskDriver_freeBlock(DiskDriver* disk, int block_num) {
  if (!in_range(disk, block_num)) return -1;
  if (is_used(disk, block_num)) {
    BitMap b = bitmap_of(disk);
    BitMap_set(&b, block_num, 0);
    disk->header->free_blocks++;
    disk->header->first_free_block = BitMap_get(&b, 0, 0);
  }
  return 0;
}

int DiskDriver_getFreeBlock(DiskDriver* disk, int start) {
  BitMap b = bitmap_of(disk);
  return BitMap_get(&b, start, 0);
}

int DiskDriver_flush(DiskDriver* disk) {
  return msync(disk->header, disk->map_size, MS_SYNC);
}

int DiskDriver_close(DiskDriver* disk) {
  int r = DiskDriver_flush(disk);
  munmap(disk->header, disk->map_size);
  close(disk->fd);
  disk->header = NULL;
  disk->bitmap_data = NULL;
  disk->fd = -1;
  return r;
}
