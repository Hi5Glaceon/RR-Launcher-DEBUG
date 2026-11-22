/*
    debuglog.c - DEBUG BUILD: logging to the SD card

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
    This file must NOT include "debugwrap.h": it uses the plain libc functions on purpose,
    otherwise the logger would log itself.
    It also must not include the project's "time.h" (it clashes with <ogc/lwp_watchdog.h>).
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <malloc.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <sdcard/wiisd_io.h>
#include <wiiuse/wpad.h>
#include <curl/curl.h>

#include <dir.h>
#include <version.h>
#include <riivo.h>

#define RRC_DEBUGWRAP_NO_MACROS /* only for the declaration of rrc_dbgw_log_open_files() */
#include "debugwrap.h"
#include "debuglog.h"
#include "console.h"
#include "shutdown.h"

/* ------------------------------------------------------------------------------------------------ */
/* State                                                                                            */
/* ------------------------------------------------------------------------------------------------ */

#define RING_LINES 160 /* log lines kept in RAM */
#define LINE_LEN 256   /* longest line kept in RAM (the file gets the full line) */
#define MSG_MAX 1024
#define SUMMARY_MAX 40 /* warnings/errors repeated in the summary */
#define MAX_DIAGNOSES 10

struct ring_entry
{
    u8 level;
    u16 msg_off;    /* where the message starts in `text' (after the timestamp and location) */
    char where[40]; /* "file.c:123" */
    char text[LINE_LEN];
};

static struct ring_entry ring[RING_LINES];
static u32 ring_total = 0;   /* how many lines were ever stored */
static u32 written_upto = 0; /* the lines up to this number are in the file */

static char summary[SUMMARY_MAX][LINE_LEN];
static int summary_count = 0;
static int summary_dropped = 0;

static FILE *log_fp = NULL;
static bool log_opened = false; /* the file was opened at some point */
static bool log_closed = false;
static int open_errno = 0;  /* why the file could not be created */
static int write_errno = 0; /* why writing to the file failed (the file is closed after that) */

static u64 start_ticks = 0;
static u64 last_sync_ticks = 0;
static int count_warn = 0;
static int count_error = 0;

static mutex_t log_mutex;
static bool mutex_ready = false;
static volatile bool in_exception = false; /* the mutex is not used in the exception handler */
static bool in_log = false;                /* guards against recursion */

static bool console_ready = false;
static bool pads_ready = false;

static int early_argc = 0;
static char early_argv0[128] = "";
static s32 early_ios_version = -1;
static s32 early_ios_revision = -1;

static char last_diagnosis[256] = "";
static u64 last_diagnosis_ticks = 0;
static char status_buf[96];

/* ------------------------------------------------------------------------------------------------ */
/* Small helpers                                                                                    */
/* ------------------------------------------------------------------------------------------------ */

const char *rrc_log_basename(const char *path)
{
    if (!path)
        return "?";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

const char *rrc_log_errno_name(int err)
{
    static const struct
    {
        int code;
        const char *name;
    } names[] = {
        {EPERM, "EPERM"},
        {ENOENT, "ENOENT"},
        {EINTR, "EINTR"},
        {EIO, "EIO"},
        {EBADF, "EBADF"},
        {ENOMEM, "ENOMEM"},
        {EACCES, "EACCES"},
        {EEXIST, "EEXIST"},
        {ENOTDIR, "ENOTDIR"},
        {EISDIR, "EISDIR"},
        {EINVAL, "EINVAL"},
        {EMFILE, "EMFILE"},
        {ENOSPC, "ENOSPC"},
        {EROFS, "EROFS"},
        {ENAMETOOLONG, "ENAMETOOLONG"},
        {ENOTEMPTY, "ENOTEMPTY"},
        {ETIMEDOUT, "ETIMEDOUT"},
        {ENOTSUP, "ENOTSUP"},
    };

    if (err == 0)
        return "OK(0)";
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        if (names[i].code == err)
            return names[i].name;
    }
    return "E?";
}

static u32 now_ms(void)
{
    return start_ticks == 0 ? 0 : diff_msec(start_ticks, gettime());
}

static char level_char(int level)
{
    static const char chars[] = "DIWEF";
    return (level >= 0 && level <= RRC_LOG_FATAL) ? chars[level] : '?';
}

static void lock(void)
{
    if (mutex_ready && !in_exception)
        LWP_MutexLock(log_mutex);
}

static void unlock(void)
{
    if (mutex_ready && !in_exception)
        LWP_MutexUnlock(log_mutex);
}

/* Removes the colour codes of the console (ESC [ ... m). */
static void strip_ansi(char *s)
{
    char *r = s;
    char *w = s;
    while (*r)
    {
        if (*r == 0x1b && r[1] == '[')
        {
            r += 2;
            while (*r && !isalpha((unsigned char)*r))
                r++;
            if (*r)
                r++;
        }
        else
        {
            *w++ = *r++;
        }
    }
    *w = 0;
}

/* One log call must be one line in the file. */
static void make_single_line(char *s)
{
    for (; *s; s++)
    {
        if (*s == '\n')
            *s = '|';
        else if (*s == '\r')
            *s = '~';
        else if ((unsigned char)*s < 0x20 && *s != '\t')
            *s = '?';
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Ring buffer and file                                                                             */
/* ------------------------------------------------------------------------------------------------ */

static void ring_store(int level, const char *where, const char *line, int msg_off)
{
    struct ring_entry *e = &ring[ring_total % RING_LINES];
    ring_total++;

    e->level = (u8)level;
    e->msg_off = (u16)(msg_off < LINE_LEN ? msg_off : 0);
    snprintf(e->where, sizeof(e->where), "%s", where);
    snprintf(e->text, sizeof(e->text), "%s", line);

    if (level < RRC_LOG_WARN)
        return;

    if (level == RRC_LOG_WARN)
        count_warn++;
    else
        count_error++;

    if (summary_count < SUMMARY_MAX)
        snprintf(summary[summary_count++], LINE_LEN, "%s", line);
    else
        summary_dropped++;
}

/* Messages from the logger itself. They only go to the ring buffer, never directly to the file. */
static void note(int level, const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char line[LINE_LEN];
    u32 ms = now_ms();
    int off = snprintf(line, sizeof(line), "[%5u.%03u] %c debuglog | ", (unsigned)(ms / 1000), (unsigned)(ms % 1000), level_char(level));
    snprintf(line + off, sizeof(line) - off, "%s", msg);
    ring_store(level, "debuglog", line, off);
}

/* Writing failed: close the file. Later lines stay in RAM only. */
static void file_failed(int err)
{
    write_errno = err ? err : EIO;
    fclose(log_fp);
    log_fp = NULL;
    note(RRC_LOG_ERROR, "writing %s failed: " RRC_LOG_ERRNO_FMT " - no more output to the file", RRC_LOG_PATH,
         RRC_LOG_ERRNO_ARGS(write_errno));
}

/* Writes all lines that are not in the file yet. `newest_full' is the untruncated text of the
   newest line (the ring buffer only keeps LINE_LEN characters), or NULL. */
static void write_pending(const char *newest_full)
{
    if (!log_fp)
        return;

    u32 oldest = ring_total > RING_LINES ? ring_total - RING_LINES + 1 : 1;
    if (written_upto + 1 < oldest)
    {
        fprintf(log_fp, "[.........] W debuglog | %u lines were lost (overwritten in RAM before the file was open)\n",
                (unsigned)(oldest - written_upto - 1));
        written_upto = oldest - 1;
    }

    for (u32 n = written_upto + 1; n <= ring_total; n++)
    {
        const char *text = ring[(n - 1) % RING_LINES].text;
        if (newest_full && n == ring_total)
            text = newest_full;

        errno = 0;
        if (fputs(text, log_fp) < 0 || fputc('\n', log_fp) == EOF)
        {
            file_failed(errno);
            return;
        }
        written_upto = n;
    }

    errno = 0;
    if (fflush(log_fp) != 0)
        file_failed(errno);
}

/* Makes sure the data is really on the card and not only in libfat's cache. */
static void write_summary(const char *reason);

/* Writes a summary after the last line and puts the file position back in front of it. The next
   log lines overwrite it, and a fresh summary follows them. This way the file always ends with an
   up-to-date summary, even if the program crashes (an exception handler cannot write to the SD card). */
static void write_rolling_summary(void)
{
    long pos = ftell(log_fp);
    if (pos < 0)
        return;

    write_summary("state at the last write - if the file ends here, the program did not exit normally (crash or power off)");
    fflush(log_fp);
    ftruncate(fileno(log_fp), ftell(log_fp)); /* drop leftovers of an older, longer summary */
    fseek(log_fp, pos, SEEK_SET);
}

static void sync_file(void)
{
    if (log_fp)
    {
        write_rolling_summary();
        fsync(fileno(log_fp));
    }
    last_sync_ticks = gettime();
}

/* ------------------------------------------------------------------------------------------------ */
/* Logging                                                                                          */
/* ------------------------------------------------------------------------------------------------ */

void rrc_log_write(enum rrc_log_level level, const char *file, int line, const char *func, const char *fmt, ...)
{
    int saved_errno = errno;

    lock();
    if (in_log && !in_exception)
    {
        unlock();
        errno = saved_errno;
        return;
    }
    in_log = true;

    char msg[MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    strip_ansi(msg);
    for (size_t len = strlen(msg); len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r'); len--)
        msg[len - 1] = 0;
    make_single_line(msg);

    char where[40];
    snprintf(where, sizeof(where), "%s:%d", rrc_log_basename(file), line);

    u32 ms = now_ms();
    char full[MSG_MAX + 128];
    int off = snprintf(full, sizeof(full), "[%5u.%03u] %c %s %s | ", (unsigned)(ms / 1000), (unsigned)(ms % 1000),
                       level_char(level), where, func ? func : "?");
    if (off < 0 || off >= (int)sizeof(full))
        off = 0;
    snprintf(full + off, sizeof(full) - off, "%s", msg);

    ring_store(level, where, full, off);

    /* In an exception handler interrupts are off and the SD card cannot be used. */
    if (log_fp && !in_exception)
    {
        write_pending(full);
        if (log_fp && (level >= RRC_LOG_WARN || diff_msec(last_sync_ticks, gettime()) >= RRC_LOG_SYNC_INTERVAL_MS))
            sync_file();
    }

    in_log = false;
    unlock();
    errno = saved_errno;
}

void rrc_log_sync(void)
{
    int saved_errno = errno;

    lock();
    if (log_fp)
    {
        write_pending(NULL);
        sync_file();
    }
    unlock();

    errno = saved_errno;
}

void rrc_log_text_block(enum rrc_log_level level, const char *file, int line, const char *func,
                        const char *prefix, const char *text, size_t len, size_t max_bytes)
{
    if (!text)
    {
        rrc_log_write(level, file, line, func, "%s<NULL>", prefix);
        return;
    }

    size_t limit = len < max_bytes ? len : max_bytes;
    size_t start = 0;
    int line_no = 1;

    while (start < limit)
    {
        size_t end = start;
        while (end < limit && text[end] != '\n')
            end++;

        char buf[200];
        size_t n = end - start;
        if (n > sizeof(buf) - 1)
            n = sizeof(buf) - 1;
        for (size_t i = 0; i < n; i++)
        {
            unsigned char c = (unsigned char)text[start + i];
            buf[i] = (c == '\r') ? '~' : ((c < 0x20 || c >= 0x7f) ? '.' : (char)c);
        }
        buf[n] = 0;

        rrc_log_write(level, file, line, func, "%s%3d: %s", prefix, line_no++, buf);
        start = end + 1;
    }

    if (len > max_bytes)
        rrc_log_write(level, file, line, func, "%s... (%u more bytes not shown)", prefix, (unsigned)(len - max_bytes));
}

void rrc_log_hexdump(enum rrc_log_level level, const char *file, int line, const char *func,
                     const char *what, const void *data, size_t len)
{
    if (!data)
    {
        rrc_log_write(level, file, line, func, "%s: <NULL>", what);
        return;
    }

    const u8 *bytes = data;
    size_t shown = len > 256 ? 256 : len;
    rrc_log_write(level, file, line, func, "%s: %u bytes%s", what, (unsigned)len, len > shown ? " (first 256 shown)" : "");

    for (size_t off = 0; off < shown; off += 16)
    {
        char hex[16 * 3 + 1] = "";
        char ascii[17] = "";
        size_t n = shown - off < 16 ? shown - off : 16;
        for (size_t i = 0; i < n; i++)
        {
            sprintf(hex + i * 3, "%02x ", bytes[off + i]);
            ascii[i] = (bytes[off + i] >= 0x20 && bytes[off + i] < 0x7f) ? (char)bytes[off + i] : '.';
        }
        ascii[n] = 0;
        rrc_log_write(level, file, line, func, "  %04x: %-48s %s", (unsigned)off, hex, ascii);
    }
}

int rrc_log_warn_count(void)
{
    return count_warn;
}

int rrc_log_error_count(void)
{
    return count_error;
}

const char *rrc_log_file_status(void)
{
    if (!log_opened)
    {
        if (open_errno)
            snprintf(status_buf, sizeof(status_buf), "NOT WRITTEN (%s: %s)", rrc_log_errno_name(open_errno), strerror(open_errno));
        else
            snprintf(status_buf, sizeof(status_buf), "NOT WRITTEN (no SD card)");
    }
    else if (write_errno)
        snprintf(status_buf, sizeof(status_buf), "%s (WRITE ERROR: %s)", RRC_LOG_PATH, rrc_log_errno_name(write_errno));
    else
        snprintf(status_buf, sizeof(status_buf), "%s (OK)", RRC_LOG_PATH);

    return status_buf;
}

int rrc_log_recent_lines(enum rrc_log_level min_level, char out[][128], int max, int width)
{
    if (width > 127)
        width = 127;
    if (width < 8)
        width = 8;
    if (max > 32)
        max = 32;

    /* Go from the newest line backwards, then fill `out' so that the oldest line comes first. */
    int found_at[32];
    int found = 0;
    u32 count = ring_total < RING_LINES ? ring_total : RING_LINES;
    for (u32 i = 0; i < count && found < max; i++)
    {
        int idx = (ring_total - 1 - i) % RING_LINES;
        if (ring[idx].level >= min_level)
            found_at[found++] = idx;
    }

    for (int i = 0; i < found; i++)
    {
        const struct ring_entry *e = &ring[found_at[found - 1 - i]];
        snprintf(out[i], width + 1, "%c %s %s", level_char(e->level), e->where, e->text + e->msg_off);
    }
    return found;
}

void rrc_log_set_console_ready(bool ready)
{
    console_ready = ready;
}

void rrc_log_set_pads_ready(bool ready)
{
    pads_ready = ready;
}

/* ------------------------------------------------------------------------------------------------ */
/* Start and end                                                                                    */
/* ------------------------------------------------------------------------------------------------ */

static void atexit_handler(void)
{
    if (!log_closed)
    {
        RRC_LOGI("process is exiting (exit() called or main() returned)");
        rrc_log_close("process exit");
    }
}

void rrc_log_early_init(int argc, char **argv)
{
    start_ticks = gettime();
    last_sync_ticks = start_ticks;

    if (LWP_MutexInit(&log_mutex, false) == 0)
        mutex_ready = true;

    early_argc = argc;
    if (argc > 0 && argv && argv[0])
        snprintf(early_argv0, sizeof(early_argv0), "%s", argv[0]);
    early_ios_version = IOS_GetVersion();
    early_ios_revision = IOS_GetRevision();

    atexit(atexit_handler);

    RRC_LOGI("debug log started (RAM only until the SD card is mounted)");
    RRC_LOGI("IOS at startup (before reload): IOS%d v%d", (int)early_ios_version, (int)early_ios_revision);
    RRC_LOGI("argc=%d argv[0]=\"%s\"", early_argc, early_argv0);
}

static void write_header(void)
{
    char clock_str[32] = "unknown";
    time_t now = time(NULL);
    struct tm tm_now;
    if (gmtime_r(&now, &tm_now))
        strftime(clock_str, sizeof(clock_str), "%Y-%m-%d %H:%M:%S", &tm_now);

    struct rrc_version version = RRC_INTERNAL_VERSION;

    fprintf(log_fp,
            "==============================================================================\n"
            " Retro Rewind Launcher - DEBUG BUILD log\n"
            " Channel %d.%d.%d, built %s %s\n"
            " Console clock: %s\n"
            " Log file: %s (recreated on every start)\n"
            " Line format: [seconds since start] LEVEL file:line function | message\n"
            " Levels: D=debug I=info W=warning E=error F=fatal\n"
            "==============================================================================\n",
            version.major, version.minor, version.patch, __DATE__, __TIME__, clock_str, RRC_LOG_PATH);
}

void rrc_log_open_file(void)
{
    lock();
    if (log_fp || log_closed)
    {
        unlock();
        return;
    }

    struct stat st;
    if (stat("sd:/", &st) != 0)
    {
        note(RRC_LOG_WARN, "sd:/ is not mounted, cannot create the log file");
    }
    else
    {
        errno = 0;
        log_fp = fopen(RRC_LOG_PATH, "w");
        if (!log_fp)
        {
            open_errno = errno ? errno : EIO;
            note(RRC_LOG_ERROR, "could not create %s: " RRC_LOG_ERRNO_FMT, RRC_LOG_PATH, RRC_LOG_ERRNO_ARGS(open_errno));
        }
    }

    if (log_fp)
    {
        log_opened = true;
        written_upto = 0;
        write_header();
        write_pending(NULL);
        sync_file();
    }
    unlock();

    if (log_fp)
        RRC_LOGI("log file opened: %s", RRC_LOG_PATH);
}

static void write_summary(const char *reason)
{
    u32 ms = now_ms();
    fprintf(log_fp,
            "\n==============================================================================\n"
            " SUMMARY (%s)\n"
            " Runtime: %u.%03u s, lines: %u, warnings: %d, errors: %d\n",
            reason, (unsigned)(ms / 1000), (unsigned)(ms % 1000), (unsigned)ring_total, count_warn, count_error);

    if (summary_count == 0)
    {
        fprintf(log_fp, " No warnings or errors were logged.\n");
    }
    else
    {
        fprintf(log_fp, " All warnings/errors in order:\n");
        for (int i = 0; i < summary_count; i++)
            fprintf(log_fp, "   %s\n", summary[i]);
        if (summary_dropped)
            fprintf(log_fp, "   ... and %d more (see above)\n", summary_dropped);
    }
    fprintf(log_fp, "==============================================================================\n");
}

void rrc_log_close(const char *reason)
{
    RRC_LOGI("closing log: %s", reason);

    lock();
    if (log_fp)
    {
        write_pending(NULL);
        if (log_fp)
        {
            write_summary(reason);
            fflush(log_fp);
            ftruncate(fileno(log_fp), ftell(log_fp));
            fsync(fileno(log_fp));
            fclose(log_fp);
            log_fp = NULL;
        }
    }
    log_closed = true;
    unlock();
}

/* ------------------------------------------------------------------------------------------------ */
/* Directory listings                                                                               */
/* ------------------------------------------------------------------------------------------------ */

/* Logs the entries of a directory (at most `max'). */
static void log_listing(const char *dir, int max)
{
    DIR *d = opendir(dir);
    if (!d)
    {
        RRC_LOGW("cannot list %s: " RRC_LOG_ERRNO_FMT, dir, RRC_LOG_ERRNO_ARGS(errno));
        return;
    }

    RRC_LOGI("listing of %s:", dir);
    int shown = 0;
    int total = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        total++;
        if (shown >= max)
            continue;
        shown++;

        char path[300];
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        struct stat st;
        if (stat(path, &st) != 0)
            RRC_LOGW("    [????] %-40s stat failed: " RRC_LOG_ERRNO_FMT, entry->d_name, RRC_LOG_ERRNO_ARGS(errno));
        else if (S_ISDIR(st.st_mode))
            RRC_LOGD("    [dir ] %s/", entry->d_name);
        else
            RRC_LOGD("    [file] %-40s %10lld bytes", entry->d_name, (long long)st.st_size);
    }
    closedir(d);

    RRC_LOGD("    (%d entries%s)", total, total > shown ? ", the rest is not listed" : "");
}

/* Is there an entry called `name' in `dir'? Returns 1 for an exact match, 2 if only the upper/lower
   case differs, 0 if there is none, -1 if the directory cannot be read. */
static int dir_has_entry(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    if (!d)
        return -1;

    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL)
    {
        if (strcmp(entry->d_name, name) == 0)
        {
            result = 1;
            break;
        }
        if (strcasecmp(entry->d_name, name) == 0)
            result = 2;
    }
    closedir(d);
    return result;
}

/* ------------------------------------------------------------------------------------------------ */
/* Diagnosis of failed file operations                                                              */
/* ------------------------------------------------------------------------------------------------ */

void rrc_log_memory_state(const char *when)
{
    struct mallinfo info = mallinfo();
    RRC_LOGI("memory (%s): heap arena=%u used=%u free=%u | MEM1 arena %p-%p | MEM2 arena %p-%p",
             when, (unsigned)info.arena, (unsigned)info.uordblks, (unsigned)info.fordblks,
             SYS_GetArena1Lo(), SYS_GetArena1Hi(), SYS_GetArena2Lo(), SYS_GetArena2Hi());
}

/* Reads sector 0 straight from the SD card, without the file system. Tells if the card itself works. */
static const char *raw_sd_check(void)
{
    static u8 sector[512] __attribute__((aligned(32)));

    if (!__io_wiisd.isInserted())
        return "SD not inserted";
    if (!__io_wiisd.readSectors(0, 1, sector))
        return "raw SD read FAILED";
    return "raw SD read OK";
}

void rrc_log_diagnose_path(const char *op, const char *path, int err, const char *file, int line, const char *func)
{
    static int diagnoses = 0;
    int saved_errno = errno;

    if (++diagnoses > MAX_DIAGNOSES)
    {
        if (diagnoses == MAX_DIAGNOSES + 1)
            rrc_log_write(RRC_LOG_WARN, file, line, func, "more than %d failed file operations - no more diagnoses", MAX_DIAGNOSES);
        errno = saved_errno;
        return;
    }

    if (!path)
        path = "<NULL>";
    rrc_log_write(RRC_LOG_WARN, file, line, func, "---- DIAGNOSIS #%d for failed %s(\"%s\"): " RRC_LOG_ERRNO_FMT,
                  diagnoses, op, path, RRC_LOG_ERRNO_ARGS(err));
    if (err == 0)
        rrc_log_write(RRC_LOG_ERROR, file, line, func, "  the call failed without setting errno, so an error code shown for it is meaningless");

    /* Does the path exist? */
    char path_state[48];
    struct stat st;
    if (stat(path, &st) == 0)
        snprintf(path_state, sizeof(path_state), "stat OK (%lld B)", (long long)st.st_size);
    else
        snprintf(path_state, sizeof(path_state), "stat %s", rrc_log_errno_name(errno));

    /* Is the file listed in its folder? */
    char folder[256];
    snprintf(folder, sizeof(folder), "%s", path);
    char *slash = strrchr(folder, '/');
    const char *name = path;
    if (slash)
    {
        name = slash + 1;
        if (slash == folder || slash[-1] == ':')
            slash[1] = 0; /* keep the "/" of the root folder */
        else
            *slash = 0;
    }
    else
    {
        snprintf(folder, sizeof(folder), ".");
    }

    const char *listing_state;
    switch (dir_has_entry(folder, name))
    {
    case 1:
        listing_state = "file IS in dir listing";
        break;
    case 2:
        listing_state = "different upper/lower case in dir listing";
        break;
    case 0:
        listing_state = "file NOT in dir listing";
        break;
    default:
        listing_state = "folder cannot be read";
        break;
    }

    /* Is the problem permanent? Try the same stat() again a few times. */
    char retry_state[64] = "";
    if (err != 0)
    {
        int successes = 0;
        for (int attempt = 1; attempt <= 3; attempt++)
        {
            usleep(100 * 1000);
            struct stat retry_st;
            errno = 0;
            if (stat(path, &retry_st) == 0)
            {
                successes++;
                rrc_log_write(RRC_LOG_INFO, file, line, func, "  retry %d/3: stat OK", attempt);
            }
            else
            {
                rrc_log_write(RRC_LOG_INFO, file, line, func, "  retry %d/3: stat failed again: " RRC_LOG_ERRNO_FMT, attempt,
                              RRC_LOG_ERRNO_ARGS(errno));
            }
        }
        snprintf(retry_state, sizeof(retry_state), "retries %d/3 OK%s", successes, successes ? " (transient!)" : "");
    }

    /* Free space on the card. */
    struct statvfs vfs;
    if (statvfs("sd:/", &vfs) == 0)
        rrc_log_write(RRC_LOG_INFO, file, line, func, "  sd:/ has %llu MB free, cluster size %lu bytes",
                      (unsigned long long)vfs.f_bfree * vfs.f_frsize / (1024 * 1024), (unsigned long)vfs.f_bsize);
    else
        rrc_log_write(RRC_LOG_WARN, file, line, func, "  statvfs(sd:/) failed: " RRC_LOG_ERRNO_FMT " - the file system may be gone", RRC_LOG_ERRNO_ARGS(errno));

    const char *raw_state = raw_sd_check();
    rrc_dbgw_log_open_files();
    rrc_log_memory_state("at failure");

    snprintf(last_diagnosis, sizeof(last_diagnosis), "%s(%s): %s; %s; %s%s%s", op, rrc_log_errno_name(err), path_state,
             listing_state, retry_state, retry_state[0] ? "; " : "", raw_state);
    last_diagnosis_ticks = gettime();
    rrc_log_write(RRC_LOG_WARN, file, line, func, "---- DIAGNOSIS #%d result: %s", diagnoses, last_diagnosis);

    rrc_log_sync();
    errno = saved_errno;
}

const char *rrc_log_last_diagnosis(void)
{
    return last_diagnosis[0] ? last_diagnosis : NULL;
}

u32 rrc_log_last_diagnosis_age_ms(void)
{
    return last_diagnosis[0] ? diff_msec(last_diagnosis_ticks, gettime()) : 0xFFFFFFFF;
}

/* ------------------------------------------------------------------------------------------------ */
/* System and SD card overview                                                                      */
/* ------------------------------------------------------------------------------------------------ */

void rrc_log_system_info(void)
{
    RRC_LOG_STEP("SYSTEM INFORMATION");

    RRC_LOGI("IOS now: IOS%d v%d (at startup: IOS%d v%d)", (int)IOS_GetVersion(), (int)IOS_GetRevision(),
             (int)early_ios_version, (int)early_ios_revision);
    RRC_LOGI("Hollywood revision: 0x%08x", (unsigned)SYS_GetHollywoodRevision());

    s32 dolphin_fd = IOS_Open("/dev/dolphin", 0);
    if (dolphin_fd >= 0)
        IOS_Close(dolphin_fd);
    RRC_LOGI("emulator: %s", dolphin_fd >= 0 ? "Dolphin (/dev/dolphin exists)" : "no Dolphin detected (real console)");

    RRC_LOGI("launched from: argc=%d argv[0]=\"%s\"", early_argc, early_argv0);

    GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);
    if (mode)
        RRC_LOGI("video: preferred mode viTVMode=0x%x fb=%dx%d xfbHeight=%d", (unsigned)mode->viTVMode, mode->fbWidth, mode->efbHeight, mode->xfbHeight);
    RRC_LOGI("CONF: video=%d progressive=%d eurgb60=%d aspect=%d language=%d region=%d area=%d",
             (int)CONF_GetVideo(), (int)CONF_GetProgressiveScan(), (int)CONF_GetEuRGB60(), (int)CONF_GetAspectRatio(),
             (int)CONF_GetLanguage(), (int)CONF_GetRegion(), (int)CONF_GetArea());

    RRC_LOGI("low memory: game id @80000000 = %08x %08x, IOS @80003140 = %08x",
             (unsigned)*(vu32 *)0x80000000, (unsigned)*(vu32 *)0x80000004, (unsigned)*(vu32 *)0x80003140);
    rrc_log_memory_state("startup");

    curl_version_info_data *curl = curl_version_info(CURLVERSION_NOW);
    if (curl)
        RRC_LOGI("libcurl %s, SSL: %s", curl->version ? curl->version : "?", curl->ssl_version ? curl->ssl_version : "none");

    rrc_log_sync();
}

static void log_key_file(const char *path)
{
    struct stat st;
    errno = 0;
    if (stat(path, &st) == 0)
        RRC_LOGI("  [%s] %-55s %10lld bytes", S_ISDIR(st.st_mode) ? "dir " : "file", path, (long long)st.st_size);
    else
        RRC_LOG(errno == ENOENT ? RRC_LOG_INFO : RRC_LOG_WARN, "  [----] %-55s " RRC_LOG_ERRNO_FMT, path, RRC_LOG_ERRNO_ARGS(errno));
}

void rrc_log_sd_overview(void)
{
    RRC_LOG_STEP("SD CARD OVERVIEW");

    struct statvfs vfs;
    if (statvfs("sd:/", &vfs) == 0)
    {
        unsigned long long free_bytes = (unsigned long long)vfs.f_bfree * vfs.f_frsize;
        unsigned long long total_bytes = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
        RRC_LOGI("sd:/ size %llu MB, free %llu MB, cluster size %lu bytes",
                 total_bytes / (1024 * 1024), free_bytes / (1024 * 1024), (unsigned long)vfs.f_bsize);
        if (free_bytes < 50ULL * 1024 * 1024)
            RRC_LOGW("less than 50 MB free on the SD card");
    }
    else
    {
        RRC_LOGW("statvfs(sd:/) failed: " RRC_LOG_ERRNO_FMT, RRC_LOG_ERRNO_ARGS(errno));
    }

    log_listing("sd:/", 40);
    log_listing("/" RRC_RETRO_REWIND_BASE_DIR, 60);
    log_listing("/" RRC_RETRO_REWIND_CHANNEL_DIR, 40);
    log_listing("/" RRC_RETRO_REWIND_BASE_DIR "/Binaries", 20);

    static const char *const key_files[] = {
        "/" RRC_RETRO_REWIND_BASE_DIR "/version.txt",
        RRC_RIIVO_XML_PATH,
        "/" RRC_RETRO_REWIND_BASE_DIR "/Binaries/Loader.pul",
        "/" RRC_RETRO_REWIND_BASE_DIR "/Binaries/Code.pul",
        "/" RRC_RETRO_REWIND_BASE_DIR "/.settings",
        "/" RRC_RETRO_REWIND_BASE_DIR "/Crash.pul",
        "/" RRC_RETRO_REWIND_CHANNEL_DIR "/accept.txt",
        "/" RRC_RETRO_REWIND_CHANNEL_DIR "/runtime-ext-P.dol",
        "/" RRC_RETRO_REWIND_CHANNEL_DIR "/runtime-ext-E.dol",
        "/" RRC_RETRO_REWIND_CHANNEL_DIR "/runtime-ext-J.dol",
        "/" RRC_RETRO_REWIND_CHANNEL_DIR "/runtime-ext-K.dol",
        "/" RRC_RETRO_REWIND_CHANNEL_DIR "/.crash",
        "/" RRC_RETRO_REWIND_CHANNEL_DIR "/.lfrr",
        "/apps/RetroRewind/boot.dol",
    };
    RRC_LOGI("key files:");
    for (size_t i = 0; i < sizeof(key_files) / sizeof(key_files[0]); i++)
        log_key_file(key_files[i]);

    /* The exact bytes of version.txt (a stray newline or a missing byte breaks the version check). */
    const char *version_path = "/" RRC_RETRO_REWIND_BASE_DIR "/version.txt";
    FILE *version_file = fopen(version_path, "rb");
    if (version_file)
    {
        char buf[64];
        size_t n = fread(buf, 1, sizeof(buf), version_file);
        fclose(version_file);
        RRC_LOG_HEX(RRC_LOG_INFO, "version.txt raw bytes", buf, n);
    }
    else
    {
        RRC_LOGE("cannot open %s: " RRC_LOG_ERRNO_FMT, version_path, RRC_LOG_ERRNO_ARGS(errno));
    }

    rrc_log_sync();
}

/* ------------------------------------------------------------------------------------------------ */
/* CPU exceptions                                                                                   */
/* ------------------------------------------------------------------------------------------------ */

/* MEM1 and MEM2 */
static bool valid_addr(u32 addr)
{
    return (addr >= 0x80000000 && addr < 0x81800000) || (addr >= 0x90000000 && addr < 0x94000000);
}

void rrc_log_format_backtrace(u32 sp, u32 lr, char *out, size_t out_sz)
{
    size_t pos = 0;
    out[0] = 0;
    if (valid_addr(lr))
        pos += snprintf(out + pos, out_sz - pos, "%08x", (unsigned)lr);

    /* Every stack frame starts with a pointer to the previous frame. The return address of the
       function that owns a frame is saved at offset 4 of the frame above it. */
    u32 frame = sp;
    for (int depth = 0; depth < 16 && valid_addr(frame) && (frame & 3) == 0 && pos < out_sz - 10; depth++)
    {
        u32 caller_frame = *(u32 *)frame;
        if (!valid_addr(caller_frame) || (caller_frame & 3) != 0 || caller_frame <= frame)
            break;
        u32 return_addr = *(u32 *)(caller_frame + 4);
        if (valid_addr(return_addr))
            pos += snprintf(out + pos, out_sz - pos, "%s%08x", pos ? " " : "", (unsigned)return_addr);
        frame = caller_frame;
    }
}

void rrc_log_exception(int exception, const char *type, u32 *regs)
{
    in_exception = true;

    u32 *gpr = regs;     /* r0..r31 */
    u32 *spr = regs + 32; /* CR XER LR CTR SRR0 SRR1 DAR DSISR */

    RRC_LOG(RRC_LOG_FATAL, "CPU EXCEPTION 0x%x (%s)", (unsigned)exception, type);
    RRC_LOG(RRC_LOG_FATAL, "  PC(SRR0)=%08x LR=%08x DAR=%08x DSISR=%08x SRR1=%08x CR=%08x CTR=%08x",
            (unsigned)spr[4], (unsigned)spr[2], (unsigned)spr[6], (unsigned)spr[7], (unsigned)spr[5], (unsigned)spr[0], (unsigned)spr[3]);
    for (int i = 0; i < 32; i += 8)
    {
        RRC_LOG(RRC_LOG_FATAL, "  r%-2d-r%-2d: %08x %08x %08x %08x %08x %08x %08x %08x", i, i + 7,
                (unsigned)gpr[i], (unsigned)gpr[i + 1], (unsigned)gpr[i + 2], (unsigned)gpr[i + 3],
                (unsigned)gpr[i + 4], (unsigned)gpr[i + 5], (unsigned)gpr[i + 6], (unsigned)gpr[i + 7]);
    }

    char backtrace[200];
    rrc_log_format_backtrace(gpr[1], spr[2], backtrace, sizeof(backtrace));
    RRC_LOG(RRC_LOG_FATAL, "  backtrace (newest first): %08x %s", (unsigned)spr[4], backtrace);
    RRC_LOG(RRC_LOG_FATAL, "  resolve with: powerpc-eabi-addr2line -f -C -e RR-Launcher-https.elf <addresses>");

    switch (exception)
    {
    case 0x300:
        RRC_LOG(RRC_LOG_FATAL, "  DSI: invalid memory access at DAR (a NULL pointer if DAR is small)");
        break;
    case 0x400:
        RRC_LOG(RRC_LOG_FATAL, "  ISI: jump to an invalid code address (broken function pointer or stack)");
        break;
    case 0x600:
        RRC_LOG(RRC_LOG_FATAL, "  Alignment: misaligned access at DAR");
        break;
    case 0x700:
        RRC_LOG(RRC_LOG_FATAL, "  Program: illegal instruction or trap");
        break;
    }

    /* The SD card is NOT written here. Its driver needs interrupts, and enabling them inside an exception
       handler triggers a second exception (libogc then shows "Exception (Interrupt) occurred!"). Without
       interrupts the write hangs. Everything above is shown on the exception screen instead. */
}

/* ------------------------------------------------------------------------------------------------ */
/* Fatal error screen                                                                               */
/* ------------------------------------------------------------------------------------------------ */

/* Waits `seconds`, or until the user presses A, START or HOME. */
static void wait_before_exit(int seconds)
{
    for (int i = 0; i < seconds * 50; i++)
    {
        rrc_shutdown_check();
        if (pads_ready)
        {
            WPAD_ScanPads();
            PAD_ScanPads();
            u32 wii_buttons = WPAD_ButtonsDown(0);
            u32 gc_buttons = PAD_ButtonsDown(0);
            if ((wii_buttons & (WPAD_BUTTON_A | WPAD_CLASSIC_BUTTON_A | WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)) ||
                (gc_buttons & (PAD_BUTTON_A | PAD_BUTTON_START)))
            {
                RRC_LOGI("user pressed a button on the fatal screen - exiting now");
                return;
            }
        }
        usleep(20 * 1000);
    }
}

void rrc_log_fatal_screen_and_exit(const char *error_line, const char *info, const char *where, const char *diag)
{
    /* Get the recent problems first, before anything else is logged. */
    char recent[5][128];
    int columns = console_ready ? rrc_con_get_cols() : 60;
    int recent_count = rrc_log_recent_lines(RRC_LOG_WARN, recent, 4, columns - 3);

    RRC_LOG(RRC_LOG_FATAL, "FATAL ERROR SCREEN: error=\"%s\" info=\"%s\" where=%s", error_line ? error_line : "",
            info ? info : "", where ? where : "?");
    if (diag)
        RRC_LOG(RRC_LOG_FATAL, "diagnosis: %s", diag);
    rrc_log_memory_state("fatal");
    rrc_log_sync();

    if (console_ready)
    {
        rrc_con_clear(false);
        rrc_con_print_text_centered(0, RRC_CON_ANSI_FG_BRIGHT_RED "A fatal error has occurred! [DEBUG]");
        rrc_con_cursor_seek_to(2, 0);
        printf(RRC_CON_ANSI_FG_BRIGHT_RED "Error: " RRC_CON_ANSI_FG_WHITE "%s\n", error_line ? error_line : "?");
        if (info && info[0])
            printf(RRC_CON_ANSI_FG_BRIGHT_CYAN "Info: " RRC_CON_ANSI_FG_WHITE "%s\n", info);
        if (where && where[0])
            printf(RRC_CON_ANSI_FG_BRIGHT_YELLOW "Where: " RRC_CON_ANSI_FG_WHITE "%s\n", where);
        if (diag && diag[0])
            printf(RRC_CON_ANSI_FG_BRIGHT_MAGENTA "Check: " RRC_CON_ANSI_FG_WHITE "%s\n", diag);

        printf("\n" RRC_CON_ANSI_FG_BRIGHT_GREEN "Log: " RRC_CON_ANSI_FG_WHITE "%s\n", rrc_log_file_status());
        if (recent_count > 0)
        {
            printf(RRC_CON_ANSI_FG_BRIGHT_WHITE "Recent warnings/errors:\n" RRC_CON_ANSI_FG_WHITE);
            for (int i = 0; i < recent_count; i++)
                printf(" %s\n", recent[i]);
        }

        printf("\nThe launcher will exit in %d seconds%s", RRC_LOG_FATAL_SCREEN_SEC, pads_ready ? " (A = exit now)." : ".");
        fflush(stdout);
    }

    wait_before_exit(RRC_LOG_FATAL_SCREEN_SEC);
    rrc_log_close("fatal error");
    exit(1);
}

void rrc_log_fatal(const char *file, int line, const char *func, const char *fmt, ...)
{
    char msg[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    rrc_log_write(RRC_LOG_FATAL, file, line, func, "RRC_FATAL/ASSERT: %s", msg);

    char where[96];
    snprintf(where, sizeof(where), "%s:%d %s()", rrc_log_basename(file), line, func);
    const char *diag = rrc_log_last_diagnosis_age_ms() < 3000 ? rrc_log_last_diagnosis() : NULL;
    rrc_log_fatal_screen_and_exit(msg, "Internal check failed (RRC_FATAL / RRC_ASSERT).", where, diag);
}
