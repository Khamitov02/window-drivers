#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strsafe.h>
#include <sys\driver.h>


BOOLEAN DriverControl(
    _In_ BOOLEAN Indicator
);

VOID __cdecl main(
    _In_ ULONG argc,
    _In_reads_(argc) PCHAR argv[]
) {
    if (argc >= 2) {
        if (strcmp(argv[1], "init") == 0) {
            printf("Installing my driver\n");
            if (!DriverControl(FALSE)) {
                printf("Can't install driver.\n");
                DriverControl(TRUE);
                return;
            }
            printf("Driver installed.\n");
            return;
        }

        if (strcmp(argv[1], "delete") == 0) {
            printf("Deleting driver\n");
            if (!DriverControl(TRUE)) {
                printf("Can't delete driver.\n");
                return;
            }
            printf("Driver deleted.\n");
            return;
        }

        printf("Can't process args...\n");
    }

    printf("Process spy initializing...\n");
    HANDLE hDevice;
    if ((hDevice = CreateFile("\\\\.\\PROCSPY",
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL)) == INVALID_HANDLE_VALUE)
    {
        printf("Can't open device driver: %d\n", GetLastError());
        return;
    }

    struct PROCSPY_MESSAGE message;
    memset(&message, 0, sizeof(message));
    BOOL newline = FALSE;

    while (TRUE) {
        BOOL bRc = DeviceIoControl(hDevice,
            (DWORD)IOCTL_PROCSPY_GET_SPAWNED_PROCESSES,
            &message,
            (DWORD)sizeof(message),
            &message,
            (DWORD)sizeof(message),
            NULL,
            NULL);
        DWORD status = 0;
        if (!bRc) {
            status = GetLastError();
        }

        switch (status) {
        case 0: {
            if (newline) {
                puts("");
                newline = FALSE;
            }

            printf("> %.*s\n", (int)sizeof(message.NewProcName), message.NewProcName);

            if (strcmp(message.NewProcName, "\\??\\C:\\Program Files\\WindowsApps\\Microsoft.WindowsNotepad_11.2503.16.0_x64__8wekyb3d8bbwe\\Notepad\\Notepad.exe") == 0) {
                message.TerminateLast = TRUE;
            }

            if (message.MoreAvailable) {
                continue;
            }
        } break;

        case ERROR_NO_MORE_ITEMS: {
            putchar('.');
            newline = TRUE;
        } break;

        default: {
            printf("Error in DevIoControl: %d", GetLastError());
            return;
        } break;
        }

        Sleep(1000);
    }

    // close the handle to the device.
    CloseHandle(hDevice);
}
