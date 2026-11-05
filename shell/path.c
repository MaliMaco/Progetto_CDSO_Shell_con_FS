#include "path.h"
#include <stdlib.h>
#include <string.h>

#define PATH_BUF 512


static void go_root(DirectoryHandle* h) {
  char up[] = "..";
  while (h->directory) SimpleFS_changeDir(h, up);
}

DirectoryHandle* path_walk(DirectoryHandle* cwd, const char* path) {
  char buf[PATH_BUF];
  if (strlen(path) >= sizeof buf) return NULL;
  strcpy(buf, path);
  DirectoryHandle* h = SimpleFS_cloneDir(cwd);
  if (!h) return NULL;
  if (buf[0] == '/') go_root(h);
  char* save;
  for (char* t = strtok_r(buf, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
    if (SimpleFS_changeDir(h, t) < 0) {
      SimpleFS_closeDir(h);
      return NULL;
    }
  }
  return h;
}

DirectoryHandle* path_parent(DirectoryHandle* cwd, const char* path, char* leaf, size_t leaf_size) {
  char buf[PATH_BUF];
  if (strlen(path) >= sizeof buf) return NULL;
  strcpy(buf, path);
  size_t len = strlen(buf);
  while (len > 1 && buf[len - 1] == '/') buf[--len] = '\0';

  char* slash = strrchr(buf, '/');
  const char* base = slash ? slash + 1 : buf;
  if (!*base || strlen(base) >= leaf_size) return NULL;
  strcpy(leaf, base);

  if (!slash) return SimpleFS_cloneDir(cwd);
  if (slash == buf) return path_walk(cwd, "/");
  *slash = '\0';
  return path_walk(cwd, buf);
}

int path_pwd(DirectoryHandle* cwd, char* out, size_t n) {
  char tmp[PATH_BUF];
  size_t pos = sizeof tmp - 1;
  tmp[pos] = '\0';
  DirectoryHandle* h = SimpleFS_cloneDir(cwd);
  char up[] = "..";
  while (h && h->directory) {
    size_t l = strlen(h->dcb->fcb.name);
    if (pos < l + 1) break;
    pos -= l;
    memcpy(tmp + pos, h->dcb->fcb.name, l);
    tmp[--pos] = '/';
    if (SimpleFS_changeDir(h, up) < 0) break;
  }
  SimpleFS_closeDir(h);
  strncpy(out, pos == sizeof tmp - 1 ? "/" : tmp + pos, n - 1);
  out[n - 1] = '\0';
  return 0;
}

void path_commit(DirectoryHandle* dst, DirectoryHandle* src) {
  free(dst->dcb);
  free(dst->directory);
  dst->dcb = src->dcb;
  dst->directory = src->directory;
  dst->current_block = &dst->dcb->header;
  dst->pos_in_dir = 0;
  dst->pos_in_block = 0;
  free(src);
}
