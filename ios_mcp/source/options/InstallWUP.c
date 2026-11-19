#include "InstallWUP.h"

#include "menu.h"
#include "gfx.h"
#include "mcp_install.h"
#include "imports.h"
#include "utils.h"
#include <unistd.h>
#include <string.h>
#include "diag.h"
#include "options.h"

static int callbackQueue = -1;
static uint8_t callbackThreadStack[0x200] __attribute__((aligned(0x20)));
static int asyncPending = 0;
static int asyncResult = -1;

static int callbackThread(void* arg)
{
    MCPAsyncReply* reply = NULL;
    int ret = IOS_ReceiveMessage(callbackQueue, (uint32_t*)&reply, IOS_MESSAGE_FLAGS_NONE);
    if (ret >= 0 && reply) {
        asyncResult = reply->reply.result;
        asyncPending = 0;

        // Free original request
        IOS_HeapFree(CROSS_PROCESS_HEAP_ID, reply->ioBuf);
    }

    return 0;
}

void option_InstallWUP(void)
{
    gfx_clear(COLOR_BACKGROUND);
    drawTopBar("Install WUP");
    setNotificationLED(NOTIF_LED_RED_BLINKING, 0);

    uint32_t index = 16 + 8 + 2 + 8;

    diag_log("InstallWUP start");
    int mcpHandle = IOS_Open("/dev/mcp", 0);
    diag_log("IOS_Open /dev/mcp -> %x", mcpHandle);
    if (mcpHandle < 0) {
        printf_error(index, "Failed to open /dev/mcp: %x", mcpHandle);
        return;
    }

    gfx_print(16, index, 0, "Querying install info...");
    index += CHAR_SIZE_DRC_Y + 4;

    MCPInstallInfo info;
    int res = MCP_InstallGetInfo(mcpHandle, "/vol/storage_recovsd/install", &info);
    diag_log("MCP_InstallGetInfo -> %x title %08lx-%08lx", res,
        (uint32_t)(info.titleId >> 32), (uint32_t)(info.titleId & 0xFFFFFFFFU));
    if (res < 0) {
        IOS_Close(mcpHandle);
        gfx_print(16, index, 0, "Make sure to place a valid signed WUP directly in 'sd:/install'");
        index += CHAR_SIZE_DRC_Y + 4;
        printf_error(index, "Failed to get install info: %x", res);
        return;
    }

    index = gfx_printf(16, index, 0, "Found title: %08lx-%08lx\nDo you want to install this title?",
        (uint32_t)(info.titleId >> 32), (uint32_t)(info.titleId & 0xFFFFFFFFU));
    index += CHAR_SIZE_DRC_Y + 4;

    int selected = 1; // auto-confirm install (no button input)
    if (selected == 0) {
        IOS_Close(mcpHandle);
        return;
    }

    gfx_clear(COLOR_BACKGROUND);
    drawTopBar("Install WUP");
    index = 16 + 8 + 2 + 8;

    gfx_printf(16, index, 0, "Starting install for title: %08lx-%08lx",
        (uint32_t)(info.titleId >> 32), (uint32_t)(info.titleId & 0xFFFFFFFFU));
    index += CHAR_SIZE_DRC_Y + 4;

    // only install to NAND
    res = MCP_InstallSetTargetDevice(mcpHandle, 0);
    diag_log("SetTargetDevice(0) -> %x", res);
    if (res < 0) {
        IOS_Close(mcpHandle);
        printf_error(index, "MCP_InstallSetTargetDevice: %x", res);
        return;
    }
    res = MCP_InstallSetTargetUsb(mcpHandle, 0);
    diag_log("SetTargetUsb(0) -> %x", res);
    if (res < 0) {
        IOS_Close(mcpHandle);
        printf_error(index, "MCP_InstallSetTargetUsb: %x", res);
        return;
    }

    uint32_t messages[5];
    callbackQueue = IOS_CreateMessageQueue(messages, sizeof(messages) / sizeof(uint32_t));
    if (callbackQueue < 0) {
        IOS_Close(mcpHandle);
        printf_error(index, "IOS_CreateMessageQueue: %x", res);
        return;
    }

    int callbackThreadId = IOS_CreateThread(callbackThread, NULL, callbackThreadStack + sizeof(callbackThreadStack), sizeof(callbackThreadStack), IOS_GetThreadPriority(0), IOS_THREAD_FLAGS_NONE);
    if (callbackThreadId < 0) {
        IOS_DestroyMessageQueue(callbackQueue);
        IOS_Close(mcpHandle);
        printf_error(index, "IOS_CreateThread: %x", res);
        return;   
    }

    if (IOS_StartThread(callbackThreadId) < 0) {
        IOS_DestroyMessageQueue(callbackQueue);
        IOS_Close(mcpHandle);
        printf_error(index, "IOS_StartThread failed");
        return; 
    }

    asyncResult = -1;
    asyncPending = 1;
    res = MCP_InstallTitleAsync(mcpHandle, "/vol/storage_recovsd/install", callbackQueue);
    diag_log("MCP_InstallTitleAsync -> %x", res);
    if (res < 0) {
        // Send a NULL message and wait for the thread to stop
        IOS_SendMessage(callbackQueue, 0, IOS_MESSAGE_FLAGS_NONE);
        IOS_JoinThread(callbackThreadId, NULL);

        IOS_DestroyMessageQueue(callbackQueue);
        IOS_Close(mcpHandle);
        printf_error(index, "Failed to start install: %x", res);
        return;
    }

    uint32_t loops = 0, lastKiB = 0xFFFFFFFFu, stallLoops = 0, stallDumped = 0;
    while (asyncPending) {
        MCPInstallProgress progress;
        memset(&progress, 0, sizeof(progress));
        res = MCP_InstallGetProgress(mcpHandle, &progress);
        uint32_t curKiB = (uint32_t) (progress.sizeProgress / 1024llu);
        if (res >= 0) {
            gfx_printf(16, index, GfxPrintFlag_ClearBG, "Installing... (%lu KiB / %lu KiB)", curKiB, (uint32_t) (progress.sizeTotal / 1024llu));
        }

        // log every ~5 s (100 * 50 ms): heartbeat shows whether IOSU is alive while MCP is stuck
        if ((loops % 100) == 0) {
            diag_log("t=%lus res=%x inProg=%lu %lu/%lu KiB contents %lu/%lu",
                loops / 20, res, progress.inProgress, curKiB,
                (uint32_t) (progress.sizeTotal / 1024llu),
                progress.contentsProgress, progress.contentsTotal);
        }

        if (curKiB == lastKiB) {
            stallLoops++;
        } else {
            stallLoops = 0;
            lastKiB = curKiB;
        }
        // stuck for ~120 s: try to grab the system logs once while still running
        if (stallLoops == 2400 && !stallDumped) {
            stallDumped = 1;
            diag_log("STALL 120s at %lu KiB -> dumping syslogs", curKiB);
            diag_dump_syslogs("logs_stall");
            diag_log("syslog dump after stall done");
        }

        loops++;
        usleep(50 * 1000);
    }
    diag_log("install finished asyncResult=%x after %lus", asyncResult, loops / 20);
    index += CHAR_SIZE_DRC_Y + 4;

    if (asyncResult < 0) {
        printf_error(index, "Failed to install: %x", asyncResult);
    } else {
        setNotificationLED(NOTIF_LED_PURPLE, 0);
        gfx_set_font_color(COLOR_SUCCESS);
        gfx_printf(16, index, 0, "Done!");
        usleep(5 * 1000 * 1000); // wait 5s instead of button press
    }

    IOS_JoinThread(callbackThreadId, NULL);
    IOS_DestroyMessageQueue(callbackQueue);

    IOS_Close(mcpHandle);
}
