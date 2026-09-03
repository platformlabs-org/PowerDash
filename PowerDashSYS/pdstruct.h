#ifndef POWERDASH_IOCTL_HEADER
#define POWERDASH_IOCTL_HEADER


#ifndef CTL_CODE
#include <WinIoCtl.h>
#endif
/* NOTE: keep this header ASCII-only and free of <stdint.h>: the WDK kernel CRT
 * has no stdint.h and MSVC's copy conflicts with km/crt/crtdefs.h (C4005/C4083,
 * warnings-as-errors). uint32_t is therefore spelled ULONG32 (ntdef.h kernel /
 * basetsd.h user) - identical unsigned 32-bit layout on both sides. */

#define POWERDASH_DEV_TYPE 55000

#define IO_CTL_MSR_READ     CTL_CODE(POWERDASH_DEV_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_MSR_WRITE    CTL_CODE(POWERDASH_DEV_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_PCICFG_READ  CTL_CODE(POWERDASH_DEV_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_PCICFG_WRITE CTL_CODE(POWERDASH_DEV_TYPE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_MMAP_SUPPORT CTL_CODE(POWERDASH_DEV_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_MMAP         CTL_CODE(POWERDASH_DEV_TYPE, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_MUNMAP       CTL_CODE(POWERDASH_DEV_TYPE, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_PMU_ALLOC_SUPPORT CTL_CODE(POWERDASH_DEV_TYPE, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_PMU_ALLOC         CTL_CODE(POWERDASH_DEV_TYPE, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_PMU_FREE          CTL_CODE(POWERDASH_DEV_TYPE, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_FNQ_INJECT        CTL_CODE(POWERDASH_DEV_TYPE, 0x80A, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_SMN_READ          CTL_CODE(POWERDASH_DEV_TYPE, 0x80B, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_SMN_WRITE         CTL_CODE(POWERDASH_DEV_TYPE, 0x80C, METHOD_BUFFERED, FILE_ANY_ACCESS)

struct MSR_Request
{
    int core_id;
    ULONG64 msr_address;
    ULONG64 write_value;     /* value to write if write requet
                                 ignored if read request */
};

struct PCICFG_Request
{
    ULONG bus, dev, func, reg, bytes;
    // "bytes" can be only 4 or 8
    /* value to write if write request ignored if read request */
    ULONG64 write_value;
};

struct MMAP_Request
{
    LARGE_INTEGER address;
    SIZE_T size;
};

struct SMN_Request
{
    ULONG32 address;      // SMN address (e.g. 0x59800)
    ULONG32 value;        // read request: output, 32-bit content of that address
                          // write request: input, value to write (SMU mailbox 0x3B10xxx needs writes)
};


#endif
