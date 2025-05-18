#pragma once

// Device type -- in the "User Defined" range."
#define PROCSPY_TYPE 0x8123

// The IOCTL function codes from 0x800 to 0xFFF are for customer use.
#define IOCTL_PROCSPY_GET_SPAWNED_PROCESSES \
    CTL_CODE(PROCSPY_TYPE, 0x801, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

#define DRIVER_NAME  "ProcSpy"

typedef CHAR PROC_NAME[0x100];

struct PROCSPY_MESSAGE {
    PROC_NAME NewProcName;
    BOOLEAN MoreAvailable;
    BOOLEAN TerminateLast;
};
