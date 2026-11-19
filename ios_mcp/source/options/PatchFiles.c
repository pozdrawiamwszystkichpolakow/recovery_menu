// Auto-patch: copy every file under sd:/patch/<rel> over /vol/storage_mlc01/<rel>,
// then read both back and compare. No button input.
// LED: red blinking = working, purple = all files written AND verified, solid red = failure.
// Only overwrites files whose target directory already exists (never creates dirs on MLC).
#include "PatchFiles.h"

#include "menu.h"
#include "gfx.h"
#include "fsa.h"
#include "imports.h"
#include "utils.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_DEPTH 10
#define PATH_LEN 200
#define CHUNK 0x8000

#define SD_ROOT  "/vol/storage_recovsd/patch"
#define MLC_ROOT "/vol/storage_mlc01"

typedef struct {
    char src[MAX_DEPTH + 1][PATH_LEN];
    char dst[MAX_DEPTH + 1][PATH_LEN];
    FSDirectoryEntry ent[MAX_DEPTH + 1];
} PatchWork;

static PatchWork* W = NULL;
static uint8_t* bufA = NULL;
static uint8_t* bufB = NULL;
static uint32_t nOk, nFail;
static uint32_t screenY;

#define PROGRESS_PATH "/vol/storage_recovsd/patch_progress.txt"

// progress file format: "<offset> <dst path>\n"
static uint32_t readProgress(const char* dst)
{
    int fh;
    if (FSA_OpenFile(fsaHandle, PROGRESS_PATH, "r", &fh) < 0) {
        return 0;
    }
    char* buf = (char*) bufB;
    memset(buf, 0, 400);
    int r = FSA_ReadFile(fsaHandle, buf, 1, 399, fh, 0);
    FSA_CloseFile(fsaHandle, fh);
    if (r <= 0) {
        return 0;
    }
    uint32_t off = 0;
    char* q = buf;
    while (*q >= '0' && *q <= '9') {
        off = off * 10 + (uint32_t) (*q - '0');
        q++;
    }
    if (*q != ' ') {
        return 0;
    }
    q++;
    char* nl = strchr(q, '\n');
    if (nl) {
        *nl = '\0';
    }
    return (strcmp(q, dst) == 0) ? off : 0;
}

static void saveProgress(uint32_t off, const char* dst)
{
    int fh;
    if (FSA_OpenFile(fsaHandle, PROGRESS_PATH, "w", &fh) < 0) {
        return;
    }
    char* buf = (char*) bufB;
    int n = snprintf(buf, 400, "%lu %s\n", off, dst);
    FSA_WriteFile(fsaHandle, buf, 1, n, fh, 0);
    FSA_CloseFile(fsaHandle, fh);
    FSA_FlushVolume(fsaHandle, "/vol/storage_recovsd");
}

// Resumable write: continues from the offset recorded in sd:/patch_progress.txt.
// After each chunk: flush MLC, then record the new offset on SD.
static int writeFile(const char* src, const char* dst, uint32_t* written)
{
    int rh, wh;
    *written = 0;
    uint32_t off = readProgress(dst);
    diag_log("resume offset %lu", off);

    int res = FSA_OpenFile(fsaHandle, src, "r", &rh);
    if (res < 0) {
        return res;
    }
    if (off > 0) {
        res = FSA_OpenFile(fsaHandle, dst, "r+", &wh);
        if (res >= 0) {
            res = FSA_SetPosFile(fsaHandle, wh, off);
            if (res < 0) {
                FSA_CloseFile(fsaHandle, wh);
            }
        }
        if (res < 0) {
            diag_log("resume open/seek failed %x -> restart from 0", res);
            off = 0;
        }
    }
    if (off == 0) {
        // "w" truncates the existing target; target dir must already exist
        res = FSA_OpenFile(fsaHandle, dst, "w", &wh);
        if (res < 0) {
            FSA_CloseFile(fsaHandle, rh);
            return res;
        }
        saveProgress(0, dst);
    }
    res = FSA_SetPosFile(fsaHandle, rh, off);
    if (res < 0) {
        FSA_CloseFile(fsaHandle, wh);
        FSA_CloseFile(fsaHandle, rh);
        return res;
    }

    *written = off;
    while ((res = FSA_ReadFile(fsaHandle, bufA, 1, CHUNK, rh, 0)) > 0) {
        int w = FSA_WriteFile(fsaHandle, bufA, 1, res, wh, 0);
        if (w < 0) {
            res = w;
            break;
        }
        FSA_FlushVolume(fsaHandle, MLC_ROOT);
        *written += res;
        saveProgress(*written, dst);
        gfx_printf(16, screenY, GfxPrintFlag_ClearBG, "Writing... %lu KiB (resumed at %lu KiB)", *written / 1024, off / 1024);
    }
    FSA_CloseFile(fsaHandle, wh);
    FSA_CloseFile(fsaHandle, rh);
    FSA_FlushVolume(fsaHandle, MLC_ROOT);
    return (res >= 0) ? 0 : res;
}

static int verifyFile(const char* a, const char* b)
{
    int ha, hb;
    int res = FSA_OpenFile(fsaHandle, a, "r", &ha);
    if (res < 0) {
        return res;
    }
    res = FSA_OpenFile(fsaHandle, b, "r", &hb);
    if (res < 0) {
        FSA_CloseFile(fsaHandle, ha);
        return res;
    }
    int ret = 0;
    while (1) {
        int ra = FSA_ReadFile(fsaHandle, bufA, 1, CHUNK, ha, 0);
        int rb = FSA_ReadFile(fsaHandle, bufB, 1, CHUNK, hb, 0);
        if (ra < 0 || rb < 0) {
            ret = (ra < 0) ? ra : rb;
            break;
        }
        if (ra != rb || memcmp(bufA, bufB, ra) != 0) {
            ret = -2; // mismatch
            break;
        }
        if (ra == 0) {
            break;
        }
    }
    FSA_CloseFile(fsaHandle, hb);
    FSA_CloseFile(fsaHandle, ha);
    return ret;
}

static void patchDir(int depth)
{
    int dh;
    int res = FSA_OpenDir(fsaHandle, W->src[depth], &dh);
    if (res < 0) {
        diag_log("opendir %s -> %x", W->src[depth], res);
        nFail++;
        return;
    }
    while (FSA_ReadDir(fsaHandle, dh, &W->ent[depth]) >= 0) {
        if (depth >= MAX_DEPTH) {
            break;
        }
        snprintf(W->src[depth + 1], PATH_LEN, "%s/%s", W->src[depth], W->ent[depth].name);
        snprintf(W->dst[depth + 1], PATH_LEN, "%s/%s", W->dst[depth], W->ent[depth].name);
        if (W->ent[depth].stat.flags & DIR_ENTRY_IS_DIRECTORY) {
            patchDir(depth + 1);
            continue;
        }
        uint32_t sz = W->ent[depth].stat.size;
        diag_log("W %s (%lu B) begin", W->dst[depth + 1], sz);
        uint32_t written = 0;
        res = writeFile(W->src[depth + 1], W->dst[depth + 1], &written);
        FSA_FlushVolume(fsaHandle, MLC_ROOT);
        int ver = (res >= 0) ? verifyFile(W->src[depth + 1], W->dst[depth + 1]) : res;
        diag_log("W %s write=%x written=%lu verify=%x", W->dst[depth + 1], res, written, ver);
        if (res >= 0 && ver == 0 && written == sz) {
            nOk++;
            saveProgress(0, "done");
        } else {
            nFail++;
            if (res >= 0 && written == sz) {
                // fully written but verify failed: start over next boot
                saveProgress(0, "verify-failed");
            }
        }
    }
    FSA_CloseDir(fsaHandle, dh);
}

void option_PatchFiles(void)
{
    gfx_clear(COLOR_BACKGROUND);
    drawTopBar("Patching MLC files from sd:/patch");
    setNotificationLED(NOTIF_LED_RED_BLINKING, 0);
    screenY = 16 + 8 + 2 + 8;
    nOk = nFail = 0;

    W = IOS_HeapAllocAligned(CROSS_PROCESS_HEAP_ID, sizeof(PatchWork), 0x40);
    bufA = IOS_HeapAllocAligned(CROSS_PROCESS_HEAP_ID, CHUNK, 0x40);
    bufB = IOS_HeapAllocAligned(CROSS_PROCESS_HEAP_ID, CHUNK, 0x40);
    if (!W || !bufA || !bufB) {
        diag_log("PATCH: out of memory");
        setNotificationLED(NOTIF_LED_RED, 0);
        usleep(15 * 1000 * 1000);
        return;
    }

    diag_log("PATCH start");
    strncpy(W->src[0], SD_ROOT, PATH_LEN);
    strncpy(W->dst[0], MLC_ROOT, PATH_LEN);
    patchDir(0);
    FSA_FlushVolume(fsaHandle, MLC_ROOT);
    diag_log("PATCH end ok=%lu fail=%lu", nOk, nFail);

    IOS_HeapFree(CROSS_PROCESS_HEAP_ID, bufB);
    IOS_HeapFree(CROSS_PROCESS_HEAP_ID, bufA);
    IOS_HeapFree(CROSS_PROCESS_HEAP_ID, W);

    if (nOk > 0 && nFail == 0) {
        setNotificationLED(NOTIF_LED_PURPLE, 0);
        gfx_set_font_color(COLOR_SUCCESS);
        gfx_printf(16, screenY + CHAR_SIZE_DRC_Y + 4, 0, "Done! %lu file(s) written and verified", nOk);
    } else {
        setNotificationLED(NOTIF_LED_RED, 0);
        gfx_set_font_color(COLOR_ERROR);
        gfx_printf(16, screenY + CHAR_SIZE_DRC_Y + 4, 0, "FAILED: ok=%lu fail=%lu", nOk, nFail);
    }
    usleep(15 * 1000 * 1000);
}
