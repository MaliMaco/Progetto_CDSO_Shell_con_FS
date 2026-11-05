# Progetto_CDSO_Shell_con_FS

  - Progetto del corso di CDSO che punta ad implementare un FS che prevede file mmappati e persistenti e l'implementazione dei metodi classici e di libreria per la manipolazione dei file.

# Obbiettivi

  - format "fs_filename" "size"
  - mkdir
  - cd
  - touch (crea un file vuoto)
  - cat (stampa il contenuto di un file su schermo)
  - ls
  - append "file" "testo"
  - rm "dir/file"
  - close
  
  
## Compilazione e uso

```
cmake -S . -B build
cmake --build build
cd build && ./fs_shell
```
