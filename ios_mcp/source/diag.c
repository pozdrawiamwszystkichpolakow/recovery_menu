#include "diag.h"

#include "menu.h"
#include "fsa.h"
#include "imports.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DIAG_PATH "/vol/storage_recovsd/diag.txt"
#define DIAG_BUF_SIZE 0x200

static uint32_t diagSeq = 0;

void diag_log(const char* format, ...)
{
    if (fsaHandle <= 0) {
        return;
    }

    char* buf = IOS_HeapAllocAligned(CROSS_PROCESS_HEAP_ID, DIAG_BUF_SIZE, 0x40);
    if (!buf) {
        return;
    }

    int len = snprintf(buf, DIAG_BUF_SIZE, "[%04lu] ", diagSeq++);
    va_list args;
    va_start(args, format);
    len += vsnprintf(buf + len, DIAG_BUF_SIZE - len - 2, format, args);
    va_end(args);
    if (len > DIAG_BUF_SIZE - 2) {
        len = DIAG_BUF_SIZE - 2;
    }
    buf[len++] = '\n';
    buf[len] = '\0';

    int fh;
    if (FSA_OpenFile(fsaHandle, DIAG_PATH, "a", &fh) >= 0) {
        FSA_WriteFile(fsaHandle, buf, 1, len, fh, 0);
        FSA_CloseFile(fsaHandle, fh);
        FSA_FlushVolume(fsaHandle, "/vol/storage_recovsd");
    }

    IOS_HeapFree(CROSS_PROCESS_HEAP_ID, buf);
}
