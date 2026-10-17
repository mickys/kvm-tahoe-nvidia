// rm.h: a minimal NVIDIA RM client for macOS on nullmoth's driver. It talks to GSP-RM through the NVAccel/NVRM user
// client (the escape interface NVK uses): IOServiceOpen type 0x4E560000|0 (control) / |1 (device), selector 3 binds
// an fd number, selector 1 = RM escape (NV_ESC_RM_ALLOC / CONTROL / FREE ...). Structures: NVIDIA
// open-gpu-kernel-modules (the RM version the driver runs).
#pragma once
#include <IOKit/IOKitLib.h>
#include <stdbool.h>
#include "nvtypes.h"
#include "nvos.h"
#include "nvstatus.h"
#include "nv-ioctl.h"

typedef struct {
    io_connect_t ctl, dev;            // control and device user clients
    NvHandle client, device, subdevice;
    nv_ioctl_card_info_t card;
    NvU32 nextHandle;                 // handles for objects we allocate
    bool verbose;
} Rm;

int  rm_open(Rm *rm);                                                    // client + device + subdevice; 0 = ok
void rm_close(Rm *rm);
NvHandle rm_handle(Rm *rm);                                              // a fresh object handle
NV_STATUS rm_alloc(Rm *rm, NvHandle parent, NvHandle obj, NvU32 cls, void *params);
NV_STATUS rm_free(Rm *rm, NvHandle parent, NvHandle obj);
NV_STATUS rm_ctrl(Rm *rm, NvHandle obj, NvU32 cmd, void *params, NvU32 size);
int rm_esc(Rm *rm, io_connect_t conn, NvU32 cmd, void *data, size_t size);
