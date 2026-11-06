#include "simplefs.h"
#include <stdlib.h>
#include <string.h>

//un blocco letto dal disco, visto con il tipo che serve
typedef union {
  FirstFileBlock ff;
  FirstDirectoryBlock fd;
  FileBlock fb;
  DirectoryBlock db;
  char raw[BLOCK_SIZE];
} Block;

//Lo scopo di questi assert è di assicurarci che tutto sia stato correttamente inizializzato senza
//dover usare assert nei singoli metodi
_Static_assert(sizeof(FirstFileBlock) == BLOCK_SIZE, "FirstFileBlock != BLOCK_SIZE");
_Static_assert(sizeof(FileBlock) == BLOCK_SIZE, "FileBlock != BLOCK_SIZE");
_Static_assert(sizeof(FirstDirectoryBlock) == BLOCK_SIZE, "FirstDirectoryBlock != BLOCK_SIZE");
_Static_assert(sizeof(DirectoryBlock) == BLOCK_SIZE, "DirectoryBlock != BLOCK_SIZE");


//blocchi che servono per n entry
#define DIR_BLOCKS(n) ((n) > DIR_FIRST_CAP ? 1 + ((n) - DIR_FIRST_CAP + DIR_NEXT_CAP - 1) / DIR_NEXT_CAP : 1)
#define NAME_SIZE      ((int)sizeof(((FileControlBlock*)0)->name))
#define FIRST_DATA     ((int)sizeof(((FirstFileBlock*)0)->data))
#define NEXT_DATA      ((int)sizeof(((FileBlock*)0)->data))
#define DIR_FIRST_CAP  ((int)(sizeof(((FirstDirectoryBlock*)0)->file_blocks) / sizeof(int)))
#define DIR_NEXT_CAP   ((int)(sizeof(((DirectoryBlock*)0)->file_blocks) / sizeof(int)))

//Metodi per la gestione dei blocchi di una directory
//Una directory è una catena di blocchi (FirstDirectoryBlock + DirectoryBlock)

//libera tutti i blocchi della catena di directory aggiornando la bitmap di conseguenza
static void free_chain(DiskDriver* disk, int first) {
  Block b;
  for (int cur = first; cur != -1;) {
    if (DiskDriver_readBlock(disk, &b, cur) < 0) break;
    int next = b.ff.header.next_block;
    DiskDriver_freeBlock(disk, cur);
    cur = next;
  }
}

//legge l'n-esimo blocco della catena, con n=0 legge il primo.
//Restituisce -1 se non esiste
static int chain_at(DiskDriver* disk, FirstDirectoryBlock* dcb, int n, Block* out) {
  int cur = dcb->fcb.block_in_disk;
  memcpy(out, dcb, sizeof *dcb);
  for (int k = 0; k < n; k++) {
    cur = out->ff.header.next_block;
    if (cur == -1 || DiskDriver_readBlock(disk, out, cur) < 0) return -1;
  }
  return cur;
}

//numero di blocco dell'entry idx, -1 se non esiste
static int dir_get(DiskDriver* disk, FirstDirectoryBlock* dcb, int idx) {
  if (idx < 0 || idx >= dcb->num_entries) return -1;
  if (idx < DIR_FIRST_CAP) return dcb->file_blocks[idx];
  Block b;
  int j = idx - DIR_FIRST_CAP;
  return chain_at(disk, dcb, 1 + j / DIR_NEXT_CAP, &b) < 0 ? -1 : b.db.file_blocks[j % DIR_NEXT_CAP];
}

//scrive l'entry idx e salva il blocco che la contiene
static int dir_put(DiskDriver* disk, FirstDirectoryBlock* dcb, int idx, int val) {
  if (idx < DIR_FIRST_CAP) {
    dcb->file_blocks[idx] = val;
    return DiskDriver_writeBlock(disk, dcb, dcb->fcb.block_in_disk);
  }
  Block b;
  int j = idx - DIR_FIRST_CAP;
  int ret = chain_at(disk, dcb, 1 + j / DIR_NEXT_CAP, &b);
  if (ret < 0) return -1;
  b.db.file_blocks[j % DIR_NEXT_CAP] = val;
  return DiskDriver_writeBlock(disk, &b, ret);
}

//ridimensiona l'intera catena di blocchi di directory, poi salva dcb
static int dir_resize(DiskDriver* disk, FirstDirectoryBlock* dcb, int n) {
  int have = DIR_BLOCKS(dcb->num_entries), need = DIR_BLOCKS(n);
  if (need != have) {
    Block b;
    int low = (have < need ? have : need) - 1;      // ultimo blocco che resta in catena
    int keep = chain_at(disk, dcb, low, &b);
    if (keep < 0) return -1;
    if (need > have) {
      int nb = DiskDriver_getFreeBlock(disk, 0);
      if (nb < 0) return -1;                        // disco pieno: nulla è stato modificato
      Block nbk;
      memset(&nbk, 0, sizeof nbk);
      nbk.db.header.previous_block = keep;
      nbk.db.header.next_block = -1;
      nbk.db.header.block_in_file = have;
      if (DiskDriver_writeBlock(disk, &nbk, nb) < 0) return -1;
      b.ff.header.next_block = nb;
    } 
    else 
    {
      DiskDriver_freeBlock(disk, b.ff.header.next_block);
      b.ff.header.next_block = -1;
    }
    if (low == 0) dcb->header.next_block = b.ff.header.next_block;   // il primo blocco è la copia in memoria
    else if (DiskDriver_writeBlock(disk, &b, keep) < 0) return -1;
  }
  dcb->num_entries = n;
  dcb->fcb.size_in_blocks = DIR_BLOCKS(n);
  return DiskDriver_writeBlock(disk, dcb, dcb->fcb.block_in_disk);
}

//Metodi che rappresentano semplici QoL 


//controlla che n sia un nome valido per un file o una directory
static int valid_name(const char* n) {
  size_t l = n ? strlen(n) : 0;
  return l > 0 && l < (size_t)NAME_SIZE && !strchr(n, '/') && strcmp(n, ".") && strcmp(n, "..");
}

//cerca la directory per una entry di nome "name"
static int dir_lookup(DiskDriver* disk, FirstDirectoryBlock* dcb, const char* name, int* idx, Block* out) {
  Block tmp;
  if (!out) out = &tmp;
  for (int i = 0; i < dcb->num_entries; i++) {
    int b = dir_get(disk, dcb, i);
    if (b < 0 || DiskDriver_readBlock(disk, out, b) < 0) continue;
    out->ff.fcb.name[NAME_SIZE - 1] = '\0';
    if (!strcmp(out->ff.fcb.name, name)) {
      if (idx) *idx = i;
      return b;
    }
  }
  return -1;
}

static FirstDirectoryBlock* load_dir(DiskDriver* disk, int blk) {
  Block b;
  if (blk < 0 || DiskDriver_readBlock(disk, &b, blk) < 0 || !b.ff.fcb.is_dir) return NULL;
  FirstDirectoryBlock* d = malloc(sizeof *d);
  if (d) memcpy(d, &b.fd, sizeof *d);
  return d;
}

//rimuove ricorsivamente una directory e tutti i suoi contenuti
static void remove_tree(DiskDriver* disk, int blk) {
  Block b;
  if (DiskDriver_readBlock(disk, &b, blk) < 0) return;
  if (b.ff.fcb.is_dir)
    for (int i = 0; i < b.fd.num_entries; i++) {
      int c = dir_get(disk, &b.fd, i);
      if (c >= 0) remove_tree(disk, c);
    }
  free_chain(disk, blk);
}

//controlliamo se siamo nella cwd o fuori
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

//resetta il puntatore interno al blocco corrente della directory
static void dir_handle_reset(DirectoryHandle* d) {
  d->current_block = &d->dcb->header;
  d->pos_in_dir = 0;
  d->pos_in_block = 0;
}

//sincronizza la directory con il disco
static int dir_sync(DirectoryHandle* d) {
  Block b;
  if (DiskDriver_readBlock(d->sfs->disk, &b, d->dcb->fcb.block_in_disk) < 0 || !b.ff.fcb.is_dir) return -1;
  memcpy(d->dcb, &b.fd, sizeof *d->dcb);
  return 0;
}


//inizializza il filesystem sul disco puntato da fs
void SimpleFS_format(SimpleFS* fs) {
  DiskDriver* disk = fs->disk;
  memset(disk->bitmap_data, 0, (size_t)disk->header->bitmap_entries);
  disk->header->free_blocks = disk->header->num_blocks;
  disk->header->first_free_block = 0;

  Block b;
  memset(&b, 0, sizeof b);
  int root = DiskDriver_getFreeBlock(disk, 0);      //sara' il blocco 0
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

//inizializza il filesystem creato con SimpleFS_format 
DirectoryHandle* SimpleFS_init(SimpleFS* fs, DiskDriver* disk) {
  fs->disk = disk;
  fs->cwd = NULL;
  FirstDirectoryBlock* root = load_dir(disk, 0);

  //disco non formattato
  if (!root || root->fcb.directory_block != -1 || strcmp(root->fcb.name, "/")) {
    free(root);
    return NULL;                                   
  }

  //disco formattato correttamente, siamo ora in /, la root directory
  DirectoryHandle* h = calloc(1, sizeof *h);
  h->sfs = fs;
  h->dcb = root;
  h->directory = NULL;                              
  dir_handle_reset(h);
  fs->cwd = h;
  return h;
}

//crea un file o una directory nella directory d
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
  int n = dcb->num_entries;                        // aggiunge b in coda alle entry
  if (dir_resize(disk, dcb, n + 1) < 0 || dir_put(disk, dcb, n, b) < 0) {
    DiskDriver_freeBlock(disk, b);
    return -1;
  }
  return b;
}

//restituisce l'handle per il file appena creato
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


//mappa la posizione di un file ai suoi indici di blocco, offset e capacità.
static void locate(int pos, int* k, int* off, int* cap) {
  if (pos < FIRST_DATA) { *k = 0; *off = pos; *cap = FIRST_DATA; }
  else {
    *k = 1 + (pos - FIRST_DATA) / NEXT_DATA;
    *off = (pos - FIRST_DATA) % NEXT_DATA;
    *cap = NEXT_DATA;
  }
}

//carica in *out il blocco in posizione k, con k >=1 e ne ritorna il numero del blocco sul disco.
//se alloc è true, alloca un nuovo blocco in caso non esiste, altrimenti ritorna -1
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
  Block b;
  int n = d->dcb->num_entries;
  for (int i = 0; i < n; i++) {
    int blk = dir_get(disk, d->dcb, i);
    if (blk < 0 || DiskDriver_readBlock(disk, &b, blk) < 0) return -1;
    strncpy(names[i], b.ff.fcb.name, NAME_SIZE - 1);
    names[i][NAME_SIZE - 1] = '\0';
  }
  return n;
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
  remove_tree(disk, blk);
  int last = d->dcb->num_entries - 1;                 // l'ultima entry prende il posto di quella rimossa
  if (idx != last && dir_put(disk, d->dcb, idx, dir_get(disk, d->dcb, last)) < 0) return -1;
  return dir_resize(disk, d->dcb, last) < 0 ? -1 : 0;
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