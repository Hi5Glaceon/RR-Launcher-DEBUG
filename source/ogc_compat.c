/*
    ogc_compat.c - link compatibility with libogc >= 3.x

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
    libogc 3.x turned gettime() and diff_msec() into `static inline` functions in <ogc/lwp_watchdog.h>,
    so libogc.a no longer exports them as symbols. The launcher (source/time.h declares them as normal
    functions) and the prebuilt wii-mbedtls package (entropy_poll.o, platform_util.o) still call the
    exported versions, which fails to link with "undefined reference to `gettime'".

    This file provides real (exported) functions that simply forward to libogc's inline implementations.
    The inline names are renamed while including the header so that they don't clash with the
    exported definitions below.
*/

#define gettime _rrc_ogc_inline_gettime
#define diff_msec _rrc_ogc_inline_diff_msec
#include <ogc/lwp_watchdog.h>
#undef gettime
#undef diff_msec

u64 gettime(void)
{
    return _rrc_ogc_inline_gettime();
}

u32 diff_msec(u64 start, u64 end)
{
    return _rrc_ogc_inline_diff_msec(start, end);
}
