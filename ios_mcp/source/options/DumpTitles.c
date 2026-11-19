// READ-ONLY dump of system title folders from MLC to sd:/dump/ (no button input).
// Runs in its own thread with a large stack (menu thread has only 0x1000 bytes).
#include "DumpTitles.h"

#include "menu.h"
#include "gfx.h"
#include "fsa.h"
#include "imports.h"
#include "utils.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_DEPTH 6
#define PATH_LEN 200
#define BIG_BUF 0x8000
#define SMALL_BUF 0x1000

// all large buffers live on the heap (ios_mcp .bss is limited to 0x3000)
typedef struct {
    char src[MAX_DEPTH + 1][PATH_LEN];
    char dst[MAX_DEPTH + 1][PATH_LEN];
    FSDirectoryEntry ent[MAX_DEPTH + 1];
} DumpWork;

static DumpWork* W = NULL;
#define srcPath (W->src)
#define dstPath (W->dst)
#define dirEntry (W->ent)
static uint32_t filesOk, filesFail, bytesTotal;
static uint32_t screenY;

// JPN system titles touched by the language swap; HOME Menu first.
static const char* const titles[] = {
    "00050030/1001000a", // HOME Menu (hbm) - crashes on boot
    "00050030/1001100a", // Error display
    "00050010/10040000", // Wii U Menu
    "00050010/10047000", // System Settings
    "00050010/10048000", // Parental Controls
    "00050010/10049000", // User Settings
    "00050010/1004c000", // Daily Log
    "00050030/1001500a", // Friend List
    "00050030/1001800a", // Download Management
};

static int copyFileBig(const char* src, const char* dst, uint32_t* outSize)
{
    int rh, wh;
    int res = FSA_OpenFile(fsaHandle, src, "r", &rh);
    if (res < 0) {
        return res;
    }
    res = FSA_OpenFile(fsaHandle, dst, "w", &wh);
    if (res < 0) {
        FSA_CloseFile(fsaHandle, rh);
        return res;
    }

    uint32_t bufSize = BIG_BUF;
    void* buf = IOS_HeapAllocAligned(CROSS_PROCESS_HEAP_ID, bufSize, 0x40);
    if (!buf) {
        bufSize = SMALL_BUF;
        buf = IOS_HeapAllocAligned(CROSS_PROCESS_HEAP_ID, bufSize, 0x40);
    }
    if (!buf) {
        FSA_CloseFile(fsaHandle, wh);
        FSA_CloseFile(fsaHandle, rh);
        return -1;
    }

    uint32_t total = 0;
    while ((res = FSA_ReadFile(fsaHandle, buf, 1, bufSize, rh, 0)) > 0) {
        int w = FSA_WriteFile(fsaHandle, buf, 1, res, wh, 0);
        if (w < 0) {
            res = w;
            break;
        }
        total += res;
    }

    IOS_HeapFree(CROSS_PROCESS_HEAP_ID, buf);
    FSA_CloseFile(fsaHandle, wh);
    FSA_CloseFile(fsaHandle, rh);
    *outSize = total;
    return (res >= 0) ? 0 : res;
}

static void dumpDir(int depth)
{
    int res = FSA_MakeDir(fsaHandle, dstPath[depth], 0x600);
    if (res < 0 && res != -0x30016) {
        diag_log("mkdir %s -> %x", dstPath[depth], res);
    }

    int dh;
    res = FSA_OpenDir(fsaHandle, srcPath[depth], &dh);
    if (res < 0) {
        diag_log("opendir %s -> %x", srcPath[depth], res);
        return;
    }

    while (FSA_ReadDir(fsaHandle, dh, &dirEntry[depth]) >= 0) {
        if (depth >= MAX_DEPTH) {
            diag_log("too deep: %s", srcPath[depth]);
            break;
        }
        snprintf(srcPath[depth + 1], PATH_LEN, "%s/%s", srcPath[depth], dirEntry[depth].name);
        snprintf(dstPath[depth + 1], PATH_LEN, "%s/%s", dstPath[depth], dirEntry[depth].name);

        if (dirEntry[depth].stat.flags & DIR_ENTRY_IS_DIRECTORY) {
            dumpDir(depth + 1);
        } else {
            uint32_t got = 0;
            gfx_printf(16, screenY, GfxPrintFlag_ClearBG, "%lu ok %lu fail: %s", filesOk, filesFail, dirEntry[depth].name);
            res = copyFileBig(srcPath[depth + 1], dstPath[depth + 1], &got);
            if (res < 0) {
                filesFail++;
            } else {
                filesOk++;
            }
            bytesTotal += got;
            diag_log("F %s size=%lu got=%lu res=%x", srcPath[depth + 1], dirEntry[depth].stat.size, got, res);
        }
    }
    FSA_CloseDir(fsaHandle, dh);
}

static int workerThread(void* arg)
{
    FSA_MakeDir(fsaHandle, "/vol/storage_recovsd/dump", 0x600);
    for (uint32_t i = 0; i < sizeof(titles) / sizeof(titles[0]); i++) {
        snprintf(srcPath[0], PATH_LEN, "/vol/storage_mlc01/sys/title/%s", titles[i]);
        snprintf(dstPath[0], PATH_LEN, "/vol/storage_recovsd/dump/%.8s_%s", titles[i], titles[i] + 9);
        diag_log("TITLE %s begin (ok=%lu fail=%lu bytes=%lu)", titles[i], filesOk, filesFail, bytesTotal);
        dumpDir(0);
        diag_log("TITLE %s end (ok=%lu fail=%lu bytes=%lu)", titles[i], filesOk, filesFail, bytesTotal);
        FSA_FlushVolume(fsaHandle, "/vol/storage_recovsd");
    }
    return 0;
}

void option_DumpTitles(void)
{
    gfx_clear(COLOR_BACKGROUND);
    drawTopBar("Dumping system titles to sd:/dump (read-only)");
    setNotificationLED(NOTIF_LED_RED_BLINKING, 0);
    screenY = 16 + 8 + 2 + 8;
    filesOk = filesFail = bytesTotal = 0;

    diag_log("DumpTitles start");
    W = IOS_HeapAllocAligned(CROSS_PROCESS_HEAP_ID, sizeof(DumpWork), 0x40);
    if (!W) {
        diag_log("heap alloc %lu failed", (uint32_t) sizeof(DumpWork));
        printf_error(screenY, "Out of memory");
        return;
    }
    workerThread(NULL);
    IOS_HeapFree(CROSS_PROCESS_HEAP_ID, W);
    W = NULL;

    diag_log("ALL DONE ok=%lu fail=%lu bytes=%lu", filesOk, filesFail, bytesTotal);
    setNotificationLED(NOTIF_LED_PURPLE, 0);
    gfx_set_font_color(COLOR_SUCCESS);
    gfx_printf(16, screenY + CHAR_SIZE_DRC_Y + 4, 0, "Done! %lu files, %lu fail, %lu bytes", filesOk, filesFail, bytesTotal);
    usleep(10 * 1000 * 1000);
}
