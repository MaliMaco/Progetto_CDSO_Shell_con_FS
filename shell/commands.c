#include "commands.h"
#include "parser.h"
#include "path.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LEAF_MAX 256

static int fail(const char* cmd, const char* what, const char* why) {
  fprintf(stderr, "%s: %s: %s\n", cmd, what, why);
  return -1;
}

void ctx_init(FsContext* ctx) {
  memset(ctx, 0, sizeof *ctx);
  ctx->disk.fd = -1;
}

int ctx_is_open(const FsContext* ctx) { return ctx->cwd != NULL; }

//monta il filesystem 
static int mount(FsContext* ctx, const char* file, int num_blocks, int do_format) {
  if (DiskDriver_init(&ctx->disk, file, num_blocks) < 0) return -1;
  ctx->sfs.disk = &ctx->disk;
  if (do_format) SimpleFS_format(&ctx->sfs);
  ctx->cwd = SimpleFS_init(&ctx->sfs, &ctx->disk);
  if (!ctx->cwd) {
    DiskDriver_close(&ctx->disk);
    return -1;
  }
  return 0;
}

//formatta creando un nuovo filesystem o sovrascrivendo il precedente
int cmd_format(FsContext* ctx, const char* file, const char* size) {
  size_t bytes;
  if (parse_size(size, &bytes) < 0) return fail("format", size, "dimensione non valida");
  int blocks = (int)(bytes / BLOCK_SIZE);
  if (blocks < 2) return fail("format", size, "dimensione troppo piccola");
  if (ctx_is_open(ctx)) cmd_close(ctx);

  int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0660);   // parte da un file vuoto
  if (fd < 0) return fail("format", file, "impossibile creare il file");
  close(fd);
  if (mount(ctx, file, blocks, 1) < 0) return fail("format", file, "formattazione fallita");
  return 0;
}

//apre un filesystem esistente
int cmd_open(FsContext* ctx, const char* file) {
  if (access(file, F_OK) < 0) return fail("open", file, "file inesistente");
  if (ctx_is_open(ctx)) cmd_close(ctx);
  if (mount(ctx, file, 0, 0) < 0) return fail("open", file, "non e' un filesystem valido");
  return 0;
}

//chiude il filesystem attualmente aperto
int cmd_close(FsContext* ctx) {
  SimpleFS_closeDir(ctx->cwd);
  ctx->cwd = NULL;
  ctx->sfs.cwd = NULL;
  return DiskDriver_close(&ctx->disk);
}

//cambio di directory
int cmd_cd(FsContext* ctx, const char* path) {
  DirectoryHandle* h = path_walk(ctx->cwd, path ? path : "/");
  if (!h) return fail("cd", path ? path : "/", "directory inesistente");
  path_commit(ctx->cwd, h);
  return 0;
}

//stampa la directory corrente
int cmd_pwd(FsContext* ctx) {
  char p[512];
  path_pwd(ctx->cwd, p, sizeof p);
  puts(p);
  return 0;
}

//crea una nuova directory
int cmd_mkdir(FsContext* ctx, const char* path) {
  char leaf[LEAF_MAX];
  DirectoryHandle* p = path_parent(ctx->cwd, path, leaf, sizeof leaf);
  if (!p) return fail("mkdir", path, "percorso non valido");
  int r = SimpleFS_mkDir(p, leaf);
  SimpleFS_closeDir(p);
  return r < 0 ? fail("mkdir", path, "esiste gia', nome non valido o disco pieno") : 0;
}

//crea un file vuoto
int cmd_touch(FsContext* ctx, const char* path) {
  char leaf[LEAF_MAX];
  DirectoryHandle* p = path_parent(ctx->cwd, path, leaf, sizeof leaf);
  if (!p) return fail("touch", path, "percorso non valido");
  FileHandle* f = SimpleFS_createFile(p, leaf);
  SimpleFS_close(f);
  SimpleFS_closeDir(p);
  return f ? 0 : fail("touch", path, "esiste gia', nome non valido o disco pieno");
}

//funzione ausiliare per cat
static FileHandle* open_file(FsContext* ctx, const char* cmd, const char* path) {
  char leaf[LEAF_MAX];
  DirectoryHandle* p = path_parent(ctx->cwd, path, leaf, sizeof leaf);
  if (!p) { fail(cmd, path, "percorso non valido"); return NULL; }
  FileHandle* f = SimpleFS_openFile(p, leaf);
  SimpleFS_closeDir(p);
  if (!f) fail(cmd, path, "file inesistente o e' una directory");
  return f;
}

//stampa nella shell il contenuto di un file
int cmd_cat(FsContext* ctx, const char* path) {
  FileHandle* f = open_file(ctx, "cat", path);
  if (!f) return -1;
  char buf[BLOCK_SIZE];
  int n;
  while ((n = SimpleFS_read(f, buf, sizeof buf)) > 0) fwrite(buf, 1, (size_t)n, stdout);
  SimpleFS_close(f);
  return 0;
}

//elenca il contenuto di una directory
int cmd_ls(FsContext* ctx, const char* path) {
  DirectoryHandle* d = path ? path_walk(ctx->cwd, path) : SimpleFS_cloneDir(ctx->cwd);
  if (!d) {
    // forse e' un file
    char leaf[LEAF_MAX];
    DirectoryHandle* p = path ? path_parent(ctx->cwd, path, leaf, sizeof leaf) : NULL;
    FileHandle* f = p ? SimpleFS_openFile(p, leaf) : NULL;
    SimpleFS_closeDir(p);
    if (!f) return fail("ls", path ? path : ".", "file o directory inesistente");
    printf("- %8d %s\n", f->fcb->fcb.size_in_bytes, leaf);
    SimpleFS_close(f);
    return 0;
  }
  int n = d->dcb->num_entries;
  if (n > 0) {
    char** names = malloc((size_t)n * sizeof(char*));
    char* pool = malloc((size_t)n * 128);
    for (int i = 0; i < n; i++) names[i] = pool + (size_t)i * 128;
    int got = SimpleFS_readDir(names, d);
    for (int i = 0; i < got; i++) {
      FileHandle* f = SimpleFS_openFile(d, names[i]);   // NULL: è una directory
      printf("%c %8d %s%s\n", f ? '-' : 'd', f ? f->fcb->fcb.size_in_bytes : 0, names[i], f ? "" : "/");
      SimpleFS_close(f);
    }
    free(names);
    free(pool);
  }
  SimpleFS_closeDir(d);
  return 0;
}

//aggiunge testo alla coda di un file
int cmd_append(FsContext* ctx, const char* path, const char* text) {
  FileHandle* f = open_file(ctx, "append", path);
  if (!f) return -1;
  size_t len = strlen(text);
  char* line = malloc(len + 2);
  memcpy(line, text, len);
  line[len++] = '\n';
  SimpleFS_seek(f, f->fcb->fcb.size_in_bytes);
  int w = SimpleFS_write(f, line, (int)len);
  free(line);
  SimpleFS_close(f);
  return w == (int)len ? 0 : fail("append", path, "disco pieno");
}

//rimuove un file o una directory
int cmd_rm(FsContext* ctx, const char* path) {
  char leaf[LEAF_MAX];
  DirectoryHandle* p = path_parent(ctx->cwd, path, leaf, sizeof leaf);
  if (!p) return fail("rm", path, "percorso non valido");
  int r = SimpleFS_removeIn(p, leaf);
  SimpleFS_closeDir(p);
  if (r == -2) return fail("rm", path, "contiene la directory corrente");
  return r < 0 ? fail("rm", path, "file o directory inesistente") : 0;
}