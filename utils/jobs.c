#include "jobs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_JOBS 32

static Job jobs[MAX_JOBS];
static pid_t shell_pgid;
static int interactive;

//assegna il terminale al processo che appartiene al process group pgid, permette di riportare i processi in foreground
static void give_terminal(pid_t pgid) {
  if (interactive) tcsetpgrp(STDIN_FILENO, pgid);
}

void jobs_init(void) {
  interactive = isatty(STDIN_FILENO);
  if (!interactive) return;
  //controlla che la shell sia in foreground, altrimenti si ferma
  while (tcgetpgrp(STDIN_FILENO) != (shell_pgid = getpgrp()))
    kill(-shell_pgid, SIGTTIN);
  //la shell del fs ignora i segnali del terminale non rischiando di essere sospeso
  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);
  shell_pgid = getpid();
  setpgid(shell_pgid, shell_pgid);
  tcsetpgrp(STDIN_FILENO, shell_pgid);
}

//azzera i vari jobs rimasti in background
static void job_free(Job* j) { memset(j, 0, sizeof *j); }

//aggiunge un nuovo job ai jobs in bg
static Job* job_add(pid_t pgid, const char* cmd) {
  for (int i = 0; i < MAX_JOBS; i++) {
    if (jobs[i].state == J_FREE) {
      jobs[i].id = i + 1;
      jobs[i].pgid = pgid;
      jobs[i].state = J_RUNNING;
      snprintf(jobs[i].cmd, sizeof jobs[i].cmd, "%s", cmd);
      return &jobs[i];
    }
  }
  return NULL;
}

//trova nella tabella dei jobs il job con pid == pgid
static Job* job_by_pid(pid_t pid) {
  for (int i = 0; i < MAX_JOBS; i++)
    if (jobs[i].state != J_FREE && jobs[i].pgid == pid) return &jobs[i];
  return NULL;
}

//se spec è NULL, restituisce l'ultimo job nella job table, altrimenti cerca il job specifico
static Job* job_by_spec(const char* spec) {
  if (spec) {
    if (*spec == '%') spec++;
    int id = atoi(spec);
    if (id < 1 || id > MAX_JOBS || jobs[id - 1].state == J_FREE) return NULL;
    return &jobs[id - 1];
  }
  for (int i = MAX_JOBS - 1; i >= 0; i--)
    if (jobs[i].state != J_FREE) return &jobs[i];
  return NULL;
}

//attende che il job in fg termini o venga fermato, ed in tal caso ridà il controllo del terminale alla shell
static void wait_fg(Job* j) {
  int st;
  pid_t r;
  do { r = waitpid(j->pgid, &st, WUNTRACED); } while (r < 0 && errno == EINTR);
  give_terminal(shell_pgid);
  if (r < 0) { job_free(j); return; }
  if (WIFSTOPPED(st)) {
    j->state = J_STOPPED;
    printf("\n[%d]+ Stopped\t%s\n", j->id, j->cmd);
  } 
  else {
    if (WIFSIGNALED(st)) printf("\n");
    job_free(j);
  }
}

//è la funzione che permette di lanciare comandi esterni nella shell interna del fs in fg o bg con i relativi controlli
void jobs_launch(char** argv, const char* cmdline, int background) {
  pid_t pid = fork();
  if (pid < 0) { perror("fork"); return; }
  //il figlio definisce un nuovo pgid di cui è a capo per gestire individualmente il nuovo job
  if (pid == 0) {
    setpgid(0, 0);
    if (!background) give_terminal(getpid());
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    execvp(argv[0], argv);
    perror(argv[0]);
    _exit(127);
  }

  //il padre inserisce nel suo gruppo il figlio e lo aggiunge alla tabella dei job
  setpgid(pid, pid);
  Job* j = job_add(pid, cmdline);
  if (!j) {
    fprintf(stderr, "troppi job attivi\n");
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return;
  }
  if (background) {
    printf("[%d] %d\n", j->id, (int)pid);
  } 
  else {
    give_terminal(pid);
    wait_fg(j);
  }
}

//aggiorna lo stato dei job e ne stampa lo stato attuale
void jobs_update(void) {
  int st;
  pid_t pid;
  while ((pid = waitpid(-1, &st, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
    Job* j = job_by_pid(pid);
    if (!j) continue;
    if (WIFSTOPPED(st)) {
      j->state = J_STOPPED;
      printf("[%d]+ Stopped\t%s\n", j->id, j->cmd);
    } 
    else if (WIFCONTINUED(st)) {
      j->state = J_RUNNING;
    } 
    else {
      printf("[%d]  %s\t%s\n", j->id, WIFEXITED(st) ? "Done" : "Terminated", j->cmd);
      job_free(j);
    }
  }
}

//stampa tutti i jobs presenti nella tabella 
void jobs_print(void) {
  for (int i = 0; i < MAX_JOBS; i++) {
    if (jobs[i].state == J_FREE) continue;
    printf("[%d] %-8s %s\n", jobs[i].id,
           jobs[i].state == J_STOPPED ? "Stopped" : "Running", jobs[i].cmd);
  }
}

//mette in foreground il job specificato da spec, se non specificato prende l'ultimo job
int jobs_fg(const char* spec) {
  Job* j = job_by_spec(spec);
  if (!j) return -1;
  printf("%s\n", j->cmd);
  give_terminal(j->pgid);
  if (j->state == J_STOPPED) kill(-j->pgid, SIGCONT);
  j->state = J_RUNNING;
  wait_fg(j);
  return 0;
}

//mette in background il job specificato da spec, se non specificato prende l'ultimo job
int jobs_bg(const char* spec) {
  Job* j = job_by_spec(spec);
  if (!j) return -1;
  if (j->state == J_STOPPED) kill(-j->pgid, SIGCONT);
  j->state = J_RUNNING;
  printf("[%d]+ %s &\n", j->id, j->cmd);
  return 0;
}

//uccide tutti i processi sospesi all'uscita dalla shell
void jobs_kill_all(void) {
  for (int i = 0; i < MAX_JOBS; i++) {
    if (jobs[i].state == J_FREE) continue;
    kill(-jobs[i].pgid, SIGHUP);
    kill(-jobs[i].pgid, SIGCONT);
  }
}
