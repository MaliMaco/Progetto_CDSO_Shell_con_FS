#include "simplefs.h"
#include <stdlib.h>
#include <string.h>

// un blocco letto dal disco, visto con il tipo che serve
typedef union {
  FirstFileBlock ff;
  FirstDirectoryBlock fd;
  FileBlock fb;
  DirectoryBlock db;
  char raw[BLOCK_SIZE];
} Block;

//Lo scopo di questi assert è di assicurarci che tutto sia stato correttamente inizializzato
_Static_assert(sizeof(FirstFileBlock) == BLOCK_SIZE, "FirstFileBlock != BLOCK_SIZE");
_Static_assert(sizeof(FileBlock) == BLOCK_SIZE, "FileBlock != BLOCK_SIZE");
_Static_assert(sizeof(FirstDirectoryBlock) == BLOCK_SIZE, "FirstDirectoryBlock != BLOCK_SIZE");
_Static_assert(sizeof(DirectoryBlock) == BLOCK_SIZE, "DirectoryBlock != BLOCK_SIZE");

#define NAME_SIZE      ((int)sizeof(((FileControlBlock*)0)->name))
#define FIRST_DATA     ((int)sizeof(((FirstFileBlock*)0)->data))
#define NEXT_DATA      ((int)sizeof(((FileBlock*)0)->data))
#define DIR_FIRST_CAP  ((int)(sizeof(((FirstDirectoryBlock*)0)->file_blocks) / sizeof(int)))
#define DIR_NEXT_CAP   ((int)(sizeof(((DirectoryBlock*)0)->file_blocks) / sizeof(int)))

//Metodi per la gestione dei blocchi di una directory
//Una directory è un array di numeri di blocco distribuito su una catena (FirstDirectoryBlock + DirectoryBlock):
//invece di accedere alle singole entry la si legge e la si riscrive per intero con dir_load e dir_store.

static void free_chain(DiskDriver* disk, int first) {
  Block b;
  for (int cur = first; cur != -1;) {
    if (DiskDriver_readBlock(disk, &b, cur) < 0) break;
    int next = b.ff.header.next_block;
    DiskDriver_freeBlock(disk, cur);
    cur = next;
  }
}

// legge i numeri di blocco di tutte le entry; l'array (da liberare) ha un posto libero in più per un'aggiunta
static int* dir_load(DiskDriver* disk, FirstDirectoryBlock* dcb) {
  int n = dcb->num_entries;
  int* e = malloc((size_t)(n + 1) * sizeof *e);
  if (!e) return NULL;
  int done = n < DIR_FIRST_CAP ? n : DIR_FIRST_CAP;
  memcpy(e, dcb->file_blocks, (size_t)done * sizeof *e);
  Block b;
  for (int cur = dcb->header.next_block; done < n; cur = b.db.header.next_block) {
    if (cur == -1 || DiskDriver_readBlock(disk, &b, cur) < 0) {
      free(e);
      return NULL;
    }
    int cnt = n - done < DIR_NEXT_CAP ? n - done : DIR_NEXT_CAP;
    memcpy(e + done, b.db.file_blocks, (size_t)cnt * sizeof *e);
    done += cnt;
  }
  return e;
}

// riscrive le n entry di e[] allungando o accorciando la catena; aggiorna anche dcb e lo salva.
// Se il disco è pieno ritorna -1 e il disco resta coerente (la copia in memoria di dcb va riletta)
static int dir_store(DiskDriver* disk, FirstDirectoryBlock* dcb, const int* e, int n) {
  dcb->num_entries = n;
  dcb->fcb.size_in_blocks = n > DIR_FIRST_CAP ? 1 + (n - DIR_FIRST_CAP + DIR_NEXT_CAP - 1) / DIR_NEXT_CAP : 1;

  BlockHeader* h = &dcb->header;                  // blocco in scrittura: prima dcb, poi b
  int* slots = dcb->file_blocks;
  int cap = DIR_FIRST_CAP, phys = dcb->fcb.block_in_disk, done = 0;
  Block b;
  for (int k = 0;; k++) {
    int cnt = n - done < cap ? n - done : cap;
    memset(slots, 0, (size_t)cap * sizeof *slots);
    memcpy(slots, e + done, (size_t)cnt * sizeof *slots);
    done += cnt;
    void* cur = k ? (void*)&b : (void*)dcb;
    int old_next = h->next_block;

    if (done == n) {                              // ultimo blocco: l'eventuale coda in avanzo va liberata
      h->next_block = -1;
      free_chain(disk, old_next);
      return DiskDriver_writeBlock(disk, cur, phys);
    }
    int next = old_next != -1 ? old_next : DiskDriver_getFreeBlock(disk, 0);
    if (next < 0) return -1;
    h->next_block = next;
    if (DiskDriver_writeBlock(disk, cur, phys) < 0) return -1;

    if (old_next != -1) {                         // blocco già in catena
      if (DiskDriver_readBlock(disk, &b, next) < 0) return -1;
    } else {                                      // blocco nuovo: lo si riserva subito sul disco
      memset(&b, 0, sizeof b);
      b.db.header.previous_block = phys;
      b.db.header.next_block = -1;
      b.db.header.block_in_file = k + 1;
      if (DiskDriver_writeBlock(disk, &b, next) < 0) return -1;
    }
    h = &b.db.header;
    slots = b.db.file_blocks;
    cap = DIR_NEXT_CAP;
    phys = next;
  }
}

//Metodi che rappresentano semplici QoL 

static int valid_name(const char* n) {
  size_t l = n ? strlen(n) : 0;
  return l > 0 && l < (size_t)NAME_SIZE && !strchr(n, '/') && strcmp(n, ".") && strcmp(n, "..");
}

// cerca 'name' in dcb; ritorna il blocco dell'entry (e opzionalmente indice e contenuto) o -1
static int dir_lookup(DiskDriver* disk, FirstDirectoryBlock* dcb, const char* name, int* idx, Block* out) {
  int* e = dir_load(disk, dcb);
  if (!e) return -1;
  Block tmp;
  if (!out) out = &tmp;
  int found = -1;
  for (int i = 0; i < dcb->num_entries && found < 0; i++) {
    if (DiskDriver_readBlock(disk, out, e[i]) < 0) continue;
    out->ff.fcb.name[NAME_SIZE - 1] = '\0';
    if (!strcmp(out->ff.fcb.name, name)) {
      found = e[i];
      if (idx) *idx = i;
    }
  }
  free(e);
  return found;
}

static FirstDirectoryBlock* load_dir(DiskDriver* disk, int blk) {
  Block b;
  if (blk < 0 || DiskDriver_readBlock(disk, &b, blk) < 0 || !b.ff.fcb.is_dir) return NULL;
  FirstDirectoryBlock* d = malloc(sizeof *d);
  if (d) memcpy(d, &b.fd, sizeof *d);
  return d;
}

static void remove_tree(DiskDriver* disk, int blk) {
  Block b;
  if (DiskDriver_readBlock(disk, &b, blk) < 0) return;
  if (b.ff.fcb.is_dir) {
    int* e = dir_load(disk, &b.fd);
    if (e) {
      for (int i = 0; i < b.fd.num_entries; i++) remove_tree(disk, e[i]);
      free(e);
    }
  }
  free_chain(disk, blk);
}

// true se la directory corrente è target o è contenuta in target
static int cwd_inside(SimpleFS* fs, int target) {
  if (!fs->cwd) return 0;
  Block b;
  for (int c = fs->cwd->dcb->fcb.block_in_disk; c != -1;) {
    if (c == target) return 1;
    if (DiskDriver_readBlock(fs->disk, &b, c) < 0) break;
    c = b.ff.fcb.directory_block;
  }
  return 0;
}

static void dir_handle_reset(DirectoryHandle* d) {
  d->current_block = &d->dcb->header;
  d->pos_in_dir = 0;
  d->pos_in_block = 0;
}

// rilegge dal disco il primo blocco della directory: la copia nell'handle invecchia
// se la stessa directory viene modificata attraverso un altro handle
static int dir_sync(DirectoryHandle* d) {
  Block b;
  if (DiskDriver_readBlock(d->sfs->disk, &b, d->dcb->fcb.block_in_disk) < 0 || !b.ff.fcb.is_dir) return -1;
  memcpy(d->dcb, &b.fd, sizeof *d->dcb);
  return 0;
}



void SimpleFS_format(SimpleFS* fs) {
  DiskDriver* disk = fs->disk;
  memset(disk->bitmap_data, 0, (size_t)disk->header->bitmap_entries);
  disk->header->free_blocks = disk->header->num_blocks;
  disk->header->first_free_block = 0;

  Block b;
  memset(&b, 0, sizeof b);
  int root = DiskDriver_getFreeBlock(disk, 0);      // sara' il blocco 0
  b.fd.header.previous_block = -1;
  b.fd.header.next_block = -1;
  b.fd.fcb.directory_block = -1;
  b.fd.fcb.block_in_disk = root;
  strcpy(b.fd.fcb.name, "/");
  b.fd.fcb.size_in_blocks = 1;
  b.fd.fcb.is_dir = 1;
  DiskDriver_writeBlock(disk, &b, root);
  fs->cwd = NULL;
}

DirectoryHandle* SimpleFS_init(SimpleFS* fs, DiskDriver* disk) {
  fs->disk = disk;
  fs->cwd = NULL;
  FirstDirectoryBlock* root = load_dir(disk, 0);
  if (!root || root->fcb.directory_block != -1 || strcmp(root->fcb.name, "/")) {
    free(root);
    return NULL;                                   // disco non formattato
  }
  DirectoryHandle* h = calloc(1, sizeof *h);
  h->sfs = fs;
  h->dcb = root;
  h->directory = NULL;                              // NULL = siamo alla radice
  dir_handle_reset(h);
  fs->cwd = h;
  return h;
}


static int new_node(DirectoryHandle* d, const char* name, int is_dir, Block* out) {
  DiskDriver* disk = d->sfs->disk;
  FirstDirectoryBlock* dcb = d->dcb;
  if (!valid_name(name) || dir_sync(d) < 0 || dir_lookup(disk, dcb, name, NULL, NULL) >= 0) return -1;
  int b = DiskDriver_getFreeBlock(disk, 0);
  if (b < 0) return -1;

  memset(out, 0, sizeof *out);
  out->ff.header.previous_block = -1;
  out->ff.header.next_block = -1;
  out->ff.fcb.directory_block = dcb->fcb.block_in_disk;
  out->ff.fcb.block_in_disk = b;
  strncpy(out->ff.fcb.name, name, NAME_SIZE - 1);
  out->ff.fcb.size_in_blocks = 1;
  out->ff.fcb.is_dir = is_dir;
  if (DiskDriver_writeBlock(disk, out, b) < 0) return -1;
  int* e = dir_load(disk, dcb);                    // aggiunge b in coda alle entry
  if (!e) {
    DiskDriver_freeBlock(disk, b);
    return -1;
  }
  e[dcb->num_entries] = b;
  int r = dir_store(disk, dcb, e, dcb->num_entries + 1);
  free(e);
  if (r < 0) {
    DiskDriver_freeBlock(disk, b);
    return -1;
  }
  return b;
}

static FileHandle* make_file_handle(DirectoryHandle* d, FirstFileBlock* first) {
  FileHandle* f = calloc(1, sizeof *f);
  f->sfs = d->sfs;
  f->fcb = malloc(sizeof *f->fcb);
  memcpy(f->fcb, first, sizeof *f->fcb);
  f->directory = malloc(sizeof *f->directory);
  memcpy(f->directory, d->dcb, sizeof *f->directory);   // copia privata del handle
  f->current_block = &f->fcb->header;
  f->pos_in_file = 0;
  return f;
}

FileHandle* SimpleFS_createFile(DirectoryHandle* d, const char* filename) {
  Block b;
  if (new_node(d, filename, 0, &b) < 0) return NULL;
  return make_file_handle(d, &b.ff);
}

int SimpleFS_mkDir(DirectoryHandle* d, char* dirname) {
  Block b;
  return new_node(d, dirname, 1, &b) < 0 ? -1 : 0;
}

FileHandle* SimpleFS_openFile(DirectoryHandle* d, const char* filename) {
  Block b;
  if (dir_sync(d) < 0 || dir_lookup(d->sfs->disk, d->dcb, filename, NULL, &b) < 0 || b.ff.fcb.is_dir) return NULL;
  return make_file_handle(d, &b.ff);
}

int SimpleFS_close(FileHandle* f) {
  if (!f) return -1;
  free(f->fcb);
  free(f->directory);
  free(f);
  return 0;
}


// dato il byte pos: blocco logico k (0 = primo), offset nei dati e capacita' dati del blocco
static void locate(int pos, int* k, int* off, int* cap) {
  if (pos < FIRST_DATA) { *k = 0; *off = pos; *cap = FIRST_DATA; }
  else {
    *k = 1 + (pos - FIRST_DATA) / NEXT_DATA;
    *off = (pos - FIRST_DATA) % NEXT_DATA;
    *cap = NEXT_DATA;
  }
}

// carica in *out il k-esimo blocco (k>=1) e ne ritorna il numero fisico;
// con alloc crea (e concatena) il blocco se manca
static int file_block(FileHandle* f, int k, Block* out, int alloc) {
  DiskDriver* disk = f->sfs->disk;
  Block pb;
  int prev = f->fcb->fcb.block_in_disk, cur = f->fcb->header.next_block, prev_is_first = 1;
  for (int i = 1; i <= k; i++) {
    if (cur == -1) {
      if (!alloc) return -1;
      int nb = DiskDriver_getFreeBlock(disk, 0);
      if (nb < 0) return -1;
      memset(out, 0, sizeof *out);
      out->fb.header.previous_block = prev;
      out->fb.header.next_block = -1;
      out->fb.header.block_in_file = i;
      if (DiskDriver_writeBlock(disk, out, nb) < 0) return -1;
      if (prev_is_first) f->fcb->header.next_block = nb;
      else {
        pb.fb.header.next_block = nb;
        DiskDriver_writeBlock(disk, &pb, prev);
      }
      f->fcb->fcb.size_in_blocks++;
      cur = nb;
    } else if (DiskDriver_readBlock(disk, out, cur) < 0) {
      return -1;
    }
    if (i < k) {
      pb = *out;
      prev = cur;
      prev_is_first = 0;
      cur = out->fb.header.next_block;
    }
  }
  return cur;
}

int SimpleFS_write(FileHandle* f, void* data, int size) {
  DiskDriver* disk = f->sfs->disk;
  int done = 0;
  while (done < size) {
    int k, off, cap;
    locate(f->pos_in_file, &k, &off, &cap);
    int n = cap - off < size - done ? cap - off : size - done;
    if (k == 0) {
      memcpy(f->fcb->data + off, (char*)data + done, (size_t)n);
    } else {
      Block b;
      int bn = file_block(f, k, &b, 1);
      if (bn < 0) break;                               // disco pieno
      memcpy(b.fb.data + off, (char*)data + done, (size_t)n);
      DiskDriver_writeBlock(disk, &b, bn);
    }
    done += n;
    f->pos_in_file += n;
  }
  if (f->pos_in_file > f->fcb->fcb.size_in_bytes) f->fcb->fcb.size_in_bytes = f->pos_in_file;
  DiskDriver_writeBlock(disk, f->fcb, f->fcb->fcb.block_in_disk);
  return done;
}

int SimpleFS_read(FileHandle* f, void* data, int size) {
  int avail = f->fcb->fcb.size_in_bytes - f->pos_in_file;
  if (size > avail) size = avail;
  int done = 0;
  while (done < size) {
    int k, off, cap;
    locate(f->pos_in_file, &k, &off, &cap);
    int n = cap - off < size - done ? cap - off : size - done;
    if (k == 0) {
      memcpy((char*)data + done, f->fcb->data + off, (size_t)n);
    } else {
      Block b;
      if (file_block(f, k, &b, 0) < 0) break;
      memcpy((char*)data + done, b.fb.data + off, (size_t)n);
    }
    done += n;
    f->pos_in_file += n;
  }
  return done;
}

int SimpleFS_seek(FileHandle* f, int pos) {
  if (pos < 0 || pos > f->fcb->fcb.size_in_bytes) return -1;
  f->pos_in_file = pos;
  return pos;
}


int SimpleFS_readDir(char** names, DirectoryHandle* d) {
  DiskDriver* disk = d->sfs->disk;
  if (dir_sync(d) < 0) return -1;
  int* e = dir_load(disk, d->dcb);
  if (!e) return -1;
  Block b;
  int n = d->dcb->num_entries, r = n;
  for (int i = 0; i < n; i++) {
    if (DiskDriver_readBlock(disk, &b, e[i]) < 0) {
      r = -1;
      break;
    }
    strncpy(names[i], b.ff.fcb.name, NAME_SIZE - 1);
    names[i][NAME_SIZE - 1] = '\0';
  }
  free(e);
  return r;
}

int SimpleFS_changeDir(DirectoryHandle* d, char* dirname) {
  DiskDriver* disk = d->sfs->disk;
  if (!strcmp(dirname, ".")) return 0;

  FirstDirectoryBlock *ndcb, *nparent;
  if (!strcmp(dirname, "..")) {
    if (!d->directory) return 0;                        // gia' alla radice
    ndcb = load_dir(disk, d->dcb->fcb.directory_block);
    if (!ndcb) return -1;
    nparent = load_dir(disk, ndcb->fcb.directory_block);   // NULL se ndcb e' la radice
  } else {
    Block b;
    int blk = dir_sync(d) < 0 ? -1 : dir_lookup(disk, d->dcb, dirname, NULL, &b);
    if (blk < 0 || !b.ff.fcb.is_dir) return -1;
    ndcb = load_dir(disk, blk);
    nparent = load_dir(disk, d->dcb->fcb.block_in_disk);   // riletto da disco: sempre aggiornato
    if (!ndcb || !nparent) { free(ndcb); free(nparent); return -1; }
  }
  free(d->dcb);
  free(d->directory);
  d->dcb = ndcb;
  d->directory = nparent;
  dir_handle_reset(d);
  return 0;
}

int SimpleFS_remove(SimpleFS* fs, char* filename) {
  return fs->cwd ? SimpleFS_removeIn(fs->cwd, filename) : -1;
}


int SimpleFS_removeIn(DirectoryHandle* d, const char* name) {
  DiskDriver* disk = d->sfs->disk;
  if (!valid_name(name) || dir_sync(d) < 0) return -1;
  int idx;
  Block b;
  int blk = dir_lookup(disk, d->dcb, name, &idx, &b);
  if (blk < 0) return -1;
  if (b.ff.fcb.is_dir && cwd_inside(d->sfs, blk)) return -2;
  int* e = dir_load(disk, d->dcb);                    // l'ultima entry prende il posto di quella rimossa
  if (!e) return -1;
  remove_tree(disk, blk);
  e[idx] = e[d->dcb->num_entries - 1];
  int r = dir_store(disk, d->dcb, e, d->dcb->num_entries - 1);
  free(e);
  return r < 0 ? -1 : 0;
}

DirectoryHandle* SimpleFS_cloneDir(DirectoryHandle* d) {
  FirstDirectoryBlock* dcb = load_dir(d->sfs->disk, d->dcb->fcb.block_in_disk);   // riletto da disco
  if (!dcb) return NULL;
  DirectoryHandle* c = calloc(1, sizeof *c);
  c->sfs = d->sfs;
  c->dcb = dcb;
  if (d->directory) {
    c->directory = malloc(sizeof *c->directory);
    memcpy(c->directory, d->directory, sizeof *c->directory);
  }
  dir_handle_reset(c);
  return c;
}

void SimpleFS_closeDir(DirectoryHandle* d) {
  if (!d) return;
  free(d->dcb);
  free(d->directory);
  free(d);
}
