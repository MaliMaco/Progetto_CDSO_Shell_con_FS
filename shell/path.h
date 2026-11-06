#pragma once
#include <stddef.h>
#include "simplefs.h"

//I path vengono risolti su una copia del DirectoryHandle, senza modificare subito la cwd.

//ci spostiamo nella directory indicata da path, NULL se non esiste
DirectoryHandle* path_walk(DirectoryHandle* cwd, const char* path);

//divide path nella directory parente e in una foglia, che contiene l'ultimo elemento del path;
//se slash è vuoto, la directory parente e' cwd, se coincide con / è la root, altrimenti camminiamo
//verso la directory indicata da leaf
DirectoryHandle* path_parent(DirectoryHandle* cwd, const char* path, char* leaf, size_t leaf_size);

//stampa nella shell il path assoluto della directory corrente
int path_pwd(DirectoryHandle* cwd, char* out, size_t n);

//copia src in dst, liberando prima la directory corrente di dst per poi aggiornare
//dst con i dati di src, che viene poi liberato.
void path_commit(DirectoryHandle* dst, DirectoryHandle* src);
