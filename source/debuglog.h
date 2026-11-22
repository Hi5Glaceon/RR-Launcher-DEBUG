/*
    debuglog.h - DEBUG BUILD: logging to the SD card

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
    Everything the launcher does is written to sd:/RR_debug.log. The file is recreated on every start.

    Until the SD card is mounted, lines are only kept in RAM. A ring buffer with the most recent
    lines is always kept, so the crash screens can show them.

    Line format:
        [  12.345] E update.c:69 rrc_update_get_current_version | message
         seconds   level  file:line  function                     text

    Levels: D = debug, I = info, W = warning, E = error, F = fatal
*/

#ifndef RRC_DEBUGLOG_H
#define RRC_DEBUGLOG_H

#include <stdbool.h>
#include <stddef.h>
#include <gctypes.h>

#define RRC_LOG_PATH "sd:/RR_debug.log"

/* Lines of level WARN and above are written to the card immediately. Other lines at least this often. */
#define RRC_LOG_SYNC_INTERVAL_MS 250

/* How long the fatal error screen stays up in this build. */
#define RRC_LOG_FATAL_SCREEN_SEC 60

enum rrc_log_level
{
    RRC_LOG_DEBUG = 0,
    RRC_LOG_INFO,
    RRC_LOG_WARN,
    RRC_LOG_ERROR,
    RRC_LOG_FATAL
};

/* Call first thing in main(). Only uses RAM. */
void rrc_log_early_init(int argc, char **argv);

/* Creates the log file once the SD card is mounted and writes everything that was buffered so far. */
void rrc_log_open_file(void);

/* Writes a summary of all warnings and errors and closes the file. */
void rrc_log_close(const char *reason);

/* Forces everything onto the card now. */
void rrc_log_sync(void);

/* Use the RRC_LOG* macros below instead of calling this directly. */
void rrc_log_write(enum rrc_log_level level, const char *file, int line, const char *func, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

/* Logs text (for example a downloaded file) line by line, each line starting with `prefix'.
   At most `max_bytes' of the text are logged. */
void rrc_log_text_block(enum rrc_log_level level, const char *file, int line, const char *func,
                        const char *prefix, const char *text, size_t len, size_t max_bytes);

/* Hex dump of up to 256 bytes. */
void rrc_log_hexdump(enum rrc_log_level level, const char *file, int line, const char *func,
                     const char *what, const void *data, size_t len);

/* "ENOENT", "EIO", ... for an errno value. Never NULL. */
const char *rrc_log_errno_name(int err);

/* Number of warnings / errors logged so far. */
int rrc_log_warn_count(void);
int rrc_log_error_count(void);

/* Short status of the log file for the screen, for example "sd:/RR_debug.log (OK)". */
const char *rrc_log_file_status(void);

/* Copies the most recent lines of at least `min_level' (oldest first) into `out', each cut to `width'
   characters. Returns the number of lines. */
int rrc_log_recent_lines(enum rrc_log_level min_level, char out[][128], int max, int width);

/* Checks what is wrong after a failed file operation (stat, parent directory, free space, raw SD read)
   and logs the result. Only the first few calls do the full check. */
void rrc_log_diagnose_path(const char *op, const char *path, int err, const char *file, int line, const char *func);

/* One line result of the last diagnosis, or NULL. */
const char *rrc_log_last_diagnosis(void);

/* Milliseconds since the last diagnosis (0xFFFFFFFF if there was none). */
u32 rrc_log_last_diagnosis_age_ms(void);

/* Logs information about the console, IOS, memory and libcurl. */
void rrc_log_system_info(void);

/* Logs the folders and files on the SD card that matter for Retro Rewind. */
void rrc_log_sd_overview(void);

/* Logs the current heap and arena sizes. */
void rrc_log_memory_state(const char *when);

/* Logs a CPU exception (registers and backtrace) and saves the log. `regs' holds r0..r31, then
   CR XER LR CTR SRR0 SRR1 DAR DSISR. */
void rrc_log_exception(int exception, const char *type, u32 *regs);

/* Writes the backtrace for stack pointer `sp' and link register `lr' to `out' as "80012345 80023456 ...". */
void rrc_log_format_backtrace(u32 sp, u32 lr, char *out, size_t out_sz);

/* Replacement for RRC_FATAL in this build: logs, shows the fatal screen, waits and exits. */
void rrc_log_fatal(const char *file, int line, const char *func, const char *fmt, ...)
    __attribute__((noreturn));

/* The fatal error screen: error, info, location, diagnosis and the last warnings. Waits, then exits. */
void rrc_log_fatal_screen_and_exit(const char *error_line, const char *info, const char *where, const char *diag)
    __attribute__((noreturn));

/* Tell the logger which parts of the UI work (needed for the fatal screen). */
void rrc_log_set_console_ready(bool ready);
void rrc_log_set_pads_ready(bool ready);

/* __FILE__ without the directories. */
const char *rrc_log_basename(const char *path);

#define RRC_LOG(level, ...) rrc_log_write((level), __FILE__, __LINE__, __func__, __VA_ARGS__)
#define RRC_LOGD(...) RRC_LOG(RRC_LOG_DEBUG, __VA_ARGS__)
#define RRC_LOGI(...) RRC_LOG(RRC_LOG_INFO, __VA_ARGS__)
#define RRC_LOGW(...) RRC_LOG(RRC_LOG_WARN, __VA_ARGS__)
#define RRC_LOGE(...) RRC_LOG(RRC_LOG_ERROR, __VA_ARGS__)

/* Marks a major step of the launch sequence and writes it to the card right away. */
#define RRC_LOG_STEP(...)                                                               \
    do                                                                                  \
    {                                                                                   \
        rrc_log_write(RRC_LOG_INFO, __FILE__, __LINE__, __func__, "==== " __VA_ARGS__); \
        rrc_log_sync();                                                                 \
    } while (0)

#define RRC_LOG_HEX(level, what, data, len) rrc_log_hexdump((level), __FILE__, __LINE__, __func__, (what), (data), (len))

/* For printf: RRC_LOGE("open failed: " RRC_LOG_ERRNO_FMT, RRC_LOG_ERRNO_ARGS(errno)); */
#define RRC_LOG_ERRNO_FMT "%s (%d: %s)"
#define RRC_LOG_ERRNO_ARGS(e) rrc_log_errno_name(e), (e), strerror(e)

#endif
