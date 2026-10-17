// rm.c: see rm.h.
#include "rm.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "nv_escape.h"
#include "class/cl0000.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"

enum { SEL_ESC = 1, SEL_BINDFD = 3 };

static io_connect_t open_conn(NvU32 type)
{
    // NVAccel's bridge first (0x4E56xxxx), then NVRM itself, as NVK's nvrm_xnu_open does
    io_connect_t conn = 0; kern_return_t kr = KERN_FAILURE;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVAccel"));
    if (svc) { kr = IOServiceOpen(svc, mach_task_self(), 0x4E560000u | type, &conn); IOObjectRelease(svc); }
    if (kr != KERN_SUCCESS) {
        svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVRM"));
        if (!svc) return 0;
        kr = IOServiceOpen(svc, mach_task_self(), type, &conn); IOObjectRelease(svc);
        if (kr != KERN_SUCCESS) return 0;
    }
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);   // the user client keys its RM file on an fd number
    uint64_t in = (uint64_t)fd;
    IOConnectCallScalarMethod(conn, SEL_BINDFD, &in, 1, NULL, NULL);
    return conn;
}

int rm_esc(Rm *rm, io_connect_t conn, NvU32 cmd, void *data, size_t size)
{
    uint64_t in = cmd; size_t out = size;
    kern_return_t kr = IOConnectCallMethod(conn, SEL_ESC, &in, 1, data, size, NULL, NULL, data, &out);
    if (kr != KERN_SUCCESS && rm->verbose) fprintf(stderr, "rm: esc 0x%02x -> 0x%08x\n", cmd, kr);
    return kr == KERN_SUCCESS ? 0 : -1;
}

NvHandle rm_handle(Rm *rm) { return 0xcaf00000u + rm->nextHandle++; }

NV_STATUS rm_alloc(Rm *rm, NvHandle parent, NvHandle obj, NvU32 cls, void *params)
{
    NVOS21_PARAMETERS p = { .hRoot = rm->client, .hObjectParent = parent, .hObjectNew = obj, .hClass = cls, .pAllocParms = params };
    if (rm_esc(rm, rm->ctl, NV_ESC_RM_ALLOC, &p, sizeof p)) return NV_ERR_GENERIC;
    if (p.status && rm->verbose) fprintf(stderr, "rm: alloc class 0x%04x under 0x%08x -> 0x%x\n", cls, parent, p.status);
    return p.status;
}

NV_STATUS rm_free(Rm *rm, NvHandle parent, NvHandle obj)
{
    NVOS00_PARAMETERS p = { .hRoot = rm->client, .hObjectParent = parent, .hObjectOld = obj };
    if (rm_esc(rm, rm->ctl, NV_ESC_RM_FREE, &p, sizeof p)) return NV_ERR_GENERIC;
    return p.status;
}

NV_STATUS rm_ctrl(Rm *rm, NvHandle obj, NvU32 cmd, void *params, NvU32 size)
{
    NVOS54_PARAMETERS p = { .hClient = rm->client, .hObject = obj, .cmd = cmd, .params = params, .paramsSize = size };
    if (rm_esc(rm, rm->ctl, NV_ESC_RM_CONTROL, &p, sizeof p)) return NV_ERR_GENERIC;
    if (p.status && rm->verbose) fprintf(stderr, "rm: control 0x%08x -> 0x%x\n", cmd, p.status);
    return p.status;
}

int rm_open(Rm *rm)
{
    bool v = rm->verbose; memset(rm, 0, sizeof *rm); rm->verbose = v;
    rm->ctl = open_conn(0); rm->dev = open_conn(1);
    if (!rm->ctl || !rm->dev) { fprintf(stderr, "rm: no NVAccel/NVRM user client\n"); return -1; }
    if (rm_esc(rm, rm->ctl, NV_ESC_CARD_INFO, &rm->card, sizeof rm->card) || !rm->card.valid) { fprintf(stderr, "rm: card info failed\n"); return -1; }
    NVOS21_PARAMETERS p = { .hClass = NV01_ROOT_CLIENT };
    if (rm_esc(rm, rm->ctl, NV_ESC_RM_ALLOC, &p, sizeof p) || p.status) { fprintf(stderr, "rm: client alloc failed\n"); return -1; }
    rm->client = p.hObjectNew;
    NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id = { .gpuId = rm->card.gpu_id };
    if (rm_ctrl(rm, rm->client, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &id, sizeof id)) return -1;
    rm->device = rm_handle(rm); rm->subdevice = rm_handle(rm);
    NV0080_ALLOC_PARAMETERS a0 = { .deviceId = id.deviceInstance, .hClientShare = rm->client };
    NV2080_ALLOC_PARAMETERS a2 = { .subDeviceId = id.subDeviceInstance };
    if (rm_alloc(rm, rm->client, rm->device, NV01_DEVICE_0, &a0) || rm_alloc(rm, rm->device, rm->subdevice, NV20_SUBDEVICE_0, &a2)) return -1;
    return 0;
}

void rm_close(Rm *rm)
{
    if (rm->client) { NVOS00_PARAMETERS p = { .hRoot = rm->client, .hObjectParent = rm->client, .hObjectOld = rm->client }; rm_esc(rm, rm->ctl, NV_ESC_RM_FREE, &p, sizeof p); }
    if (rm->dev) IOServiceClose(rm->dev);
    if (rm->ctl) IOServiceClose(rm->ctl);
}
