/*
    debugwrap.h - DEBUG BUILD: automatic logging of file, memory and network calls

    Copyright (C) 2025  Retro Rewind Team

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/*
    Include this header as the LAST #include of a .c file. It replaces the common libc calls
    (fopen, fread, stat, opendir, malloc, exit, ...) and curl_easy_perform with logging versions
    that record the call site, the arguments, the result and errno, and run a detailed
    diagnosis when something fails.

    Successful calls of fread/fwrite/malloc are not logged one by one (that would be far too much),
    but per-file statistics are logged on fclose.
*/

#ifndef RRC_DEBUGWRAP_H
#define RRC_DEBUGWRAP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <curl/curl.h>

#include "debuglog.h"

#define RRC_DBGW_SITE const char *_file, int _line, const char *_func

FILE *rrc_dbgw_fopen(const char *path, const char *mode, RRC_DBGW_SITE);
int rrc_dbgw_fclose(FILE *f, RRC_DBGW_SITE);
size_t rrc_dbgw_fread(void *buf, size_t size, size_t n, FILE *f, RRC_DBGW_SITE);
size_t rrc_dbgw_fwrite(const void *buf, size_t size, size_t n, FILE *f, RRC_DBGW_SITE);
int rrc_dbgw_fseek(FILE *f, long offset, int whence, RRC_DBGW_SITE);
int rrc_dbgw_stat(const char *path, struct stat *st, RRC_DBGW_SITE);
int rrc_dbgw_mkdir(const char *path, mode_t mode, RRC_DBGW_SITE);
int rrc_dbgw_remove(const char *path, RRC_DBGW_SITE);
int rrc_dbgw_unlink(const char *path, RRC_DBGW_SITE);
int rrc_dbgw_rename(const char *from, const char *to, RRC_DBGW_SITE);
DIR *rrc_dbgw_opendir(const char *path, RRC_DBGW_SITE);
int rrc_dbgw_closedir(DIR *dir, RRC_DBGW_SITE);
int rrc_dbgw_chdir(const char *path, RRC_DBGW_SITE);
int rrc_dbgw_statvfs(const char *path, struct statvfs *buf, RRC_DBGW_SITE);
ssize_t rrc_dbgw_read(int fd, void *buf, size_t n, RRC_DBGW_SITE);
void *rrc_dbgw_malloc(size_t n, RRC_DBGW_SITE);
void *rrc_dbgw_realloc(void *p, size_t n, RRC_DBGW_SITE);
void rrc_dbgw_exit(int code, RRC_DBGW_SITE) __attribute__((noreturn));
CURLcode rrc_dbgw_curl_easy_perform(CURL *curl, RRC_DBGW_SITE);

/* Logs all files currently opened through the wrappers (leak detection). */
void rrc_dbgw_log_open_files(void);

/* If the most recent failed file operation was not diagnosed in detail yet (e.g. ENOENT, which is
   often expected), run the diagnosis now. Called when the code turns an errno into an error result,
   i.e. when the failure turned out to matter. `eno' must match the errno of that failure. */
void rrc_dbgw_diagnose_pending(int eno);

#ifndef RRC_DEBUGWRAP_NO_MACROS

#define _RRC_DBGW_AT __FILE__, __LINE__, __func__

#define fopen(path, mode) rrc_dbgw_fopen((path), (mode), _RRC_DBGW_AT)
#define fclose(f) rrc_dbgw_fclose((f), _RRC_DBGW_AT)
#define fread(buf, size, n, f) rrc_dbgw_fread((buf), (size), (n), (f), _RRC_DBGW_AT)
#define fwrite(buf, size, n, f) rrc_dbgw_fwrite((buf), (size), (n), (f), _RRC_DBGW_AT)
#define fseek(f, offset, whence) rrc_dbgw_fseek((f), (offset), (whence), _RRC_DBGW_AT)
#define stat(path, st) rrc_dbgw_stat((path), (st), _RRC_DBGW_AT)
#define mkdir(path, mode) rrc_dbgw_mkdir((path), (mode), _RRC_DBGW_AT)
#define remove(path) rrc_dbgw_remove((path), _RRC_DBGW_AT)
#define unlink(path) rrc_dbgw_unlink((path), _RRC_DBGW_AT)
#define rename(from, to) rrc_dbgw_rename((from), (to), _RRC_DBGW_AT)
#define opendir(path) rrc_dbgw_opendir((path), _RRC_DBGW_AT)
#define closedir(d) rrc_dbgw_closedir((d), _RRC_DBGW_AT)
#define chdir(path) rrc_dbgw_chdir((path), _RRC_DBGW_AT)
#define statvfs(path, buf) rrc_dbgw_statvfs((path), (buf), _RRC_DBGW_AT)
#define read(fd, buf, n) rrc_dbgw_read((fd), (buf), (n), _RRC_DBGW_AT)
#define malloc(n) rrc_dbgw_malloc((n), _RRC_DBGW_AT)
#define realloc(p, n) rrc_dbgw_realloc((p), (n), _RRC_DBGW_AT)
#define exit(code) rrc_dbgw_exit((code), _RRC_DBGW_AT)
#define curl_easy_perform(curl) rrc_dbgw_curl_easy_perform((curl), _RRC_DBGW_AT)

#endif /* RRC_DEBUGWRAP_NO_MACROS */

#endif
