/*
    main.c - entry point and key init routines

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

#include <stdio.h>
#include <stdlib.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <unistd.h>
#include <ogc/wiilaunch.h>
#include <string.h>
#include <fat.h>
#include <curl/curl.h>
#include <mxml.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <dirent.h>
#include <bitflags.h>

#include "loader/disc_loader.h"
#include "shutdown.h"
#include "util.h"
#include "di.h"
#include "time.h"
#include "loader/loader.h"
#include <dol.h>
#include <riivo.h>
#include "console.h"
#include "settings.h"
#include "update/versionsfile.h"
#include "update/update.h"
#include "prompt.h"
#include "gui.h"
#include "res.h"
#include "settingsfile.h"
#include "result.h"
#include "exception.h"
#include "sd.h"
#include "pad.h"
#include "crash.h"
#include "ephfile.h"
#include "debuglog.h"
#include "debugwrap.h"

/* 100ms */
#define DISKCHECK_DELAY 100000

// When debug is disabled, some variables become unused.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"

/* DEBUG BUILD: mxml parse errors (with line numbers) go to the log. */
static void rrc_mxml_error_cb(const char *msg)
{
    RRC_LOGE("mxml (XML parser): %s", msg);
}

int main(int argc, char **argv)
{
    // DEBUG BUILD: start the logger first (RAM only until the SD card is mounted).
    rrc_log_early_init(argc, argv);

    // There are bugs in pulsar with USB/HIDv5 if the IOS version is 59, which HBC commonly boots programs with.
    // Use a version used by the game that is known to work with pulsar.
    // FIXME: try to use the disk's IOS version?
    RRC_LOG_STEP("Reload IOS37");
    s32 ios_reload_res = IOS_ReloadIOS(37);
    RRC_LOGI("IOS_ReloadIOS(37) returned %d, now running IOS%d v%d", (int)ios_reload_res, (int)IOS_GetVersion(), (int)IOS_GetRevision());
    RRC_ASSERTEQ(ios_reload_res, 0, "Failed to reload IOS");

    rrc_shutdown_register_callbacks();

    // We reserve ~1MB of MEM1 upfront for the runtime-ext dol.
    u32 mem1_hi = RRC_RUNTIME_EXT_DOL_SAFE_START;
    u32 mem2_hi = *(u32 *)0x80003128;

    s64 systime_start = gettime();

    // response codes for various library functions
    int res;

    void *xfb;
    // init video, setup console framebuffer
    RRC_LOG_STEP("Initialise video");
    rrc_gui_xfb_alloc(&xfb, false);
    rrc_gui_display_con(xfb, true);
    int banner_res = rrc_gui_display_banner(xfb);
    rrc_log_set_console_ready(true);
    RRC_LOGI("video initialised, xfb=%p, banner draw result %d, console %dx%d", xfb, banner_res, rrc_con_get_cols(), rrc_con_get_rows());
    if (banner_res != 0)
        RRC_LOGW("drawing the banner failed (%d)", banner_res);

    init_exception_handlers();
    RRC_LOGI("exception handlers installed");

    // NOTE: We can't call any kind of printf before initialising libfat
    RRC_LOG_STEP("Initialise SD card");
    struct rrc_result sdinit_res = rrc_sd_init();
    if (rrc_result_is_error(sdinit_res))
    {
        // Try to open the log file anyway (it only works if sd:/ is usable)
        rrc_log_open_file();
    }
    rrc_result_error_check_error_fatal(sdinit_res);

    rrc_log_system_info();
    rrc_log_sd_overview();
    mxmlSetErrorCallback(rrc_mxml_error_cb);

    errno = 0;

    RRC_LOG_STEP("Initialise controllers");
    rrc_con_update("Initialise controllers", 0);
    res = PAD_Init();
    RRC_LOGI("PAD_Init() = %d", res);
    RRC_ASSERTEQ(res, 1, "PAD_Init");
    res = WPAD_Init();
    RRC_LOGI("WPAD_Init() = %d", res);
    RRC_ASSERTEQ(res, WPAD_ERR_NONE, "WPAD_Init");
    rrc_log_set_pads_ready(true);

    bool first_open = false;

    RRC_LOG_STEP("Check acceptance file");
    FILE *afd = fopen("sd:/" RRC_RETRO_REWIND_CHANNEL_DIR "/accept.txt", "r");
    if (afd == NULL)
    {
        RRC_LOGI("no accept.txt - first start of the channel, showing the welcome prompt");
        char *lines[] = {
            "Welcome to the new (beta) Retro Rewind Channel!",
            "",
            "You will now be taken to the settings menu.",
            "Please make sure your settings are the same",
            "as what you used with the old channel.",
            "",
            "",
            "By continuing, you acknowledge that:",
            "1. The channel is in a beta and therefore unfinished state,",
            "2. The channel comes with NO WARRANTY, express or implied,",
            "3. Taking a backup of your SD card is recommended."};

        enum rrc_prompt_result result = rrc_prompt_2_options(xfb, lines, 11, "I Accept", "Close Launcher", RRC_PROMPT_RESULT_OK, RRC_PROMPT_RESULT_CANCEL);
        RRC_LOGI("welcome prompt result: %d", result);
        if (result == RRC_PROMPT_RESULT_CANCEL)
        {
            RRC_LOGI("user chose 'Close Launcher'");
            exit(0);
        }

        if (result == RRC_PROMPT_RESULT_ERROR)
        {
            struct rrc_result err = rrc_result_create_error_errno(EIO, "Failed to display prompt");
            rrc_result_error_check_error_normal(err, xfb);
        }

        FILE *afd = fopen("sd:/" RRC_RETRO_REWIND_CHANNEL_DIR "/accept.txt", "w");
        if (afd == NULL)
        {
            struct rrc_result err = rrc_result_create_error_errno(errno, "Failed to create acceptance file. The SD card may be write locked.");
            rrc_result_error_check_error_normal(err, xfb);
        }
        else
        {
            fclose(afd);
        }

        first_open = true;
    }
    else
    {
        fclose(afd);
    }

    RRC_LOG_STEP("Load settings");
    rrc_con_update("Load settings", 5);
    struct rrc_settingsfile stored_settings;
    struct rrc_result settingsfile_res = rrc_settingsfile_parse(&stored_settings);
    RRC_LOGI("settings: my_stuff=%u separate_savegame=%u auto_update=%u (parse %s)",
             (unsigned)stored_settings.my_stuff, (unsigned)stored_settings.separate_savegame, (unsigned)stored_settings.auto_update,
             rrc_result_is_error(settingsfile_res) ? "FAILED" : "OK");
    if (rrc_result_is_error(settingsfile_res))
    {
        char *lines[] = {
            rrc_result_strerror(settingsfile_res),
            (char *)rrc_result_context(settingsfile_res),
            "It may be possible to fix this by recreating the file.",
            "Recreate now?",
        };

        enum rrc_prompt_result prompt_res = rrc_prompt_yes_no(xfb, lines, 4);
        RRC_LOGI("'recreate settings file?' prompt result: %d", prompt_res);
        settingsfile_res.err->reported = true;
        rrc_result_free(settingsfile_res);

        if (prompt_res == RRC_PROMPT_RESULT_YES)
        {
            settingsfile_res = rrc_settingsfile_create();
            if (rrc_result_is_error(settingsfile_res))
            {
                char *lines[] = {
                    "Failed to recreate settings file.",
                    rrc_result_strerror(settingsfile_res),
                    (char *)rrc_result_context(settingsfile_res),
                    "Defaults will be used with no changes on the SD card.",
                };
                settingsfile_res.err->reported = true;
                rrc_prompt_1_option(xfb, lines, 4, "OK");
            }
            rrc_result_free(settingsfile_res);
        }

        // `rrc_settingsfile_parse()` always initializes the settingsfile, so even in case of an error here,
        // it is initialized with defaults and we can continue with that.
    }

    bool crashed = false;
    if (rrc_launched_after_crash())
    {
        RRC_LOGW("the game crashed during the previous session (crash flag file found)");
        crashed = true;
        rrc_crash_handle(xfb, &stored_settings);
    }

    RRC_LOG_STEP("Initialise DVD");
    rrc_con_update("Initialise DVD", 10);
    int fd = rrc_di_init();
    RRC_LOGI("rrc_di_init() -> fd %d", fd);
    if (fd < 0)
        RRC_LOGE("opening /dev/di failed with %d (the original code only checks for 0)", fd);
    RRC_ASSERT(fd != 0, "rrc_di_init");

    RRC_LOG_STEP("Check for Mario Kart Wii");
    rrc_con_update("Initialise DVD: Check for Mario Kart Wii", 12);
    /*  We should load Mario Kart Wii before doing anything else */
    char region;
    res = rrc_disc_loader_await_mkw(xfb, &region);
    if (res == RRC_RES_SHUTDOWN_INTERRUPT)
    {
        RRC_LOGI("user cancelled the 'insert Mario Kart Wii' prompt");
        exit(0);
    }
    RRC_LOGI("Mario Kart Wii found, region '%c'", region);

    // We've identified the game. Now find the data partition, which will tell us where the DOL and FST is.
    // This first requires parsing the partition *groups*. Each partition group contains multiple partitions.
    // Data partitions have the id 0.

    RRC_LOG_STEP("Load data partition");
    rrc_con_update("Initialise DVD: Load Data Partition", 15);
    struct rrc_di_part_group part_groups[4] __attribute__((aligned(32)));
    res = rrc_di_unencrypted_read(&part_groups, sizeof(part_groups), RRC_DI_PART_GROUPS_OFFSET >> 2);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_unencrypted_read for partition group");
    for (int i = 0; i < 4; i++)
        RRC_LOGD("partition group %d: count=%u offset=0x%x", i, (unsigned)part_groups[i].count, (unsigned)(part_groups[i].offset << 2));

    u32 data_part_offset = UINT32_MAX;
    res = rrc_disc_loader_locate_data_part(&data_part_offset);

    RRC_LOGI("rrc_disc_loader_locate_data_part() = %d, data partition offset 0x%x", res, (unsigned)(data_part_offset << 2));
    if (data_part_offset == UINT32_MAX)
    {
        RRC_FATAL("no data partition found on disk");
    }
    rrc_dbg_printf("data partition found at offset %x\n", (unsigned)(data_part_offset << 2));

    rrc_con_update("Initialise DVD: Read Data Partition", 17);

    res = rrc_di_open_partition(data_part_offset);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_open_partition");

    struct rrc_di_data_part_header data_header[3] __attribute__((aligned(32)));
    res = rrc_di_read(&data_header, sizeof(data_header), 0x420 >> 2);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_read data partition header");

    rrc_dbg_printf("DOL offset: %d\n", (int)(data_header->dol_offset << 2));
    rrc_dbg_printf("FST offset: %d\n", (int)(data_header->fst_offset << 2));
    rrc_dbg_printf("FST size: %d\n", (int)(data_header->fst_size << 2));

    bool loaded_from_rr = rrc_launched_from_rr();
    RRC_LOGI("launched from RR (game returned to channel): %s, crashed: %s, auto update: %s",
             loaded_from_rr ? "yes" : "no", crashed ? "yes" : "no", stored_settings.auto_update ? "on" : "off");
    // Check for updates if the user enabled that setting. Skip if we loaded from the game since this will have already ran.
    if (stored_settings.auto_update && !loaded_from_rr && !crashed)
    {
        RRC_LOG_STEP("Automatic update check");

        int update_count;
        bool any_updates;
        struct rrc_result update_res = rrc_update_do_updates(xfb, &update_count, &any_updates);
        RRC_LOGI("automatic update finished: %s, %d updates available, installed: %s",
                 rrc_result_is_error(update_res) ? "ERROR" : "ok", update_count, any_updates ? "yes" : "no");
        if (rrc_result_is_error(update_res))
        {
            rrc_result_error_check_error_normal(update_res, xfb);
        }
        else if(any_updates)
        {
            char status_message[64];
            snprintf(status_message, sizeof(status_message), "%d updates were installed.", update_count);
            char *lines[] = {status_message, "", "The channel will now exit to apply the updates."};
            rrc_prompt_1_option(xfb, lines, 3, "OK");
            exit(0);
        }
    }
    else
    {
        RRC_LOGI("automatic update check skipped");
    }

    RRC_LOG_STEP("Waiting for user (A = launch, B = settings, HOME = exit)");
#define INTERRUPT_TIME 5000000 /* 5 seconds */
    rrc_con_clear(true);

    if (!first_open)
    {
        rrc_con_print_text_centered(_RRC_ACTION_ROW, "Press A to launch, or press B to load settings.");
        rrc_con_print_text_centered(_RRC_ACTION_ROW + 1, "Auto-launching in 5 seconds...");
    }

    for (int i = 0; i < INTERRUPT_TIME / RRC_WPAD_LOOP_TIMEOUT; i++)
    {
        rrc_shutdown_check();

        struct pad_state pad = rrc_pad_buttons();

        if (rrc_pad_home_pressed(pad))
        {
            RRC_LOGI("HOME pressed - exiting");
            return 0;
        }
        else if (rrc_pad_a_pressed(pad))
        {
            RRC_LOGI("A pressed - launching");
            break;
        }
        // Only allow auto launch if this is a successful first boot
        else if (rrc_pad_b_pressed(pad) || first_open || loaded_from_rr || crashed)
        {
            RRC_LOGI("opening settings (B pressed: %s, first_open: %d, loaded_from_rr: %d, crashed: %d)",
                     rrc_pad_b_pressed(pad) ? "yes" : "no", first_open, loaded_from_rr, crashed);
            struct rrc_result r;
            int out = rrc_settings_display(xfb, &stored_settings, &r);
            RRC_LOGI("settings menu returned %d", out);
            rrc_result_error_check_error_fatal(r);

            switch (out)
            {
            case RRC_SETTINGS_LAUNCH:
                goto interrupt_loop_end;
            case RRC_SETTINGS_EXIT:
                RRC_LOGI("user chose 'Exit Channel' in the settings");
                return 0;
            }
        }

        usleep(RRC_WPAD_LOOP_TIMEOUT);
    }
interrupt_loop_end:

    rrc_con_clear(true);

    RRC_LOG_STEP("Read game DOL from disc");
    rrc_log_memory_state("before reading the game");
    rrc_con_update("Initialise DVD: Read Game DOL", 25);

    // read dol
    struct rrc_dol *dol = (struct rrc_dol *)0x80901000;
    res = rrc_di_read(dol, sizeof(struct rrc_dol), data_header->dol_offset);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_read for dol");

    rrc_dbg_printf("Entrypoint at %x\n", (unsigned)dol->entry_point);
    rrc_dbg_printf("BSS Addr: %x\n", (unsigned)dol->bss_addr);
    rrc_dbg_printf("BSS size: %d\n", (int)dol->bss_size);
    for (u32 i = 0; i < RRC_DOL_SECTION_COUNT; i++)
    {
        if (dol->section_size[i] == 0)
        {
            continue;
        }
        rrc_dbg_printf("%x at %x-%x (%d b)\n", (unsigned)dol->section[i], (unsigned)dol->section_addr[i], (unsigned)(dol->section_addr[i] + dol->section_size[i]), (int)dol->section_size[i]);
        if ((dol->section_addr[i] < 0x80000000) || (dol->section_addr[i] + dol->section_size[i] > 0x90000000))
        {
            RRC_FATAL("Invalid section address: %x", dol->section_addr[i]);
        }

        // See patch.c comment for why we first copy them to `dol + dol->section[i]` rather than to `section_addr[i]` directly.
        res = rrc_di_read(
            (void *)((u32)dol + dol->section[i]),
            dol->section_size[i],
            data_header->dol_offset + (dol->section[i] >> 2));
        RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_read section");
    }

    RRC_LOG_STEP("Read filesystem table");
    rrc_con_update("Initialise DVD: Read Filesystem Table", 50);

    rrc_dbg_printf("mem1 hi: %x, mem2 hi %x\n", (unsigned)mem1_hi, (unsigned)mem2_hi);

    u32 fst_size = data_header->fst_size << 2;
    u32 fst_dest = align_down(mem1_hi - fst_size, 32);
    if (fst_dest < 0x81700000)
    {
        RRC_FATAL("fst size too large (size %u, would start at %x)", (unsigned)fst_size, (unsigned)fst_dest);
    }

    mem1_hi = fst_dest;
    rrc_dbg_printf("FST at %x, size: %d, aligned: %d\n", (unsigned)fst_dest, (int)fst_size, (int)align_up(fst_size, 32));
    res = rrc_di_read((void *)fst_dest, align_up(fst_size, 32), data_header->fst_offset);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_read fst");

    *((u32 *)0x80000038) = fst_dest; // start of FST

    // read BI2
    mem1_hi = align_down(mem1_hi - RRC_BI2_SIZE, 32);
    void *bi2 = (void *)(mem1_hi);
    res = rrc_di_read(bi2, RRC_BI2_SIZE, 0x440 >> 2);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_read for bi2");
    DCStoreRange(bi2, RRC_BI2_SIZE);
    RRC_LOGI("BI2 read to %p, mem1_hi now %x", bi2, (unsigned)mem1_hi);

    RRC_LOG_STEP("Prepare for patching");
    rrc_con_update("Prepare For Patching", 60);

    // start shutting down background threads to boot the game

    WPAD_Shutdown();
    rrc_log_set_pads_ready(false);
    RRC_LOGI("WPAD shut down");

    s64 systime_end = gettime();

    rrc_dbg_printf("time taken: %.3f seconds\n", ((f64)diff_msec(systime_start, systime_end)) / 1000.0);

    rrc_loader_load(dol, &stored_settings, bi2, mem1_hi, mem2_hi, region);

    RRC_LOGE("rrc_loader_load() returned - this should never happen");
    return 0;
}

#pragma GCC diagnostic pop