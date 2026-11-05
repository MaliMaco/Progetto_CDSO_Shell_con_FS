#pragma once
#include <sys/types.h>

//Implementazione del controllo dei processi: permette il lancio di processi esterni, come ad esempio sleep, e
//di poterli fermare con ctrl-z, di poterli mettere in primo piano con fg, in background con bg, e di poterli visualizzare con jobs

typedef enum { J_FREE, J_RUNNING, J_STOPPED } JobState;

typedef struct {
  int id;
  pid_t pgid;
  JobState state;
  char cmd[128];
} Job;

//attiva il controllo dei processi, da chiamare all'inizio della shell
void jobs_init(void);
//esegue comandi esterni e li tratta come se fossero job nella shell
void jobs_launch(char** argv, const char* cmdline, int background);
//aggiorna lo stato dei job
void jobs_update(void);
void jobs_print(void);
int  jobs_fg(const char* spec);
int  jobs_bg(const char* spec);
//uccide tutti i processi sospesi all'uscita dalla shell
void jobs_kill_all(void);
