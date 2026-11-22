/*
    result.c - Error and success handling implementation

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

#include <stdlib.h>
#include <unistd.h>
#include <gccore.h>
#include <zip.h>

#include "shutdown.h"
#include "prompt.h"
#include "result.h"
#include "time.h"
#include "console.h"
#include "debuglog.h"
#include "debugwrap.h"

const struct rrc_result rrc_result_success = {.err = NULL};

/* Used if allocating an error fails, so that the error is still reported. */
static struct
{
    struct rrc_result_error err;
    char context[160];
} fallback_error;

static const char *source_name(enum rrc_result_error_source s)
{
    switch (s)
    {
    case ESOURCE_CURL:
        return "CURL";
    case ESOURCE_ERRNO:
        return "ERRNO";
    case ESOURCE_ZIP:
        return "ZIP";
    case ESOURCE_CORRUPTED_SETTINGSFILE:
        return "CORRUPTED_SETTINGSFILE";
    case ESOURCE_UPDATE_MISC:
        return "UPDATE_MISC";
    case ESOURCE_CORRUPTED_VERSIONFILE:
        return "CORRUPTED_VERSIONFILE";
    case ESOURCE_SD_CARD:
        return "SD_CARD";
    case ESOURCE_WIISOCKET_INIT:
        return "WIISOCKET_INIT";
    case ESOURCE_CORRUPTED_RR_XML:
        return "CORRUPTED_RR_XML";
    case ESOURCE_VERSION_MISMATCH:
        return "VERSION_MISMATCH";
    default:
        return "UNKNOWN";
    }
}

void rrc_result_describe_code(struct rrc_result result, char *out, int out_sz)
{
    if (!rrc_result_is_error(result))
    {
        snprintf(out, out_sz, "no error");
        return;
    }

    struct rrc_result_error *e = result.err;
    switch (e->source)
    {
    case ESOURCE_ERRNO:
    case ESOURCE_SD_CARD:
        snprintf(out, out_sz, "%s %s (%d: %s)", source_name(e->source), rrc_log_errno_name(e->inner.errnocode),
                 e->inner.errnocode, strerror(e->inner.errnocode));
        break;
    case ESOURCE_CURL:
        snprintf(out, out_sz, "CURLcode %d (%s)", (int)e->inner.ccode, curl_easy_strerror(e->inner.ccode));
        break;
    case ESOURCE_ZIP:
    {
        zip_error_t ze;
        zip_error_init_with_code(&ze, e->inner.ziperr);
        snprintf(out, out_sz, "ZIP error %d (%s)", e->inner.ziperr, zip_error_strerror(&ze));
        zip_error_fini(&ze);
        break;
    }
    case ESOURCE_WIISOCKET_INIT:
        snprintf(out, out_sz, "wiisocket_init code %d", e->inner.wiisocket_init_code);
        break;
    default:
        snprintf(out, out_sz, "%s", source_name(e->source));
        break;
    }
}

static struct rrc_result_error *alloc_error(enum rrc_result_error_source esource, union rrc_result_error_inner einner,
                                            const char *context, RRC_RESULT_SITE_PARAMS)
{
    if (!context)
        context = "(no context given)";

    /* If a file operation was just diagnosed, attach the verdict to this error. */
    const char *diag = rrc_log_last_diagnosis_age_ms() < 3000 ? rrc_log_last_diagnosis() : NULL;

    int contextlen = strlen(context);
    int diaglen = diag ? strlen(diag) : 0;
    struct rrc_result_error *err = malloc(sizeof(struct rrc_result_error) + contextlen + 1 + (diag ? diaglen + 1 : 0));
    if (!err)
    {
        rrc_log_write(RRC_LOG_ERROR, site_file, site_line, site_func, "could not allocate error structure - using fallback");
        err = &fallback_error.err;
        contextlen = sizeof(fallback_error.context) - 1;
        diag = NULL;
    }
    err->source = esource;
    err->inner = einner;
    err->site_file = site_file;
    err->site_line = site_line;
    err->site_func = site_func;
    err->reported = false;
    strncpy(err->context, context, contextlen);
    err->context[contextlen] = '\0';
    if (diag)
    {
        char *d = err->context + contextlen + 1;
        memcpy(d, diag, diaglen + 1);
        err->diag = d;
    }
    else
    {
        err->diag = NULL;
    }

    struct rrc_result tmp = {.err = err};
    char code[160];
    rrc_result_describe_code(tmp, code, sizeof(code));
    rrc_log_write(RRC_LOG_WARN, site_file, site_line, site_func, "error result created: [%s] %s | context: %s", source_name(esource), code, context);
    return err;
}

struct rrc_result rrc_result_create_error_curl_at(CURLcode error, const char *context, RRC_RESULT_SITE_PARAMS)
{
    union rrc_result_error_inner einner = {.ccode = error};
    struct rrc_result res = {.err = alloc_error(ESOURCE_CURL, einner, context, site_file, site_line, site_func)};
    return res;
}

struct rrc_result rrc_result_create_error_errno_at(int eno, const char *context, RRC_RESULT_SITE_PARAMS)
{
    if (eno == 0)
        rrc_log_write(RRC_LOG_WARN, site_file, site_line, site_func,
                      "errno is 0 here - the failing call did not set errno, the message shown will say 'Success'");
    rrc_dbgw_diagnose_pending(eno);
    union rrc_result_error_inner einner = {.errnocode = eno};
    struct rrc_result res = {.err = alloc_error(ESOURCE_ERRNO, einner, context, site_file, site_line, site_func)};
    return res;
}

struct rrc_result rrc_result_create_error_sdcard_at(int eno, const char *context, RRC_RESULT_SITE_PARAMS)
{
    union rrc_result_error_inner einner = {.errnocode = eno};
    struct rrc_result res = {.err = alloc_error(ESOURCE_SD_CARD, einner, context, site_file, site_line, site_func)};
    return res;
}

struct rrc_result rrc_result_create_error_zip_at(int error, const char *context, RRC_RESULT_SITE_PARAMS)
{
    union rrc_result_error_inner einner = {.ziperr = error};
    struct rrc_result res = {.err = alloc_error(ESOURCE_ZIP, einner, context, site_file, site_line, site_func)};
    return res;
}

struct rrc_result rrc_result_create_error_corrupted_settingsfile_at(const char *context, RRC_RESULT_SITE_PARAMS)
{
    union rrc_result_error_inner einner = {0};
    struct rrc_result res = {.err = alloc_error(ESOURCE_CORRUPTED_SETTINGSFILE, einner, context, site_file, site_line, site_func)};
    return res;
}

struct rrc_result rrc_result_create_error_corrupted_versionfile_at(const char *context, RRC_RESULT_SITE_PARAMS)
{
    union rrc_result_error_inner einner = {0};
    struct rrc_result res = {.err = alloc_error(ESOURCE_CORRUPTED_VERSIONFILE, einner, context, site_file, site_line, site_func)};
    return res;
}

struct rrc_result rrc_result_create_error_misc_update_at(const char *context, RRC_RESULT_SITE_PARAMS)
{
    union rrc_result_error_inner einner = {0};
    struct rrc_result res = {.err = alloc_error(ESOURCE_UPDATE_MISC, einner, context, site_file, site_line, site_func)};
    return res;
}

struct rrc_result rrc_result_create_error_corrupted_rr_xml_at(const char *context, RRC_RESULT_SITE_PARAMS)
{
    union rrc_result_error_inner einner = {0};
    struct rrc_result res = {.err = alloc_error(ESOURCE_CORRUPTED_RR_XML, einner, context, site_file, site_line, site_func)};
    return res;
}

struct rrc_result rrc_result_create_error_version_mismatch_at(const char *context, RRC_RESULT_SITE_PARAMS)
{

    union rrc_result_error_inner einner = {0};
    struct rrc_result res = {.err = alloc_error(ESOURCE_VERSION_MISMATCH, einner, context, site_file, site_line, site_func)};
    return res;
}

char *rrc_result_strerror(struct rrc_result result)
{
    if (!rrc_result_is_error(result))
    {
        return NULL;
    }

    switch (result.err->source)
    {
    case ESOURCE_CURL:
        return (char *)curl_easy_strerror(result.err->inner.ccode);
    case ESOURCE_ERRNO:
        return strerror(result.err->inner.errnocode);
    case ESOURCE_ZIP:
        return "ZIP file error.";
    case ESOURCE_CORRUPTED_SETTINGSFILE:
        return "Corrupted settings file detected.";
    case ESOURCE_CORRUPTED_VERSIONFILE:
        return "Corrupted version file detected.";
    case ESOURCE_UPDATE_MISC:
        return "Update failed.";
    case ESOURCE_SD_CARD:
        return "SD card error.";
    case ESOURCE_WIISOCKET_INIT:
    {
        switch (result.err->inner.wiisocket_init_code)
        {
        case -1:
            return "Network initialisation already in progress.";
        case -2:
            return "Failed to initialise library.";
        case -3:
            return "Failed to initialise network.";
        case -4:
            return "Failed to register hardware in devoptab.";
        default:
            return "Unknown error code.";
        }
    }
    case ESOURCE_CORRUPTED_RR_XML:
        return "Invalid or corrupted RetroRewind6.xml.";
    case ESOURCE_VERSION_MISMATCH:
        return "Version mismatch.";
    default:
        return NULL;
    }
}

/* Splits `text' at newlines and wraps it to `width' columns. Returns the number of lines produced. */
static int wrap_text(const char *text, int width, char out[][128], int max_lines)
{
    if (width > 127)
        width = 127;
    int n = 0;
    const char *p = text;
    while (*p && n < max_lines)
    {
        int len = 0;
        while (p[len] && p[len] != '\n' && len < width)
            len++;

        /* try not to cut words */
        if (p[len] && p[len] != '\n' && len == width)
        {
            int back = len;
            while (back > width / 2 && p[back] != ' ')
                back--;
            if (p[back] == ' ')
                len = back;
        }

        memcpy(out[n], p, len);
        out[n][len] = 0;
        n++;
        p += len;
        if (*p == '\n' || *p == ' ')
            p++;
    }
    return n;
}

void rrc_result_error_check_error_normal(struct rrc_result result, void *xfb)
{
    if (!rrc_result_is_error(result))
    {
        return;
    }

    struct rrc_result_error *e = result.err;
    e->reported = true;

    char code[160];
    rrc_result_describe_code(result, code, sizeof(code));
    rrc_log_write(RRC_LOG_ERROR, e->site_file, e->site_line, e->site_func,
                  "ERROR SHOWN TO USER (dismissable): %s | %s | context: %s", rrc_result_strerror(result), code, e->context);
    rrc_log_sync();

    int rows, cols;
    CON_GetMetrics(&cols, &rows);
    int width = cols - 2;
    if (width > 100)
        width = 100;
    if (width < 20)
        width = 20;

    /* The prompt shows at most 12 lines and silently shows nothing if a line is longer than the console width.
       In this build all lines are wrapped/truncated so that the error is always visible. */
    char storage[12][128];
    char *lines[12];
    int n = 0;

    snprintf(storage[n], width + 1, "Error: %s", rrc_result_strerror(result) ? rrc_result_strerror(result) : "?");
    lines[n] = storage[n];
    n++;

    snprintf(storage[n], width + 1, "%s @ %s:%d", code, rrc_log_basename(e->site_file), e->site_line);
    lines[n] = storage[n];
    n++;

    lines[n++] = "";
    lines[n++] = "Additional info:";

    int max_ctx = e->diag ? 4 : 6;
    char ctx[6][128];
    int nctx = wrap_text(e->context, width, ctx, max_ctx);
    for (int i = 0; i < nctx; i++)
    {
        snprintf(storage[n], sizeof(storage[n]), "%s", ctx[i]);
        lines[n] = storage[n];
        n++;
    }

    if (e->diag)
    {
        char dl[2][128];
        char tmp[300];
        snprintf(tmp, sizeof(tmp), "Check: %s", e->diag);
        int nd = wrap_text(tmp, width, dl, 2);
        for (int i = 0; i < nd; i++)
        {
            snprintf(storage[n], sizeof(storage[n]), "%s", dl[i]);
            lines[n] = storage[n];
            n++;
        }
    }

    if (n < 12)
    {
        snprintf(storage[n], width + 1, "Log: %s", rrc_log_file_status());
        lines[n] = storage[n];
        n++;
    }

    rrc_prompt_1_option(xfb, lines, n, "OK");
    rrc_log_write(RRC_LOG_INFO, __FILE__, __LINE__, __func__, "user dismissed the error prompt");
    rrc_result_free(result);
}

void rrc_result_error_check_error_fatal(struct rrc_result result)
{
    if (!rrc_result_is_error(result))
    {
        return;
    }

    struct rrc_result_error *e = result.err;
    e->reported = true;

    char code[160];
    rrc_result_describe_code(result, code, sizeof(code));

    char error_line[200];
    snprintf(error_line, sizeof(error_line), "%s [%s]", rrc_result_strerror(result) ? rrc_result_strerror(result) : "?", code);

    char where[128];
    snprintf(where, sizeof(where), "%s:%d %s()", rrc_log_basename(e->site_file), e->site_line, e->site_func ? e->site_func : "?");

    rrc_log_write(RRC_LOG_FATAL, e->site_file, e->site_line, e->site_func, "FATAL ERROR: %s | context: %s", error_line, e->context);

    rrc_log_fatal_screen_and_exit(error_line, e->context, where, e->diag);
}

void rrc_result_free(struct rrc_result result)
{
    if (result.err)
    {
        if (!result.err->reported)
        {
            rrc_log_write(RRC_LOG_WARN, result.err->site_file, result.err->site_line, result.err->site_func,
                          "error was handled internally / discarded without the generic error screen: %s", result.err->context);
        }
        if (result.err != &fallback_error.err)
        {
            free(result.err);
        }
    }
}
