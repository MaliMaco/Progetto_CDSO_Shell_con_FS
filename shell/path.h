#pragma once
#include <stddef.h>
#include "simplefs.h"

<<<<<<< HEAD
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
=======
// Il filesystem ragiona solo per nome nella directory corrente: qui i path
// (assoluti o relativi, con "." e "..") vengono risolti lavorando su COPIE
// del DirectoryHandle, cosi' la directory corrente non cambia finche' non si vuole.

// directory indicata da path (deve essere una directory); NULL se non esiste. Da chiudere con SimpleFS_closeDir
DirectoryHandle* path_walk(DirectoryHandle* cwd, const char* path);

// directory che contiene l'ultimo componente di path; l'ultimo componente va in leaf
DirectoryHandle* path_parent(DirectoryHandle* cwd, const char* path, char* leaf, size_t leaf_size);

// scrive in out il path assoluto della directory corrente
int path_pwd(DirectoryHandle* cwd, char* out, size_t n);

// fa diventare 'dst' uguale a 'src' (src viene consumato)
>>>>>>> origin
void path_commit(DirectoryHandle* dst, DirectoryHandle* src);
