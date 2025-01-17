//
// Copyright (C) 2017 Tavis Ormandy
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdarg.h>
#include <assert.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/unistd.h>
#include <asm/unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <mcheck.h>
#include <err.h>

#include "winnt_types.h"
#include "pe_linker.h"
#include "ntoskernel.h"
#include "util.h"
#include "hook.h"
#include "log.h"
#include "rsignal.h"
#include "engineboot.h"
#include "scanreply.h"
#include "streambuffer.h"
#include "openscan.h"

DWORD isResponsePositive;
ULONGLONG leftOffset;
ULONGLONG subFileSize;
DWORD remainingSize;
CHAR lastVirusName[50];
// Any usage limits to prevent bugs disrupting system.
const struct rlimit kUsageLimits[] = {
    [RLIMIT_FSIZE]  = { .rlim_cur = 0x20000000, .rlim_max = 0x20000000 },
    [RLIMIT_CPU]    = { .rlim_cur = 3600,       .rlim_max = RLIM_INFINITY },
    [RLIMIT_CORE]   = { .rlim_cur = 0,          .rlim_max = 0 },
    [RLIMIT_NOFILE] = { .rlim_cur = 32,         .rlim_max = 32 },
};

DWORD (* __rsignal)(PHANDLE KernelHandle, DWORD Code, PVOID Params, DWORD Size);

static DWORD EngineScanCallback(PSCANSTRUCT Scan){
    if ((Scan->Flags & 0x08000022) || (Scan->Flags & 0x40010000) == 0x40010000) {
        isResponsePositive = TRUE;
        strcpy(lastVirusName, Scan->VirusName);
    }
    return 0;
}
static DWORD ReadStream(PVOID this, ULONGLONG Offset, PVOID Buffer, DWORD Size, PDWORD SizeRead)
{
    fseek(this, leftOffset + Offset, SEEK_SET);
    DWORD sizeToRead = (Size < remainingSize)? Size : remainingSize;
    *SizeRead = fread(Buffer, 1, sizeToRead, this);
    remainingSize -= sizeToRead;
    return TRUE;
}

static DWORD GetStreamSize(PVOID this, PULONGLONG FileSize)
{
    fseek(this, 0, SEEK_END);
    *FileSize = ftell(this);
    return TRUE;
}

static DWORD GetCustomStreamSize(PVOID this, PULONGLONG FileSize){
    *FileSize = subFileSize;
    return TRUE;
}

static PWCHAR GetStreamName(PVOID this)
{
    return L"input";
}

// These are available for pintool.
BOOL __noinline InstrumentationCallback(PVOID ImageStart, SIZE_T ImageSize)
{
    // Prevent the call from being optimized away.
    asm volatile ("");
    return TRUE;
}

int main(int argc, char **argv, char **envp)
{
    PIMAGE_DOS_HEADER DosHeader;
    PIMAGE_NT_HEADERS PeHeader;
    HANDLE KernelHandle;
    SCAN_REPLY ScanReply;
    BOOTENGINE_PARAMS BootParams;
    SCANSTREAM_PARAMS ScanParams;
    STREAMBUFFER_DESCRIPTOR ScanDescriptor;
    ENGINE_INFO EngineInfo;
    ENGINE_CONFIG EngineConfig;
    struct pe_image image = {
        .entry  = NULL,
        .name   = "engine/mpengine.dll",
    };

    // Load the mpengine module.
    if (pe_load_library(image.name, &image.image, &image.size) == false) {
        LogMessage("You must add the dll and vdm files to the engine directory");
        return 1;
    }

    // Handle relocations, imports, etc.
    link_pe_images(&image, 1);

    // Fetch the headers to get base offsets.
    DosHeader   = (PIMAGE_DOS_HEADER) image.image;
    PeHeader    = (PIMAGE_NT_HEADERS)(image.image + DosHeader->e_lfanew);

    // Load any additional exports.
    if (!process_extra_exports(image.image, PeHeader->OptionalHeader.BaseOfCode, "engine/mpengine.map")) {
#ifndef NDEBUG
        LogMessage("The map file wasn't found, symbols wont be available");
#endif
    } else {
        // Calculate the commands needed to get export and map symbols visible in gdb.
        if (IsGdbPresent()) {
            LogMessage("GDB: add-symbol-file %s %#x+%#x",
                       image.name,
                       image.image,
                       PeHeader->OptionalHeader.BaseOfCode);
            LogMessage("GDB: shell bash genmapsym.sh %#x+%#x symbols_%d.o < %s",
                       image.image,
                       PeHeader->OptionalHeader.BaseOfCode,
                       getpid(),
                       "engine/mpengine.map");
            LogMessage("GDB: add-symbol-file symbols_%d.o 0", getpid());
            __debugbreak();
        }
    }

    if (get_export("__rsignal", &__rsignal) == -1) {
        errx(EXIT_FAILURE, "Failed to resolve mpengine entrypoint");
    }

    EXCEPTION_DISPOSITION ExceptionHandler(struct _EXCEPTION_RECORD *ExceptionRecord,
            struct _EXCEPTION_FRAME *EstablisherFrame,
            struct _CONTEXT *ContextRecord,
            struct _EXCEPTION_FRAME **DispatcherContext)
    {
        LogMessage("Toplevel Exception Handler Caught Exception");
        abort();
    }

    VOID ResourceExhaustedHandler(int Signal)
    {
        errx(EXIT_FAILURE, "Resource Limits Exhausted, Signal %s", strsignal(Signal));
    }

    setup_nt_threadinfo(ExceptionHandler);

    // Call DllMain()
    image.entry((PVOID) 'MPEN', DLL_PROCESS_ATTACH, NULL);

    // Install usage limits to prevent system crash.
    setrlimit(RLIMIT_CORE, &kUsageLimits[RLIMIT_CORE]);
    setrlimit(RLIMIT_CPU, &kUsageLimits[RLIMIT_CPU]);
    setrlimit(RLIMIT_FSIZE, &kUsageLimits[RLIMIT_FSIZE]);
    setrlimit(RLIMIT_NOFILE, &kUsageLimits[RLIMIT_NOFILE]);

    signal(SIGXCPU, ResourceExhaustedHandler);
    signal(SIGXFSZ, ResourceExhaustedHandler);

# ifndef NDEBUG
    // Enable Maximum heap checking.
    mcheck_pedantic(NULL);
# endif

    ZeroMemory(&BootParams, sizeof BootParams);
    ZeroMemory(&EngineInfo, sizeof EngineInfo);
    ZeroMemory(&EngineConfig, sizeof EngineConfig);

    BootParams.ClientVersion = BOOTENGINE_PARAMS_VERSION;
    BootParams.Attributes    = BOOT_ATTR_NORMAL;
    BootParams.SignatureLocation = L"engine";
    BootParams.ProductName = L"Legitimate Antivirus";
    EngineConfig.QuarantineLocation = L"quarantine";
    EngineConfig.Inclusions = L"*.*";
    EngineConfig.EngineFlags = 1 << 1;
    BootParams.EngineInfo = &EngineInfo;
    BootParams.EngineConfig = &EngineConfig;
    KernelHandle = NULL;

    if (__rsignal(&KernelHandle, RSIG_BOOTENGINE, &BootParams, sizeof BootParams) != 0) {
        LogMessage("__rsignal(RSIG_BOOTENGINE) returned failure, missing definitions?");
        LogMessage("Make sure the VDM files and mpengine.dll are in the engine directory");
        return 1;
    }

    ZeroMemory(&ScanParams, sizeof ScanParams);
    ZeroMemory(&ScanDescriptor, sizeof ScanDescriptor);
    ZeroMemory(&ScanReply, sizeof ScanReply);

    ScanParams.Descriptor        = &ScanDescriptor;
    ScanParams.ScanReply         = &ScanReply;
    ScanReply.EngineScanCallback = EngineScanCallback;
    ScanReply.field_C            = 0x7fffffff;
    ScanDescriptor.Read          = ReadStream;
    ScanDescriptor.GetSize       = GetCustomStreamSize;
    ScanDescriptor.GetName       = GetStreamName;

    if (argc < 2) {
        LogMessage("usage: %s [filenames...]", *argv);
        return 1;
    }

    // Enable Instrumentation.
    InstrumentationCallback(image.image, image.size);
    char* signature = NULL;
    for (char *filename = *++argv; *argv; ++argv) {
        ScanDescriptor.UserPtr = fopen(*argv, "r");

        if (ScanDescriptor.UserPtr == NULL) {
            LogMessage("failed to open file %s", *argv);
            return 1;
        }

        LogMessage("Scanning %s...", *argv);
        GetStreamSize(ScanDescriptor.UserPtr, &subFileSize);
        LogMessage("size : %llu bytes",subFileSize);

        // cache the file size
        DWORD fileSize = subFileSize;
        while (true){
            ULONGLONG rightOffset = fileSize - 1ULL;
            subFileSize = rightOffset - leftOffset + 1;
            if (subFileSize <= 0){
                break;
            }
            fseek(ScanDescriptor.UserPtr, leftOffset, SEEK_SET);
            remainingSize = subFileSize;
            isResponsePositive = FALSE;
            if (__rsignal(&KernelHandle, RSIG_SCAN_STREAMBUFFER, &ScanParams, sizeof ScanParams) != 0) {
                LogMessage("__rsignal(RSIG_SCAN_STREAMBUFFER) returned failure, file unreadable?");
                return 1;
            }
            if (!isResponsePositive){
                //no further threaats
                break;
            }
            // binary search the right part
            // we need the minimum right part such that the response is positive
            ULONGLONG lowerRightOffset = leftOffset;
            ULONGLONG upperRightOffset = rightOffset;
            while (lowerRightOffset < upperRightOffset){
                fseek(ScanDescriptor.UserPtr, leftOffset, SEEK_SET);
                ULONGLONG middleRightOffset = lowerRightOffset + ((upperRightOffset - lowerRightOffset) >> 1ULL);
                subFileSize = middleRightOffset - leftOffset + 1;
                isResponsePositive = FALSE;
                remainingSize = subFileSize;
                if (__rsignal(&KernelHandle, RSIG_SCAN_STREAMBUFFER, &ScanParams, sizeof ScanParams) != 0) {
                    LogMessage("__rsignal(RSIG_SCAN_STREAMBUFFER) returned failure, file unreadable?");
                    return 1;
                }
                if (isResponsePositive) {
                    upperRightOffset = middleRightOffset;
                }
                else {
                    lowerRightOffset = middleRightOffset + 1ULL;
                }
            }
            rightOffset = upperRightOffset;

            //binary search the left part
            //we need the maximum left part such that the response is positive
            ULONGLONG lowerLeftOffset = leftOffset;
            ULONGLONG upperLeftOffset = rightOffset;
            while (lowerLeftOffset < upperLeftOffset){
                ULONGLONG middleLeftOffset = lowerLeftOffset + ((upperLeftOffset - lowerLeftOffset + 1) >> 1ULL);
                subFileSize = rightOffset - middleLeftOffset + 1;
                fseek(ScanDescriptor.UserPtr, middleLeftOffset, SEEK_SET);
                isResponsePositive = FALSE;
                remainingSize = subFileSize;
                if (__rsignal(&KernelHandle, RSIG_SCAN_STREAMBUFFER, &ScanParams, sizeof ScanParams) != 0) {
                    LogMessage("__rsignal(RSIG_SCAN_STREAMBUFFER) returned failure, file unreadable?");
                    return 1;
                }
                if (isResponsePositive){
                    lowerLeftOffset = middleLeftOffset;
                }
                else {
                    upperLeftOffset = middleLeftOffset - 1ULL;
                }
            }
            leftOffset = upperLeftOffset;
            subFileSize = rightOffset - leftOffset + 1;
            if (signature == NULL || strlen(signature) <= subFileSize) {
                if (signature != NULL) free(signature);
                signature = malloc((size_t) (1.5 * subFileSize)); //exponential growth to reduce heap allocation
            }
            fseek(ScanDescriptor.UserPtr, leftOffset, SEEK_SET);
            if (fread(signature, 1, subFileSize, ScanDescriptor.UserPtr));
            signature[subFileSize] = '\0';

            LogMessage("Threat %s identified.", lastVirusName);
            LogMessage("Signature found. Signature starts at offset %llu and ends at offset %llu", leftOffset, rightOffset);
            LogMessage("Size of signature is : %llu bytes", subFileSize);
            LogMessage("The signature is :");
            LogMessage("--------------------------------------------------------------------------");
            printf("%s\n", signature);
            LogMessage("--------------------------------------------------------------------------\n");
            //reset data for next iteration
            leftOffset = rightOffset + 1;
        }
        if (signature) free(signature);
        fclose(ScanDescriptor.UserPtr);
    }

    return 0;
}
