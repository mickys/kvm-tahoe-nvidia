// nvidia-smi for macOS on nullmoth's NVIDIA driver: the default nvidia-smi table, read from the GPU's resource manager
// (GSP-RM) through the NVRM/NVAccel user client, the same RM controls NVML uses on Linux.
//
//   nvidia-smi                 the table (GPU, temperature, P-state, memory, utilization, processes)
//   nvidia-smi -l [SEC]        repeat every SEC seconds (default 1)
//   nvidia-smi -L              list GPUs
//
// RM structs and control numbers come from NVIDIA open-gpu-kernel-modules 610.57.04 (the RM the driver runs).
// Fan speed and power draw are only reachable through NVIDIA's private controls, so they print N/A.
// Build: see build.sh next to this file.
#include <IOKit/IOKitLib.h>
#include <libproc.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nvtypes.h"
#include "nvos.h"
#include "nvstatus.h"
#include "nv_escape.h"
#include "nv-ioctl.h"
#include "class/cl0000.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrl0000/ctrl0000system.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"
#include "ctrl/ctrl2080/ctrl2080fb.h"
#include "ctrl/ctrl2080/ctrl2080perf.h"
#include "ctrl/ctrl2080/ctrl2080thermal.h"

enum { SEL_ESC = 1, SEL_BINDFD = 3 };
#define ADA_A          0xC997u   // 3D class: "G" processes
#define ADA_COMPUTE_A  0xC9C0u   // compute class: "C" processes

static io_connect_t gCtl, gDev;
static NvHandle hClient, hDevice = 0xcaf00001, hSubdev = 0xcaf00002;
static int gVerbose;

static io_connect_t open_conn(uint32_t type)
{
    // like NVK's nvrm_xnu_open: NVAccel's bridge (0x4E56xxxx), else NVRM itself
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

static int esc(io_connect_t conn, uint32_t cmd, void *data, size_t size)
{
    uint64_t in = cmd; size_t out = size;
    kern_return_t kr = IOConnectCallMethod(conn, SEL_ESC, &in, 1, data, size, NULL, NULL, data, &out);
    if (kr != KERN_SUCCESS && gVerbose) fprintf(stderr, "esc 0x%02x -> 0x%08x\n", cmd, kr);
    return kr == KERN_SUCCESS ? 0 : -1;
}

static NV_STATUS rm_alloc(NvHandle parent, NvHandle *obj, NvU32 cls, void *params)
{
    NVOS21_PARAMETERS p = { .hRoot = hClient, .hObjectParent = parent, .hObjectNew = *obj, .hClass = cls, .pAllocParms = params };
    if (esc(gCtl, NV_ESC_RM_ALLOC, &p, sizeof p)) return NV_ERR_GENERIC;
    *obj = p.hObjectNew;
    return p.status;
}

static NV_STATUS rm_ctrl(NvHandle obj, NvU32 cmd, void *params, NvU32 size)
{
    NVOS54_PARAMETERS p = { .hClient = hClient, .hObject = obj, .cmd = cmd, .params = params, .paramsSize = size };
    if (esc(gCtl, NV_ESC_RM_CONTROL, &p, sizeof p)) return NV_ERR_GENERIC;
    if (p.status && gVerbose) fprintf(stderr, "control 0x%08x -> status 0x%x\n", cmd, p.status);
    return p.status;
}

static void rm_close(void)
{
    if (hClient) { NVOS00_PARAMETERS p = { .hRoot = hClient, .hObjectParent = hClient, .hObjectOld = hClient }; esc(gCtl, NV_ESC_RM_FREE, &p, sizeof p); }
    if (gDev) IOServiceClose(gDev);
    if (gCtl) IOServiceClose(gCtl);
}

typedef struct {
    char name[64], busId[32], driver[64], rmVersion[64];
    int temp;                 // C, -1 = N/A
    int pstate;               // -1 = N/A
    unsigned long long memTotalMiB, memUsedMiB; int memOk;
    int util;                 // %, -1 = N/A
    int dispActive;
} Gpu;

typedef struct { unsigned pid; char type; unsigned long long memMiB; int memOk; } Proc;

static int rm_open(nv_ioctl_card_info_t *ci)
{
    gCtl = open_conn(0); gDev = open_conn(1);
    if (!gCtl || !gDev) { fprintf(stderr, "NVIDIA-SMI has failed because it couldn't communicate with the NVIDIA driver (no NVAccel/NVRM user client).\n"); return -1; }
    memset(ci, 0, sizeof *ci);
    if (esc(gCtl, NV_ESC_CARD_INFO, ci, sizeof *ci) || !ci->valid) { fprintf(stderr, "NVIDIA-SMI: card info failed (is the driver up?)\n"); return -1; }
    NvHandle h = 0;
    if (rm_alloc(0, &h, NV01_ROOT_CLIENT, NULL) != NV_OK) { fprintf(stderr, "NVIDIA-SMI: RM client alloc failed\n"); return -1; }
    hClient = h;
    NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id = { .gpuId = ci->gpu_id };
    if (rm_ctrl(hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &id, sizeof id) != NV_OK) { fprintf(stderr, "NVIDIA-SMI: GPU id info failed\n"); return -1; }
    NV0080_ALLOC_PARAMETERS a0 = { .deviceId = id.deviceInstance, .hClientShare = hClient };
    NV2080_ALLOC_PARAMETERS a2 = { .subDeviceId = id.subDeviceInstance };
    if (rm_alloc(hClient, &hDevice, NV01_DEVICE_0, &a0) != NV_OK || rm_alloc(hDevice, &hSubdev, NV20_SUBDEVICE_0, &a2) != NV_OK) {
        fprintf(stderr, "NVIDIA-SMI: device/subdevice alloc failed\n"); return -1;
    }
    return 0;
}

static int display_active(void)
{
    // a framebuffer the driver drives with a display attached: NVRMFB publishes IOFBOnline-style state on its framebuffer
    int on = 0; io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IODisplayConnect"), &it) == KERN_SUCCESS) {
        io_object_t o; while ((o = IOIteratorNext(it))) { on = 1; IOObjectRelease(o); }
        IOObjectRelease(it);
    }
    return on;
}

static void query(Gpu *g, const nv_ioctl_card_info_t *ci, Proc *procs, int *nproc)
{
    memset(g, 0, sizeof *g); g->temp = g->pstate = g->util = -1;

    NV2080_CTRL_GPU_GET_NAME_STRING_PARAMS nm = { .gpuNameStringFlags = NV2080_CTRL_GPU_GET_NAME_STRING_FLAGS_TYPE_ASCII };
    if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_GPU_GET_NAME_STRING, &nm, sizeof nm) == NV_OK)
        snprintf(g->name, sizeof g->name, "%s", (const char *)nm.gpuNameString.ascii);
    else snprintf(g->name, sizeof g->name, "NVIDIA GPU");

    snprintf(g->busId, sizeof g->busId, "%08X:%02X:%02X.%X", ci->pci_info.domain, ci->pci_info.bus, ci->pci_info.slot, ci->pci_info.function);

    NV0000_CTRL_SYSTEM_GET_BUILD_VERSION_V2_PARAMS bv; memset(&bv, 0, sizeof bv);
    if (rm_ctrl(hClient, NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION_V2, &bv, sizeof bv) == NV_OK) snprintf(g->rmVersion, sizeof g->rmVersion, "%s", bv.driverVersionBuffer);
    else snprintf(g->rmVersion, sizeof g->rmVersion, "N/A");
    // "Driver Version" = nullmoth's release (the RM inside it is the NVIDIA-SMI version)
    snprintf(g->driver, sizeof g->driver, "N/A");
    FILE *dv = fopen("/Library/NullMoth/driver-version", "r");
    if (dv) { if (fgets(g->driver, sizeof g->driver, dv)) g->driver[strcspn(g->driver, " \r\n")] = 0; fclose(dv); }

    // temperature: thermal "execute" with one sensor-reading instruction (sensor 0 = GPU core)
    static NV2080_CTRL_THERMAL_SYSTEM_EXECUTE_V2_PARAMS th; memset(&th, 0, sizeof th);
    th.clientAPIVersion = THERMAL_SYSTEM_API_VER; th.clientAPIRevision = THERMAL_SYSTEM_API_REV;
    th.clientInstructionSizeOf = sizeof(NV2080_CTRL_THERMAL_SYSTEM_INSTRUCTION);   // RM rejects the call without it
    th.instructionListSize = 1;
    th.instructionList[0].opcode = NV2080_CTRL_THERMAL_SYSTEM_GET_STATUS_SENSOR_READING_OPCODE;
    th.instructionList[0].operands.getStatusSensorReading.sensorIndex = 0;
    if ((rm_ctrl(hSubdev, NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2, &th, sizeof th) == NV_OK ||
         rm_ctrl(hSubdev, NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2_PHYSICAL, &th, sizeof th) == NV_OK) &&
        th.successfulInstructions == 1 && th.instructionList[0].executed && th.instructionList[0].result == NV_OK)
        g->temp = th.instructionList[0].operands.getStatusSensorReading.value;

    NV2080_CTRL_PERF_GET_CURRENT_PSTATE_PARAMS ps = {0};
    if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE, &ps, sizeof ps) == NV_OK && ps.currPstate)
        g->pstate = __builtin_ctz(ps.currPstate);

    NV2080_CTRL_FB_GET_INFO_V2_PARAMS fb; memset(&fb, 0, sizeof fb);
    fb.fbInfoListSize = 3;
    fb.fbInfoList[0].index = NV2080_CTRL_FB_INFO_INDEX_TOTAL_RAM_SIZE;   // KiB
    fb.fbInfoList[1].index = NV2080_CTRL_FB_INFO_INDEX_HEAP_SIZE;        // KiB
    fb.fbInfoList[2].index = NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE;        // KiB
    if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_FB_GET_INFO_V2, &fb, sizeof fb) == NV_OK) {
        unsigned long long total = fb.fbInfoList[0].data, heap = fb.fbInfoList[1].data, freeK = fb.fbInfoList[2].data;
        g->memTotalMiB = total / 1024;
        // used = everything not free in the heap, plus what RM reserves outside it (as nvidia-smi counts "reserved" in used)
        g->memUsedMiB = (total - freeK) / 1024;
        (void)heap; g->memOk = 1;
    }

    static NV2080_CTRL_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2_PARAMS us; memset(&us, 0, sizeof us);
    us.type = NV2080_CTRL_GPUMON_SAMPLE_TYPE_PERFMON_UTIL;
    us.bufSize = sizeof us.samples; us.tracker = 0;
    if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2, &us, sizeof us) == NV_OK) {
        NvU64 newest = 0; int best = -1;
        for (int i = 0; i < NV2080_CTRL_PERF_GPUMON_SAMPLE_COUNT_PERFMON_UTIL; i++)
            if (us.samples[i].base.timeStamp > newest) { newest = us.samples[i].base.timeStamp; best = i; }
        if (best >= 0) g->util = (int)(us.samples[best].gr.util / 100);
        else g->util = 0;
    }

    g->dispActive = display_active();

    // processes: PIDs that own 3D / compute objects, and their video memory
    *nproc = 0;
    const struct { NvU32 cls; char type; } kinds[] = { { ADA_A, 'G' }, { ADA_COMPUTE_A, 'C' } };
    for (unsigned k = 0; k < 2; k++) {
        static NV2080_CTRL_GPU_GET_PIDS_PARAMS pp; memset(&pp, 0, sizeof pp);
        pp.idType = NV2080_CTRL_GPU_GET_PIDS_ID_TYPE_CLASS; pp.id = kinds[k].cls;
        if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_GPU_GET_PIDS, &pp, sizeof pp) != NV_OK) continue;
        for (NvU32 i = 0; i < pp.pidTblCount && *nproc < 256; i++) {
            int dup = 0;
            for (int j = 0; j < *nproc; j++) if (procs[j].pid == pp.pidTbl[i]) { if (kinds[k].type == 'C') procs[j].type = 'X'; dup = 1; }
            if (!dup) procs[(*nproc)++] = (Proc){ .pid = pp.pidTbl[i], .type = kinds[k].type };
        }
    }
    for (int base = 0; base < *nproc; base += NV2080_CTRL_GPU_GET_PID_INFO_MAX_COUNT) {
        static NV2080_CTRL_GPU_GET_PID_INFO_PARAMS pi; memset(&pi, 0, sizeof pi);
        int n = *nproc - base; if (n > NV2080_CTRL_GPU_GET_PID_INFO_MAX_COUNT) n = NV2080_CTRL_GPU_GET_PID_INFO_MAX_COUNT;
        pi.pidInfoListCount = n;
        for (int i = 0; i < n; i++) { pi.pidInfoList[i].pid = procs[base + i].pid; pi.pidInfoList[i].index = NV2080_CTRL_GPU_PID_INFO_INDEX_VIDEO_MEMORY_USAGE; }
        if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_GPU_GET_PID_INFO, &pi, sizeof pi) != NV_OK) continue;
        for (int i = 0; i < n; i++) if (pi.pidInfoList[i].result == NV_OK) {
            const NV2080_CTRL_GPU_PID_INFO_VIDEO_MEMORY_USAGE_DATA *v = &pi.pidInfoList[i].data.vidMemUsage;
            procs[base + i].memMiB = (v->memPrivate + v->memSharedOwned) >> 20; procs[base + i].memOk = 1;
        }
    }
}

// one table cell: left text, right text right-aligned, padded to width (the width includes one trailing space)
static void cell(char *out, int width, const char *left, const char *right)
{
    int l = (int)strlen(left), r = (int)strlen(right), gap = width - 1 - l - r;
    if (gap < 1) gap = 1;
    sprintf(out, "%s%*s%s ", left, gap, "", right);
}

static void print_table(const Gpu *g, const Proc *procs, int nproc)
{
    time_t t = time(NULL); char ts[64]; strftime(ts, sizeof ts, "%a %b %e %H:%M:%S %Y", localtime(&t));
    printf("%s       \n", ts);
    const char *bar  = "+-----------------------------------------+------------------------+----------------------+";
    const char *bar2 = "|-----------------------------------------+------------------------+----------------------+";
    const char *dbl  = "|=========================================+========================+======================|";
    printf("+-----------------------------------------------------------------------------------------+\n");
    char hdr[128]; snprintf(hdr, sizeof hdr, "NVIDIA-SMI %-22s Driver Version: %-14s CUDA Version: N/A", g->rmVersion, g->driver);
    printf("| %-87s |\n", hdr);
    printf("%s\n", bar2);
    printf("| GPU  Name                 Persistence-M | Bus-Id          Disp.A | Volatile Uncorr. ECC |\n");
    printf("| Fan  Temp   Perf          Pwr:Usage/Cap |           Memory-Usage | GPU-Util  Compute M. |\n");
    printf("|                                         |                        |               MIG M. |\n");
    printf("%s\n", dbl);

    char a[64], b[64], c[64], l[96], m[64], r[64];
    snprintf(a, sizeof a, "%4d  %.26s", 0, g->name);
    cell(l, 41, a, "N/A");
    snprintf(a, sizeof a, "   %s", g->busId); cell(m, 24, a, g->dispActive ? "On" : "Off");
    cell(r, 22, "", "N/A");
    printf("|%s|%s|%s|\n", l, m, r);

    char temp[16], perf[16];
    if (g->temp >= 0) snprintf(temp, sizeof temp, "%dC", g->temp); else snprintf(temp, sizeof temp, "N/A");
    if (g->pstate >= 0) snprintf(perf, sizeof perf, "P%d", g->pstate); else snprintf(perf, sizeof perf, "N/A");
    snprintf(a, sizeof a, "%4s  %5s    %-4s", "N/A", temp, perf); cell(l, 41, a, "N/A /  N/A");
    if (g->memOk) snprintf(b, sizeof b, "%lluMiB / %6lluMiB", g->memUsedMiB, g->memTotalMiB); else snprintf(b, sizeof b, "N/A / N/A");
    cell(m, 24, "", b);
    if (g->util >= 0) snprintf(c, sizeof c, "%8d%%", g->util); else snprintf(c, sizeof c, "%9s", "N/A");
    cell(r, 22, c, "Default");
    printf("|%s|%s|%s|\n", l, m, r);
    cell(l, 41, "", ""); cell(m, 24, "", ""); cell(r, 22, "", "N/A");
    printf("|%s|%s|%s|\n", l, m, r);
    printf("%s\n", bar);
    printf("%91s\n", "");

    printf("+-----------------------------------------------------------------------------------------+\n");
    printf("| Processes:                                                                              |\n");
    printf("|  GPU   GI   CI        PID   Type   Process name                              GPU Memory |\n");
    printf("|        ID   ID                                                               Usage      |\n");
    printf("|=========================================================================================|\n");
    if (!nproc) printf("|  No running processes found                                                             |\n");
    for (int i = 0; i < nproc; i++) {
        char path[PROC_PIDPATHINFO_MAXSIZE] = "", name[44], mem[16], type[4];
        if (proc_pidpath((int)procs[i].pid, path, sizeof path) <= 0) proc_name((int)procs[i].pid, path, sizeof path);
        if (!path[0]) snprintf(path, sizeof path, "[pid %u]", procs[i].pid);
        size_t n = strlen(path);
        if (n > 39) snprintf(name, sizeof name, "...%s", path + n - 36); else snprintf(name, sizeof name, "%s", path);
        if (procs[i].memOk) snprintf(mem, sizeof mem, "%lluMiB", procs[i].memMiB); else snprintf(mem, sizeof mem, "N/A");
        if (procs[i].type == 'X') snprintf(type, sizeof type, "C+G"); else snprintf(type, sizeof type, "%c", procs[i].type);
        printf("|  %3d   N/A  N/A  %9u  %5s   %-39s %10s |\n", 0, procs[i].pid, type, name, mem);
    }
    printf("+-----------------------------------------------------------------------------------------+\n");
}

int main(int argc, char **argv)
{
    int loop = 0, list = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-L") || !strcmp(argv[i], "--list-gpus")) list = 1;
        else if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--loop")) { loop = 1; if (i + 1 < argc && atoi(argv[i + 1]) > 0) loop = atoi(argv[++i]); }
        else if (!strncmp(argv[i], "--loop=", 7)) loop = atoi(argv[i] + 7) > 0 ? atoi(argv[i] + 7) : 1;
        else if (!strcmp(argv[i], "-v")) gVerbose = 1;
        else { fprintf(stderr, "usage: nvidia-smi [-L] [-l [SEC]] [-v]\n"); return 2; }
    }
    nv_ioctl_card_info_t ci;
    if (rm_open(&ci)) { rm_close(); return 9; }
    static Proc procs[256]; int nproc; Gpu g;
    if (list) {
        query(&g, &ci, procs, &nproc);
        printf("GPU 0: %s (Bus-Id %s)\n", g.name, g.busId);
    } else do {
        query(&g, &ci, procs, &nproc);
        if (loop) printf("\033[H\033[2J");
        print_table(&g, procs, nproc);
        fflush(stdout);
        if (loop) sleep(loop);
    } while (loop);
    rm_close();
    return 0;
}
