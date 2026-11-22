/*
    di.c - /dev/di wrapper implementation

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

/* /dev/di is documented at https://wiibrew.org/wiki//dev/di */

/*
    This file exists because some aspects of libdi seem to be implemented incorrectly.
    Speficially, `init' opens the device file with the wrong mode (2 instead of 0), and
    attempting to get cover register status uses the wrong command number. We provide
    our own implementations where necessary, but all manipulation of /dev/di should
    regardless happen through this file due to the fact that it all shares the same
    global di_fd variable.
*/
#include <di/di.h>

#include "string.h"
#include "di.h"
#include "util.h"
#include "debuglog.h"

/* DEBUG BUILD: logs the DI error code after a failed DI call. */
static void log_di_failure(const char *what, int status, u32 offset, u32 size)
{
    uint32_t dierr = 0;
    int r = DI_GetError(&dierr);
    RRC_LOGE("%s FAILED: status %d, offset 0x%llx, size %u - DI_GetError()=%d, drive error 0x%08x",
             what, status, (unsigned long long)offset << 2, (unsigned)size, r, (unsigned)dierr);
    rrc_log_sync();
}

int rrc_di_getfd()
{
    // defined in di/di.h
    return di_fd;
}

int rrc_di_init()
{
    int fd = (int)IOS_Open("/dev/di", 0);
    RRC_LOGI("IOS_Open(\"/dev/di\", 0) = %d", fd);

    di_fd = fd;
    return fd;
}

int rrc_di_reset()
{
    int r = DI_Reset();
    RRC_LOGI("DI_Reset() = %d", r);
    return r;
}

int rrc_di_get_low_cover_register(unsigned int *status)
{
    uint32_t outbuf[8] __attribute__((aligned(32))) = {0};
    uint32_t dic[8] __attribute__((aligned(32)));
    memset(dic, 0x00, 0x20);
    dic[0] = 0x7A << 24;

    // get drive status (disc inserted?)
    int res = IOS_Ioctl(rrc_di_getfd(), 0x7A, dic, 0x20, outbuf, 0x20);
    *status = outbuf[0];

    return res;
}

int rrc_di_get_disk_id(struct rrc_di_disk_id *diskid)
{
    uint64_t idr;
    int resdid = DI_ReadDiscID(&idr);
    RRC_LOGD("DI_ReadDiscID() = %d", resdid);

    if (resdid == RRC_DI_LIBDI_OK)
    {
        memcpy(diskid, &idr, sizeof(struct rrc_di_disk_id));
    }
    else
    {
        // zero it on error
        memset(diskid, 0, sizeof(struct rrc_di_disk_id));
    }

    return resdid;
}

int rrc_di_unencrypted_read(void *buf, u32 size, u32 offset)
{
    if (size < 32)
    {
        RRC_FATAL("UnencryptedRead() requires a size >= 32, got %d", size);
    }

    if (((u32)buf & 31) != 0)
    {
        RRC_FATAL("UnencryptedRead() buffer must be aligned to 32 bytes, but is at address %p", buf);
    }

    int status = DI_UnencryptedRead(buf, size, offset);
    if (status != RRC_DI_LIBDI_OK)
    {
        log_di_failure("DI_UnencryptedRead", status, offset, size);
        memset(buf, 0, size);
    }
    else
    {
        RRC_LOGD("DI_UnencryptedRead(offset 0x%llx, %u bytes) OK", (unsigned long long)offset << 2, (unsigned)size);
    }
    return status;
}

int rrc_di_read(void *buf, u32 size, u32 offset)
{
    int status = DI_Read(buf, size, offset);
    if (status != RRC_DI_LIBDI_OK)
    {
        log_di_failure("DI_Read", status, offset, size);
        memset(buf, 0, size);
    }
    else
    {
        RRC_LOGD("DI_Read(offset 0x%llx, %u bytes -> %p) OK", (unsigned long long)offset << 2, (unsigned)size, buf);
    }
    return status;
}

int rrc_di_open_partition(u32 offset)
{
    int r = DI_OpenPartition(offset);
    if (r != RRC_DI_LIBDI_OK)
        log_di_failure("DI_OpenPartition", r, offset, 0);
    else
        RRC_LOGI("DI_OpenPartition(0x%llx) OK", (unsigned long long)offset << 2);
    return r;
}
