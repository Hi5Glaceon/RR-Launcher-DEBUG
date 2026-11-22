/*
    util.h - utility constants and macro definitions

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

#ifndef RRC_UTIL_H
#define RRC_UTIL_H

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <gctypes.h>

#include "console.h"
#include "debuglog.h"

/*
    When this directive is defined and set to a value greater than 0,
    debug logging and some additional assertions are enabled.
*/
#if defined(RRC_BETA) && RRC_BETA >= 1
// beta implies debug
#define RRC_DEBUG 1
#elif !defined(RRC_DEBUG)
#define RRC_DEBUG 0
#endif

#define _RRC_STRING(s) #s

// Whenever we have a loop waiting for wiipad button inputs, this timeout is used (in microseconds).
#define RRC_WPAD_LOOP_TIMEOUT 20000

#ifndef RRC_EXIT_DELAY
#define RRC_EXIT_DELAY 3000000
#endif

#define RRC_LWP_PRIO_IDLE 0
#define RRC_LWP_OK 0

/* DEBUG BUILD: logs the message with its source location, shows a detailed fatal screen
   (including recent log lines) for RRC_LOG_FATAL_SCREEN_SEC seconds and exits. */
#define RRC_FATAL(...)                                                \
    do                                                                \
    {                                                                 \
        rrc_log_fatal(__FILE__, __LINE__, __func__, __VA_ARGS__);     \
    } while (0);

#define RRC_ASSERT(condition, what)                                        \
    do                                                                     \
    {                                                                      \
        int st = condition;                                                \
        char *st_str = _RRC_STRING(condition);                             \
        if (!st)                                                           \
        {                                                                  \
            RRC_FATAL("%s: assert failed: %s (got %i)", what, st_str, st); \
        }                                                                  \
    } while (0);

/* DEBUG BUILD: lhs/rhs are evaluated exactly once. (The original macro evaluated `lhs' a second time
   for the error message, i.e. e.g. IOS_ReloadIOS() would have been called again on failure and the
   printed value was the result of that second call.) */
#define RRC_ASSERTEQ(lhs, rhs, what)                                                                    \
    do                                                                                                  \
    {                                                                                                   \
        int _rrc_lhs = (lhs);                                                                           \
        int _rrc_rhs = (rhs);                                                                           \
        char *st_str = _RRC_STRING(lhs) " == " _RRC_STRING(rhs);                                        \
        if (_rrc_lhs != _rrc_rhs)                                                                       \
        {                                                                                               \
            RRC_FATAL("%s: assert failed: %s (lhs = %i, rhs = %i)", what, st_str, _rrc_lhs, _rrc_rhs); \
        }                                                                                               \
    } while (0);

#define RRC_STRINGIFY(x) #x

#if defined(RRC_DEBUG) && RRC_DEBUG >= 1
/* define debug macros */

#define rrc_dbg_printf(...)                                        \
    do                                                             \
    {                                                              \
        RRC_LOGD(__VA_ARGS__);                                     \
        rrc_con_clear_line(_RRC_PRINTF_ROW);                       \
        rrc_con_clear_line(_RRC_PRINTF_ROW + 1);                   \
        rrc_con_cursor_seek_to(_RRC_PRINTF_ROW, RRC_CON_EDGE_PAD); \
        printf(__VA_ARGS__);                                       \
    } while (0);

#else
/* DEBUG BUILD: the existing debug prints always go to the log file (not to the screen). */

#define rrc_dbg_printf(...)  \
    do                       \
    {                        \
        RRC_LOGD(__VA_ARGS__); \
    } while (0);

#endif

/**
 * Invalidates any data caches for a given address range.
 * This handles the pointer alignment and size-multiple requirements and does not need to be handled by callers.
 */
void rrc_invalidate_cache(void *addr, u32 size);

u32 align_down(u32 num, u32 align_as);
u32 align_up(u32 num, u32 align_as);

#endif
