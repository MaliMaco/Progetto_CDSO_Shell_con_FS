#pragma once
#include <stddef.h>

// divide la riga in token (modificandola); i doppi apici raggruppano parole con spazi
int parse_tokenize(char* line, char** argv, int max);

// legge una dimensione in byte con suffisso opzionale K o M; 0 se ok, -1 se non valida
int parse_size(const char* s, size_t* out);
