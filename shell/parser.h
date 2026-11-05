#pragma once
#include <stddef.h>

<<<<<<< HEAD
//tokenizza la riga; i doppi apici raggruppano parole con spazi
int parse_tokenize(char* line, char** argv, int max);

//legge una dimensione in byte con suffisso opzionale K o M; 0 se ok, -1 se non valida
=======
// divide la riga in token (modificandola); i doppi apici raggruppano parole con spazi
int parse_tokenize(char* line, char** argv, int max);

// legge una dimensione in byte con suffisso opzionale K o M; 0 se ok, -1 se non valida
>>>>>>> origin
int parse_size(const char* s, size_t* out);
