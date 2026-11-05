#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "commands.h"
#include "parser.h"
#include "path.h"
#include "jobs.h"

#define MAX_ARGS 64
#define LINE_MAX_LEN 1024
#define N_COMMANDS ((int)(sizeof commands / sizeof commands[0]))

static FsContext ctx;

typedef struct {
  const char* name;
  const char* usage;
  int min_args, max_args;     //numero minimo e massimo di argomenti, si esclude il nome del comando
  int needs_fs;               //flag che indica se il comando richiede o no il FS
  int (*run)(int argc, char** argv); 
} Command;

//Dichiarazione dei comandi della shell. I (void)c e (void)v permettono di evitare i warning
static int do_help(int c, char** v);
static int do_append(int argc, char** argv);
static int do_fg(int c, char** v);
static int do_bg(int c, char** v);
static int do_format(int c, char** v) { (void)c; return cmd_format(&ctx, v[1], v[2]); }
static int do_open(int c, char** v)   { (void)c; return cmd_open(&ctx, v[1]); }
static int do_close(int c, char** v)  { (void)c; (void)v; return cmd_close(&ctx); }
static int do_mkdir(int c, char** v)  { (void)c; return cmd_mkdir(&ctx, v[1]); }
static int do_cd(int c, char** v)     { return cmd_cd(&ctx, c > 1 ? v[1] : NULL); }
static int do_pwd(int c, char** v)    { (void)c; (void)v; return cmd_pwd(&ctx); }
static int do_touch(int c, char** v)  { (void)c; return cmd_touch(&ctx, v[1]); }
static int do_cat(int c, char** v)    { (void)c; return cmd_cat(&ctx, v[1]); }
static int do_ls(int c, char** v)     { return cmd_ls(&ctx, c > 1 ? v[1] : NULL); }
static int do_rm(int c, char** v)     { (void)c; return cmd_rm(&ctx, v[1]); }
static int do_jobs(int c, char** v)   { (void)c; (void)v; jobs_print(); return 0; }

//tabella dei comandi della shell
static const Command commands[] = {
  {"format", "format <fs_filename> <size[K|M]>", 2, 2, 0, do_format},
  {"open",   "open <fs_filename>",               1, 1, 0, do_open},
  {"close",  "close",                            0, 0, 1, do_close},
  {"mkdir",  "mkdir <path>",                     1, 1, 1, do_mkdir},
  {"cd",     "cd [path]",                        0, 1, 1, do_cd},
  {"pwd",    "pwd",                              0, 0, 1, do_pwd},
  {"touch",  "touch <path>",                     1, 1, 1, do_touch},
  {"cat",    "cat <path>",                       1, 1, 1, do_cat},
  {"ls",     "ls [path]",                        0, 1, 1, do_ls},
  {"append", "append <file> <testo>",            2, MAX_ARGS, 1, do_append},
  {"rm",     "rm <path>",                        1, 1, 1, do_rm},
  {"jobs",   "jobs",                             0, 0, 0, do_jobs},
  {"fg",     "fg [%n]",                          0, 1, 0, do_fg},
  {"bg",     "bg [%n]",                          0, 1, 0, do_bg},
  {"help",   "help",                             0, 0, 0, do_help},
  {"exit",   "exit",                             0, 0, 0, NULL},
};

static int do_fg(int c, char** v) {
  if (jobs_fg(c > 1 ? v[1] : NULL) < 0) { fprintf(stderr, "fg: job inesistente\n"); return -1; }
  return 0;
}

static int do_bg(int c, char** v) {
  if (jobs_bg(c > 1 ? v[1] : NULL) < 0) { fprintf(stderr, "bg: job inesistente\n"); return -1; }
  return 0;
}

static int do_append(int argc, char** argv) {
  size_t len = 1;
  for (int i = 2; i < argc; i++) len += strlen(argv[i]) + 1;
  char* text = calloc(len, 1);
  for (int i = 2; i < argc; i++) {
    if (i > 2) strcat(text, " ");
    strcat(text, argv[i]);
  }
  int r = cmd_append(&ctx, argv[1], text);
  free(text);
  return r;
}

static int do_help(int c, char** v) {
  (void)c; (void)v;
  for (int i = 0; i < N_COMMANDS; i++) printf("  %s\n", commands[i].usage);
  puts("  <comando esterno> [&]   (ctrl-z lo sospende, fg/bg lo riprendono)");
  return 0;
}

//ogni comando che non è della shell parte come processo esterno; un '&' finale (anche attaccato) lo manda in background
static void launch(int argc, char** argv, const char* line) {
  char* last = argv[argc - 1];
  size_t n = strlen(last);
  int bg = last[n - 1] == '&';
  if (bg) {
    last[n - 1] = '\0';
    if (!*last) argv[--argc] = NULL;
  }
  if (argc) jobs_launch(argv, line, bg);
}

//esegue il comando della shell
static int execute(int argc, char** argv, const char* line) {
  for (int i = 0; i < N_COMMANDS; i++) {
    const Command* c = &commands[i];
    if (strcmp(argv[0], c->name)) continue;
    //entra in questo if sono se il comando che esegue è exit, uscendo dalla shell
    if (!c->run) return 1;                                      
    if (argc - 1 < c->min_args || argc - 1 > c->max_args) {
      fprintf(stderr, "uso: %s\n", c->usage);
    } 
    else if (c->needs_fs && !ctx_is_open(&ctx)) {
      fprintf(stderr, "nessun filesystem aperto (usa format o open)\n");
    } 
    else {
      c->run(argc, argv);
    }
    return 0;
  }
  launch(argc, argv, line);
  return 0;
}

static void prompt(void) {
  char p[512];
  if (ctx_is_open(&ctx)) {
    path_pwd(ctx.cwd, p, sizeof p);
    printf("fs:%s$ ", p);
  } 
  else {
    printf("shell$ ");
  }
  fflush(stdout);
}

int main(void) {
  ctx_init(&ctx);
  jobs_init();
  char line[LINE_MAX_LEN], orig[LINE_MAX_LEN];
  char* argv[MAX_ARGS];
  for (;;) {
    jobs_update();
    prompt();
    if (!fgets(line, sizeof line, stdin)) { putchar('\n'); break; }   //ctrl-d
    line[strcspn(line, "\n")] = '\0';
    strcpy(orig, line);
    int argc = parse_tokenize(line, argv, MAX_ARGS);
    if (argc && execute(argc, argv, orig)) break;
  }
  jobs_kill_all();
  if (ctx_is_open(&ctx)) cmd_close(&ctx);
  return 0;
}
