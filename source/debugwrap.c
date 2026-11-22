/*
    debugwrap.c - DEBUG BUILD: automatic logging of file, memory and network calls

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

/* We need the real functions in here. */
#define RRC_DEBUGWRAP_NO_MACROS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <malloc.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#include "debugwrap.h"
#include "debuglog.h"

#define WLOG(level, ...) rrc_log_write((level), _file, _line, _func, __VA_ARGS__)

/* ------------------------------------------------------------------------------------------------ */
/* Open file tracking (statistics per file, leak detection)                                         */
/* ------------------------------------------------------------------------------------------------ */

#define MAX_TRACKED 48

struct tracked_file
{
    FILE *file;
    char path[112];
    char mode[8];
    char opened_at[48]; /* "file.c:123" */
    u32 read_bytes, read_calls;
    u32 written_bytes, written_calls;
    u64 open_ticks;
    bool eof_logged;
};

static struct tracked_file tracked[MAX_TRACKED];
static bool table_full_logged = false;

static struct tracked_file *track_find(FILE *f)
{
    if (!f)
        return NULL;
    for (int i = 0; i < MAX_TRACKED; i++)
    {
        if (tracked[i].file == f)
            return &tracked[i];
    }
    return NULL;
}

static const char *track_path(FILE *f)
{
    struct tracked_file *t = track_find(f);
    return t ? t->path : "<untracked FILE>";
}

void rrc_dbgw_log_open_files(void)
{
    int count = 0;
    for (int i = 0; i < MAX_TRACKED; i++)
    {
        if (!tracked[i].file)
            continue;
        if (count == 0)
            RRC_LOGI("  files currently open (opened via the launcher):");
        RRC_LOGI("    %p \"%s\" mode=%s opened at %s, %u ms ago", (void *)tracked[i].file, tracked[i].path,
                 tracked[i].mode, tracked[i].opened_at, (unsigned)diff_msec(tracked[i].open_ticks, gettime()));
        count++;
    }

    if (count == 0)
        RRC_LOGI("  no files currently open (via the launcher)");
    else if (count > 4)
        RRC_LOGW("  %d files open at once - possible FILE leak", count);
}

static void track_add(FILE *f, const char *path, const char *mode, const char *file, int line)
{
    for (int i = 0; i < MAX_TRACKED; i++)
    {
        if (tracked[i].file)
            continue;

        struct tracked_file *t = &tracked[i];
        memset(t, 0, sizeof(*t));
        t->file = f;
        snprintf(t->path, sizeof(t->path), "%s", path);
        snprintf(t->mode, sizeof(t->mode), "%s", mode);
        snprintf(t->opened_at, sizeof(t->opened_at), "%s:%d", rrc_log_basename(file), line);
        t->open_ticks = gettime();
        return;
    }

    if (!table_full_logged)
    {
        table_full_logged = true;
        RRC_LOGW("more than %d files open at the same time - probable FILE leak (fclose missing somewhere)", MAX_TRACKED);
        rrc_dbgw_log_open_files();
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Failure handling                                                                                 */
/* ------------------------------------------------------------------------------------------------ */

/* The last "file not found" that was only logged as INFO (it is often expected, for example for
   optional files). If the program later turns it into a real error, rrc_dbgw_diagnose_pending()
   runs the detailed diagnosis for it. */
static struct
{
    bool pending;
    char op[16];
    char path[160];
    int err;
    char file[64];
    int line;
    char func[64];
} not_found;

static void remember_not_found(const char *op, const char *path, int err, RRC_DBGW_SITE)
{
    not_found.pending = true;
    snprintf(not_found.op, sizeof(not_found.op), "%s", op);
    snprintf(not_found.path, sizeof(not_found.path), "%s", path ? path : "<NULL>");
    not_found.err = err;
    snprintf(not_found.file, sizeof(not_found.file), "%s", _file);
    not_found.line = _line;
    snprintf(not_found.func, sizeof(not_found.func), "%s", _func);
}

void rrc_dbgw_diagnose_pending(int eno)
{
    if (!not_found.pending || not_found.err != eno)
        return;

    not_found.pending = false;
    RRC_LOGW("the earlier %s(\"%s\") failure (%s) became an error -> running the detailed diagnosis now",
             not_found.op, not_found.path, rrc_log_errno_name(not_found.err));
    rrc_log_diagnose_path(not_found.op, not_found.path, not_found.err, not_found.file, not_found.line, not_found.func);
}

/* Logs a failed call as an error and runs the diagnosis. */
static void report_failure(const char *op, const char *path, int err, RRC_DBGW_SITE)
{
    WLOG(RRC_LOG_ERROR, "%s(\"%s\") FAILED: " RRC_LOG_ERRNO_FMT, op, path ? path : "<NULL>", RRC_LOG_ERRNO_ARGS(err));
    rrc_log_diagnose_path(op, path, err, _file, _line, _func);
}

/* A missing file is only logged. See `not_found' above. */
static void report_missing(const char *op, const char *path, int err, RRC_DBGW_SITE)
{
    WLOG(RRC_LOG_INFO, "%s(\"%s\") -> not found (ENOENT)", op, path);
    remember_not_found(op, path, err, _file, _line, _func);
}

/* ------------------------------------------------------------------------------------------------ */
/* stdio                                                                                            */
/* ------------------------------------------------------------------------------------------------ */

FILE *rrc_dbgw_fopen(const char *path, const char *mode, RRC_DBGW_SITE)
{
    int saved_errno = errno;
    u64 start = gettime();

    errno = 0;
    FILE *f = fopen(path, mode);
    int err = errno;
    u32 ms = diff_msec(start, gettime());

    if (f)
    {
        long long size = -1;
        struct stat st;
        if (fstat(fileno(f), &st) == 0)
            size = st.st_size;
        track_add(f, path, mode, _file, _line);

        WLOG(RRC_LOG_DEBUG, "fopen(\"%s\", \"%s\") -> OK fp=%p, size %lld bytes (%u ms)", path, mode, (void *)f, size, (unsigned)ms);
        if (ms > 1000)
            WLOG(RRC_LOG_WARN, "fopen(\"%s\") took %u ms - very slow SD access", path, (unsigned)ms);
        errno = saved_errno;
        return f;
    }

    bool for_writing = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
    if (err == ENOENT && !for_writing)
        report_missing("fopen", path, err, _file, _line, _func);
    else
        report_failure(for_writing ? "fopen-write" : "fopen", path, err, _file, _line, _func);

    errno = err;
    return NULL;
}

int rrc_dbgw_fclose(FILE *f, RRC_DBGW_SITE)
{
    int saved_errno = errno;

    if (!f)
    {
        WLOG(RRC_LOG_ERROR, "fclose(NULL) called - bug in the caller");
        return fclose(f);
    }

    struct tracked_file *t = track_find(f);
    char path[112];
    snprintf(path, sizeof(path), "%s", t ? t->path : "<untracked FILE>");

    errno = 0;
    int result = fclose(f);
    int err = errno;

    if (t)
    {
        WLOG(result == 0 ? RRC_LOG_DEBUG : RRC_LOG_ERROR,
             "fclose(\"%s\") -> %d [read %u B in %u calls, wrote %u B in %u calls, open %u ms]%s", path, result,
             (unsigned)t->read_bytes, (unsigned)t->read_calls, (unsigned)t->written_bytes, (unsigned)t->written_calls,
             (unsigned)diff_msec(t->open_ticks, gettime()), result == 0 ? "" : " FAILED");
        t->file = NULL;
    }
    else
    {
        WLOG(result == 0 ? RRC_LOG_DEBUG : RRC_LOG_ERROR, "fclose(%p untracked) -> %d", (void *)f, result);
    }

    if (result != 0)
    {
        WLOG(RRC_LOG_ERROR, "fclose failed: " RRC_LOG_ERRNO_FMT " - buffered data may not have reached the SD card",
             RRC_LOG_ERRNO_ARGS(err));
        report_failure("fclose", path, err, _file, _line, _func);
        errno = err;
    }
    else
    {
        errno = saved_errno;
    }
    return result;
}

size_t rrc_dbgw_fread(void *buf, size_t size, size_t n, FILE *f, RRC_DBGW_SITE)
{
    int saved_errno = errno;

    errno = 0;
    size_t result = fread(buf, size, n, f);
    int err = errno;

    struct tracked_file *t = track_find(f);
    if (t)
    {
        t->read_calls++;
        t->read_bytes += result * size;
    }

    if (result != n)
    {
        if (f && ferror(f))
        {
            WLOG(RRC_LOG_ERROR, "fread(\"%s\", %u x %u) READ ERROR after %u items: " RRC_LOG_ERRNO_FMT,
                 track_path(f), (unsigned)size, (unsigned)n, (unsigned)result, RRC_LOG_ERRNO_ARGS(err));
            rrc_log_diagnose_path("fread", track_path(f), err, _file, _line, _func);
            errno = err;
            return result;
        }
        if (t && !t->eof_logged)
        {
            t->eof_logged = true;
            WLOG(RRC_LOG_DEBUG, "fread(\"%s\") reached end of file (got %u of %u items, %u bytes read in total)",
                 track_path(f), (unsigned)result, (unsigned)n, (unsigned)t->read_bytes);
        }
    }

    errno = (result != n) ? err : saved_errno;
    return result;
}

size_t rrc_dbgw_fwrite(const void *buf, size_t size, size_t n, FILE *f, RRC_DBGW_SITE)
{
    int saved_errno = errno;

    errno = 0;
    size_t result = fwrite(buf, size, n, f);
    int err = errno;

    struct tracked_file *t = track_find(f);
    if (t)
    {
        t->written_calls++;
        t->written_bytes += result * size;
    }

    if (result != n)
    {
        WLOG(RRC_LOG_ERROR, "fwrite(\"%s\", %u x %u) SHORT WRITE: only %u items written: " RRC_LOG_ERRNO_FMT,
             track_path(f), (unsigned)size, (unsigned)n, (unsigned)result, RRC_LOG_ERRNO_ARGS(err));
        if (err == ENOSPC)
            WLOG(RRC_LOG_ERROR, "the SD card is full");
        rrc_log_diagnose_path("fwrite", track_path(f), err, _file, _line, _func);
        errno = err;
        return result;
    }

    errno = saved_errno;
    return result;
}

int rrc_dbgw_fseek(FILE *f, long offset, int whence, RRC_DBGW_SITE)
{
    errno = 0;
    int result = fseek(f, offset, whence);
    int err = errno;

    if (result != 0)
    {
        WLOG(RRC_LOG_ERROR, "fseek(\"%s\", %ld, %d) FAILED: " RRC_LOG_ERRNO_FMT, track_path(f), offset, whence,
             RRC_LOG_ERRNO_ARGS(err));
        errno = err;
    }
    else
    {
        WLOG(RRC_LOG_DEBUG, "fseek(\"%s\", %ld, %d) -> OK", track_path(f), offset, whence);
    }
    return result;
}

/* ------------------------------------------------------------------------------------------------ */
/* POSIX file system                                                                                */
/* ------------------------------------------------------------------------------------------------ */

int rrc_dbgw_stat(const char *path, struct stat *st, RRC_DBGW_SITE)
{
    int saved_errno = errno;

    errno = 0;
    int result = stat(path, st);
    int err = errno;

    if (result == 0)
    {
        WLOG(RRC_LOG_DEBUG, "stat(\"%s\") -> OK, %s, %lld bytes", path, S_ISDIR(st->st_mode) ? "dir" : "file", (long long)st->st_size);
        errno = saved_errno;
        return result;
    }

    if (err == ENOENT)
        report_missing("stat", path, err, _file, _line, _func);
    else
        report_failure("stat", path, err, _file, _line, _func);

    errno = err;
    return result;
}

int rrc_dbgw_mkdir(const char *path, mode_t mode, RRC_DBGW_SITE)
{
    errno = 0;
    int result = mkdir(path, mode);
    int err = errno;

    if (result == 0)
        WLOG(RRC_LOG_INFO, "mkdir(\"%s\") -> created", path);
    else if (err == EEXIST)
        WLOG(RRC_LOG_DEBUG, "mkdir(\"%s\") -> already exists", path);
    else
        report_failure("mkdir", path, err, _file, _line, _func);

    errno = err;
    return result;
}

static int log_remove_result(const char *op, const char *path, int result, int err, RRC_DBGW_SITE)
{
    if (result == 0)
        WLOG(RRC_LOG_INFO, "%s(\"%s\") -> deleted", op, path);
    else if (err == ENOENT)
        report_missing(op, path, err, _file, _line, _func);
    else
        report_failure(op, path, err, _file, _line, _func);

    errno = err;
    return result;
}

int rrc_dbgw_remove(const char *path, RRC_DBGW_SITE)
{
    errno = 0;
    int result = remove(path);
    return log_remove_result("remove", path, result, errno, _file, _line, _func);
}

int rrc_dbgw_unlink(const char *path, RRC_DBGW_SITE)
{
    errno = 0;
    int result = unlink(path);
    return log_remove_result("unlink", path, result, errno, _file, _line, _func);
}

int rrc_dbgw_rename(const char *from, const char *to, RRC_DBGW_SITE)
{
    errno = 0;
    int result = rename(from, to);
    int err = errno;

    if (result == 0)
        WLOG(RRC_LOG_INFO, "rename(\"%s\" -> \"%s\") -> OK", from, to);
    else
        report_failure("rename", from, err, _file, _line, _func);

    errno = err;
    return result;
}

DIR *rrc_dbgw_opendir(const char *path, RRC_DBGW_SITE)
{
    int saved_errno = errno;

    errno = 0;
    DIR *dir = opendir(path);
    int err = errno;

    if (dir)
    {
        WLOG(RRC_LOG_DEBUG, "opendir(\"%s\") -> OK", path);
        errno = saved_errno;
        return dir;
    }

    if (err == ENOENT)
        report_missing("opendir", path, err, _file, _line, _func);
    else
        report_failure("opendir", path, err, _file, _line, _func);

    errno = err;
    return NULL;
}

int rrc_dbgw_closedir(DIR *dir, RRC_DBGW_SITE)
{
    errno = 0;
    int result = closedir(dir);
    int err = errno;

    if (result != 0)
        WLOG(RRC_LOG_WARN, "closedir(%p) FAILED: " RRC_LOG_ERRNO_FMT, (void *)dir, RRC_LOG_ERRNO_ARGS(err));

    errno = err;
    return result;
}

int rrc_dbgw_chdir(const char *path, RRC_DBGW_SITE)
{
    errno = 0;
    int result = chdir(path);
    int err = errno;

    if (result == 0)
        WLOG(RRC_LOG_INFO, "chdir(\"%s\") -> OK", path);
    else
        report_failure("chdir", path, err, _file, _line, _func);

    errno = err;
    return result;
}

int rrc_dbgw_statvfs(const char *path, struct statvfs *buf, RRC_DBGW_SITE)
{
    errno = 0;
    int result = statvfs(path, buf);
    int err = errno;

    if (result == 0)
        WLOG(RRC_LOG_DEBUG, "statvfs(\"%s\") -> %llu MB free", path,
             (unsigned long long)buf->f_bfree * buf->f_frsize / (1024 * 1024));
    else
        report_failure("statvfs", path, err, _file, _line, _func);

    errno = err;
    return result;
}

ssize_t rrc_dbgw_read(int fd, void *buf, size_t n, RRC_DBGW_SITE)
{
    errno = 0;
    ssize_t result = read(fd, buf, n);
    int err = errno;

    if (result < 0)
    {
        WLOG(RRC_LOG_ERROR, "read(fd %d, %u bytes) FAILED (returned %d): " RRC_LOG_ERRNO_FMT
                            " - NOTE: callers that only check for 0 will continue with an uninitialised buffer",
             fd, (unsigned)n, (int)result, RRC_LOG_ERRNO_ARGS(err));
    }
    else if (result == 0 && n > 0)
    {
        WLOG(RRC_LOG_WARN, "read(fd %d, %u bytes) returned 0 (end of file / empty file)", fd, (unsigned)n);
    }
    else if ((size_t)result != n)
    {
        WLOG(RRC_LOG_WARN, "read(fd %d) short read: %d of %u bytes", fd, (int)result, (unsigned)n);
    }
    else
    {
        WLOG(RRC_LOG_DEBUG, "read(fd %d, %u bytes) -> OK", fd, (unsigned)n);
    }

    errno = err;
    return result;
}

/* ------------------------------------------------------------------------------------------------ */
/* Memory and process                                                                               */
/* ------------------------------------------------------------------------------------------------ */

void *rrc_dbgw_malloc(size_t n, RRC_DBGW_SITE)
{
    void *p = malloc(n);
    if (!p)
    {
        WLOG(RRC_LOG_ERROR, "malloc(%u) returned NULL - OUT OF MEMORY", (unsigned)n);
        rrc_log_memory_state("malloc failure");
    }
    else if (n >= 512 * 1024)
    {
        WLOG(RRC_LOG_DEBUG, "malloc(%u) -> %p (large allocation)", (unsigned)n, p);
    }
    return p;
}

void *rrc_dbgw_realloc(void *old, size_t n, RRC_DBGW_SITE)
{
    void *p = realloc(old, n);
    if (!p && n > 0)
    {
        WLOG(RRC_LOG_ERROR, "realloc(%p, %u) returned NULL - OUT OF MEMORY", old, (unsigned)n);
        rrc_log_memory_state("realloc failure");
    }
    return p;
}

void rrc_dbgw_exit(int code, RRC_DBGW_SITE)
{
    WLOG(code == 0 ? RRC_LOG_INFO : RRC_LOG_WARN, "exit(%d) called", code);
    exit(code);
}

/* ------------------------------------------------------------------------------------------------ */
/* curl                                                                                             */
/* ------------------------------------------------------------------------------------------------ */

/* Called by curl for its verbose output (connection steps, request and response headers). */
static int curl_verbose_callback(CURL *handle, curl_infotype type, char *data, size_t size, void *userp)
{
    const char *prefix;
    switch (type)
    {
    case CURLINFO_TEXT:
        prefix = "curl * ";
        break;
    case CURLINFO_HEADER_IN:
        prefix = "curl < ";
        break;
    case CURLINFO_HEADER_OUT:
        prefix = "curl > ";
        break;
    default:
        return 0; /* the body data itself is not logged */
    }

    while (size > 0 && (data[size - 1] == '\n' || data[size - 1] == '\r'))
        size--;
    if (size > 0)
        rrc_log_text_block(RRC_LOG_DEBUG, __FILE__, __LINE__, "curl", prefix, data, size, 1024);
    return 0;
}

CURLcode rrc_dbgw_curl_easy_perform(CURL *curl, RRC_DBGW_SITE)
{
    static char error_text[CURL_ERROR_SIZE];
    error_text[0] = 0;

    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_text);
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_verbose_callback);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    WLOG(RRC_LOG_INFO, "HTTP request starting (curl_easy_perform)");
    rrc_log_sync();

    u64 start = gettime();
    CURLcode result = curl_easy_perform(curl);
    u32 ms = diff_msec(start, gettime());

    long http_code = 0;
    char *url = NULL;
    char *ip = NULL;
    long os_errno = 0;
    long ssl_verify = 0;
    curl_off_t downloaded = 0;
    curl_off_t content_length = -1;
    curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &os_errno);
    curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &ssl_verify);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
    curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &ip);
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &downloaded);

    if (result == CURLE_OK)
    {
        WLOG(RRC_LOG_INFO, "HTTP %ld from %s (ip %s) - %lld bytes received, content-length %lld, %u ms",
             http_code, url ? url : "?", ip ? ip : "?", (long long)downloaded, (long long)content_length, (unsigned)ms);
        if (http_code >= 400)
        {
            WLOG(RRC_LOG_ERROR, "server answered HTTP %ld but curl reports success (CURLOPT_FAILONERROR is not set) - "
                                "the launcher will treat the error page as real data!",
                 http_code);
        }
    }
    else
    {
        WLOG(RRC_LOG_ERROR, "HTTP request FAILED: CURLcode %d (%s) - detail: \"%s\" - url %s, ip %s, HTTP %ld, os errno %ld, ssl verify %ld, %lld bytes, %u ms",
             (int)result, curl_easy_strerror(result), error_text, url ? url : "?", ip ? ip : "?", http_code,
             os_errno, ssl_verify, (long long)downloaded, (unsigned)ms);
    }

    rrc_log_sync();
    return result;
}
