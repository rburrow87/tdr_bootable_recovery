#ifndef NANDROID_H
#define NANDROID_H

int nandroid_main(int argc, char** argv);
int nandroid_backup(const char* backup_path);
int nandroid_advanced_backup(const char* backup_path, int boot, int recovery, int system, int data, int cache, int sdext, int wimax);
int nandroid_restore(const char* backup_path, int restore_boot, int restore_system, int restore_data, int restore_cache, int restore_sdext, int restore_wimax);

#endif
