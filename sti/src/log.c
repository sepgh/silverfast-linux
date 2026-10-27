#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

#include "log.h"

void sfl_log(const char *fmt, ...)
{
    char line[1024], path[MAX_PATH];
    int n;
    va_list ap;
    DWORD len;
    HANDLE file;

    n = _snprintf(line, sizeof(line) - 2, "[sti %lu %04lx] ", GetTickCount(), GetCurrentThreadId());
    va_start(ap, fmt);
    _vsnprintf(line + n, sizeof(line) - 2 - n, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 3] = 0;
    strcat(line, "\n");

    len = GetEnvironmentVariableA("SFL_STI_LOG", path, sizeof(path));
    if (!len || len >= sizeof(path))
    {
        OutputDebugStringA(line);
        return;
    }
    file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, line, (DWORD)strlen(line), &len, NULL);
    CloseHandle(file);
}
