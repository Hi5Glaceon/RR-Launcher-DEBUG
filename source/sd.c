/*
    sd.c - SD card helper and initialisation routines

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

#include <fat.h>
#include "unistd.h"

#include "sd.h"
#include <sys/dirent.h>
#include "update/update.h"
#include <sdcard/wiisd_io.h>
#include "debuglog.h"
#include "debugwrap.h"

struct rrc_result rrc_sd_init()
{
    /* Manually check if the SD card interface is available */
    RRC_LOGI("SD: __io_wiisd.startup()");
    if(!__io_wiisd.startup()) {
        RRC_LOGE("SD: __io_wiisd.startup() failed - SD slot driver could not start (card missing / IOS problem)");
        return rrc_result_create_error_sdcard(EIO, "Couldn't start SD card interface - is it inserted?");
    }

    if(!__io_wiisd.isInserted()) {
        RRC_LOGE("SD: __io_wiisd.isInserted() = false");
        return rrc_result_create_error_sdcard(EIO, "No SD card is inserted.");
    }
    RRC_LOGI("SD: interface started, card inserted");

    /* __io_wiisd caches state so the fat driver won't reinit */
    bool fat_ok = fatInitDefault();
    RRC_LOGI("SD: fatInitDefault() = %s", fat_ok ? "true" : "false");

    // DEBUG BUILD: open the log file as early as possible.
    rrc_log_open_file();

    if (!fat_ok)
    {
        RRC_LOGE("SD: libfat could not mount the card (not FAT16/FAT32? exFAT/NTFS are not supported, or damaged file system)");
        return rrc_result_create_error_sdcard(EIO, "Couldn't mount the SD card. Is it inserted and properly formatted?");
    }

    if (chdir("sd:/") == -1)
    {
        return rrc_result_create_error_sdcard(errno ? errno : EIO, "Failed to set SD card root");
    }

    if (!rrc_sd_folder_exists("/" RRC_RETRO_REWIND_CHANNEL_DIR))
    {
        RRC_LOGE("SD: folder /" RRC_RETRO_REWIND_CHANNEL_DIR " missing");
        return rrc_result_create_error_sdcard(errno ? errno : EIO, "sd:/" RRC_RETRO_REWIND_CHANNEL_DIR " was not found.");
    }

    RRC_LOGI("SD: write test with " RRC_SD_TEST_FILE);
    FILE *file = fopen(RRC_SD_TEST_FILE, "w+");
    if (!file)
    {
        RRC_LOGE("SD: write test - could not create the test file (lock switch? read-only file system?)");
        return rrc_result_create_error_sdcard(errno ? errno : EIO, "The SD card is write locked.");
    }

    // Test writing to the SD card, then clean up the test file.
    int res = fwrite("1.2.3", 1, 5, file);

    if (res <= 0) {
        RRC_LOGE("SD: write test - fwrite returned %d", res);
        fclose(file);
        return rrc_result_create_error_sdcard(EIO, "The SD card is write locked.");
    }

    fflush(file);
    rewind(file);
    
    // Read the file and parse the version to check the write worked
    char buf[6] = {0};
    if (fread(buf, 1, 5, file) <= 0)
    {   
        RRC_LOGE("SD: write test - reading back the test file failed");
        fclose(file);
        return rrc_result_create_error_sdcard(EIO, "The SD card is write locked.");
    }

    struct rrc_version ver;
    struct rrc_result verres = rrc_version_from_string(buf, &ver);
    if (rrc_result_is_error(verres))
    {
        RRC_LOGE("SD: write test - read back \"%s\" instead of \"1.2.3\"", buf);
        rrc_result_free(verres);
        fclose(file);
        return rrc_result_create_error_sdcard(EIO, "The SD card is write locked.");
    }

    fclose(file);
    unlink(RRC_SD_TEST_FILE);

    RRC_LOGI("SD: initialised and writable");
    return rrc_result_success;
}

bool rrc_sd_file_exists(const char *path)
{
    FILE *file = fopen(path, "r");
    if (file != NULL)
    {
        fclose(file);
        return true;
    }
    else
    {
        return false;
    }
}

bool rrc_sd_folder_exists(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
    {
        return true;
    }
    else
    {
        return false;
    }
}

int rrc_sd_get_folder_file_count(const char *path, struct rrc_result *out_err)
{
    RRC_LOGD("counting files in %s", path);
    DIR *dir = opendir(path);
    if (dir == NULL)
    {
        *out_err = rrc_result_create_error_errno(errno, "Failed to open directory");
        return -1;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        count++;
    }

    closedir(dir);
    return count;
}

struct rrc_result rrc_sd_get_free_space(unsigned long long *res)
{
    struct statvfs sbx;
    int rr = statvfs("sd:", &sbx);
    if (rr != 0)
    {
        return rrc_result_create_error_errno(errno, "Failed to get free space on SD card");
    }

    *res = sbx.f_bfree * sbx.f_frsize;
    return rrc_result_success;
}