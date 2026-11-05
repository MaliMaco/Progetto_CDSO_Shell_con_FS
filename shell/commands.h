#pragma once
#include "simplefs.h"

<<<<<<< HEAD
//Stato del filesystem aperto dalla shell.
typedef struct {
  DiskDriver disk;
  SimpleFS sfs;
  DirectoryHandle* cwd;     //NULL se nessun filesystem e' aperto
} FsContext;

//Ogni comando ritorna 0 se ok, -1 se errore
=======
// Stato del filesystem aperto dalla shell.
typedef struct {
  DiskDriver disk;
  SimpleFS sfs;
  DirectoryHandle* cwd;     // NULL se nessun filesystem e' aperto
} FsContext;

// Ogni comando ritorna 0 se ok, -1 se errore (il messaggio e' gia' stampato su stderr).
>>>>>>> origin
void ctx_init(FsContext* ctx);
int  ctx_is_open(const FsContext* ctx);

int cmd_format(FsContext* ctx, const char* file, const char* size);
int cmd_open(FsContext* ctx, const char* file);
int cmd_close(FsContext* ctx);
int cmd_mkdir(FsContext* ctx, const char* path);
int cmd_cd(FsContext* ctx, const char* path);
int cmd_pwd(FsContext* ctx);
int cmd_touch(FsContext* ctx, const char* path);
int cmd_cat(FsContext* ctx, const char* path);
int cmd_ls(FsContext* ctx, const char* path);           // path NULL = directory corrente
int cmd_append(FsContext* ctx, const char* path, const char* text);
int cmd_rm(FsContext* ctx, const char* path);
int cmd_import(FsContext* ctx, const char* host_path, const char* path);
<<<<<<< HEAD
int cmd_export(FsContext* ctx, const char* path, const char* host_path);
=======
int cmd_export(FsContext* ctx, const char* path, const char* host_path);
>>>>>>> origin
