// nvidia-smi for macOS on NullMoth's NVIDIA driver: NVIDIA's nvidia-smi output, read-only, from the GPU's resource manager
// (GSP-RM) through the NVRM/NVAccel user client (the RM controls NVML uses on Linux) and the IORegistry.
//
//   nvidia-smi                          the default table (GPU, temperature, P-state, memory, utilization, processes)
//   nvidia-smi -L                       list GPUs with their UUID
//   nvidia-smi --list                   every value read, one "key : value" per line
//   nvidia-smi -q [-d SECTIONS]         the full query (sections: MEMORY,UTILIZATION,ENCODER_STATS,TEMPERATURE,POWER,CLOCK,
//                                       PERFORMANCE,PCI,PIDS; default all)
//   nvidia-smi --query-gpu=F,F,... --format=csv[,noheader][,nounits]    selected fields (--help-query-gpu lists them)
//   nvidia-smi dmon [-c N]              one line per second: temperature, sm/mem/enc/dec %, clocks
//   nvidia-smi -l [SEC] | -lms [MS]     repeat (table, -q or --query-gpu)
//   -v                                  print RM controls that failed (stderr)
//
// RM structs and control numbers: NVIDIA open-gpu-kernel-modules 615.78.08 (the RM of NullMoth 2.5.x).
// Power draw, power limits, fan RPM and the board / memory temperatures come from the RM's user shared data page (class
// RM_USER_SHARED_DATA, 0x00de, open header class/cl00de.h): allocated on the subdevice with a polling mask, the RM keeps it
// refreshed and nvidia-smi maps it read-only, as NVIDIA's NVML does. The fan duty % and the default/min/max power limits are
// not on that page; they come from NVIDIA's closed legacy thermal/pmgr RM controls (0x2080_852f cooler status, 0x2080_a618
// power-domain table), probed on RM 615.78.08. They are not in the open SDK headers, so NullMoth's NVRM may not dispatch
// them; then fan % falls back to an RPM/max estimate and those limits print N/A. Everything degrades, nothing breaks.
// Build: build.sh next to this file.
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <libproc.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t gRunning = 1;
static void sig_stop(int s) { (void)s; gRunning = 0; }

#include "nvtypes.h"
#include "nvos.h"
#include "nvstatus.h"
#include "nv_escape.h"
#include "nv-ioctl.h"
#include "nv-unix-nvos-params-wrappers.h"
#include "class/cl0000.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "class/cl00de.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrl0000/ctrl0000system.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"
#include "ctrl/ctrl2080/ctrl2080fb.h"
#include "ctrl/ctrl2080/ctrl2080perf.h"
#include "ctrl/ctrl2080/ctrl2080thermal.h"
#include "ctrl/ctrl2080/ctrl2080bus.h"
#include "ctrl/ctrl2080/ctrl2080bios.h"
#include "ctrl/ctrl2080/ctrl2080clk.h"

enum { SEL_ESC = 1, SEL_BINDFD = 3 };
static io_connect_t gCtl, gDev;
static NvHandle hClient, hDevice = 0xcaf00001, hSubdev = 0xcaf00002;
static int gVerbose;
static int gFanMaxRpm;   // --fan-max-rpm N or NVSMI_FAN_MAX_RPM: enables fan.speed % = round(rpm / max * 100), clamped 0..100

// ---- RM client --------------------------------------------------------------------------------------------------------
static io_connect_t open_conn(uint32_t type, int *fdOut)
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
    if (fdOut) *fdOut = fd;
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
static const char *nv_err_name(NV_STATUS s)
{
    switch (s) {
    case 0x1f: return "INVALID_ARGUMENT"; case 0x56: return "NOT_SUPPORTED"; case 0x57: return "OBJECT_NOT_FOUND";
    case 0x31: return "INVALID_STATE"; case 0x25: return "INVALID_OBJECT"; case 0x48: return "TIMEOUT";
    default: return NULL;
    }
}
static NV_STATUS rm_ctrl(NvHandle obj, NvU32 cmd, void *params, NvU32 size)
{
    NVOS54_PARAMETERS p = { .hClient = hClient, .hObject = obj, .cmd = cmd, .params = params, .paramsSize = size };
    if (esc(gCtl, NV_ESC_RM_CONTROL, &p, sizeof p)) return NV_ERR_GENERIC;
    if (p.status && gVerbose) { const char *n = nv_err_name(p.status); fprintf(stderr, "control 0x%08x -> 0x%x%s%s%s\n", cmd, p.status, n ? " (" : "", n ? n : "", n ? ")" : ""); }
    return p.status;
}
// ---- RM user shared data (class 0x00de) -------------------------------------------------------------------------------
static NvHandle hRusd = 0xcaf00003;
static io_connect_t gRusdConn;
static NV00DE_SHARED_DATA *gRusd;
// allocate the page on the subdevice with a polling mask, map it (RM_MAP_MEMORY on the control connection names a second
// connection by its fd, whose memory type 0 carries the mapping), then wait for the first polled sample
static void rusd_open(void)
{
    NV00DE_ALLOC_PARAMETERS a = { .polledDataMask = NV00DE_RUSD_POLL_POWER | NV00DE_RUSD_POLL_THERMAL | NV00DE_RUSD_POLL_FAN };
    NvHandle h = hRusd;
    if (rm_alloc(hSubdev, &h, RM_USER_SHARED_DATA, &a) != NV_OK) { if (gVerbose) fprintf(stderr, "RUSD: alloc failed\n"); return; }
    hRusd = h;
    int fd = -1; gRusdConn = open_conn(0, &fd);
    if (!gRusdConn) return;
    const NvHandle parents[2] = { hSubdev, hDevice };
    for (int i = 0; i < 2 && !gRusd; i++) {
        nv_ioctl_nvos33_parameters_with_fd m = { .params = { .hClient = hClient, .hDevice = parents[i], .hMemory = hRusd, .offset = 0,
                                                             .length = sizeof(NV00DE_SHARED_DATA) }, .fd = fd };
        if (esc(gCtl, NV_ESC_RM_MAP_MEMORY, &m, sizeof m) || m.params.status) { if (gVerbose) fprintf(stderr, "RUSD: map via 0x%08x -> 0x%x\n", parents[i], m.params.status); continue; }
        mach_vm_address_t addr = 0; mach_vm_size_t len = 0;
        if (IOConnectMapMemory64(gRusdConn, 0, mach_task_self(), &addr, &len, kIOMapAnywhere) == KERN_SUCCESS && len >= sizeof(NV00DE_SHARED_DATA))
            gRusd = (NV00DE_SHARED_DATA *)(uintptr_t)addr;
        else if (gVerbose) fprintf(stderr, "RUSD: IOConnectMapMemory64 failed\n");
    }
    for (int i = 0; gRusd && i < 40 && !RusdIsSeqValid(gRusd->instPowerUsage.lastModifiedTimestamp); i++) usleep(50000);   // first poll
}

static void rm_close(void)
{
    if (gRusdConn) IOServiceClose(gRusdConn);
    if (hClient) { NVOS00_PARAMETERS p = { .hRoot = hClient, .hObjectParent = hClient, .hObjectOld = hClient }; esc(gCtl, NV_ESC_RM_FREE, &p, sizeof p); }
    if (gDev) IOServiceClose(gDev);
    if (gCtl) IOServiceClose(gCtl);
}
static int rm_open(nv_ioctl_card_info_t *ci)
{
    gCtl = open_conn(0, NULL); gDev = open_conn(1, NULL);
    if (!gCtl || !gDev) { fprintf(stderr, "NVIDIA-SMI has failed because it couldn't communicate with the NVIDIA driver. Make sure that the latest NVIDIA driver is installed and running.\n"); return -1; }
    memset(ci, 0, sizeof *ci);
    if (esc(gCtl, NV_ESC_CARD_INFO, ci, sizeof *ci) || !ci->valid) { fprintf(stderr, "No devices were found\n"); return -1; }
    NvHandle h = 0;
    if (rm_alloc(0, &h, NV01_ROOT_CLIENT, NULL) != NV_OK) { fprintf(stderr, "NVIDIA-SMI: RM client allocation failed\n"); return -1; }
    hClient = h;
    NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id = { .gpuId = ci->gpu_id };
    if (rm_ctrl(hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &id, sizeof id) != NV_OK) { fprintf(stderr, "NVIDIA-SMI: GPU id info failed\n"); return -1; }
    NV0080_ALLOC_PARAMETERS a0 = { .deviceId = id.deviceInstance, .hClientShare = hClient };
    NV2080_ALLOC_PARAMETERS a2 = { .subDeviceId = id.subDeviceInstance };
    if (rm_alloc(hClient, &hDevice, NV01_DEVICE_0, &a0) != NV_OK || rm_alloc(hDevice, &hSubdev, NV20_SUBDEVICE_0, &a2) != NV_OK) {
        fprintf(stderr, "NVIDIA-SMI: device/subdevice allocation failed\n"); return -1;
    }
    rusd_open();
    return 0;
}

// ---- what is read -----------------------------------------------------------------------------------------------------
#define NA (-1)
typedef struct { unsigned pid; char type; long long memMiB; } Proc;
typedef struct {
    char name[96], uuid[64], vbios[32], busId[32], driver[64], rmVersion[64], arch[32], serial[20], partNumber[24];
    unsigned devId;
    int temp, tempIoreg, pstate, dispActive;
    long long memTotal, memUsed, memFree, memReserved, bar1Total, bar1Used, busWidth;   // MiB / bits, NA if unknown
    int utilGpu, utilMem, utilEnc, utilDec, utilJpg, utilOfa;                          // %
    int clkGr, clkMem, clkVid;                                                         // MHz (current)
    int clkGrMax, clkMemMax, clkVidMax, clkGrDef, clkMemDef;                            // MHz (max / default from GET_LEVEL_INFO)
    int pcieGenCur, pcieGenMax, pcieWidthCur, pcieWidthMax;
    int encSessions, encFps, encLatencyUs;
    long long powerInst, powerAvg, powerLimitReq, powerLimitEnf, powerLimitDef, powerLimitMin, powerLimitMax;   // mW, NA if unknown
    int fanRpm[RUSD_FAN_COOLER_MAX_COOLERS], nFan, tempMem, tempBoard;   // fanDuty: real % from the cooler-status control; fanPct: estimate from fan 0 RPM / gFanMaxRpm
    int fanDuty, fanPct;
    Proc procs[256]; int nproc;
} Gpu;

static const char *arch_of(unsigned dev)
{
    if (dev >= 0x2b00) return "Blackwell"; if (dev >= 0x2600) return "Ada Lovelace"; if (dev >= 0x2300) return "Hopper";
    if (dev >= 0x2000) return "Ampere"; if (dev >= 0x1e00) return "Turing"; if (dev >= 0x1d80) return "Volta";
    return "Pascal";
}
// a PCIe "speed" code (1 = 2.5 GT/s ... 6 = 64 GT/s) is the generation number
static int pcie_gen(unsigned speed) { return speed >= 1 && speed <= 6 ? (int)speed : NA; }

static int ioreg_temperature(void)
{
    int t = NA; io_iterator_t it; io_object_t o;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &it) != KERN_SUCCESS) return NA;
    while ((o = IOIteratorNext(it))) {
        CFTypeRef ps = IORegistryEntryCreateCFProperty(o, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
        if (ps && CFGetTypeID(ps) == CFDictionaryGetTypeID()) {
            CFTypeRef v = CFDictionaryGetValue(ps, CFSTR("Temperature(C)"));
            int x; if (v && CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue(v, kCFNumberIntType, &x) && x > 0) t = x;
        }
        if (ps) CFRelease(ps);
        IOObjectRelease(o);
    }
    IOObjectRelease(it);
    return t;
}
static int display_active(void)
{
    int on = 0; io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IODisplayConnect"), &it) == KERN_SUCCESS) {
        io_object_t o; while ((o = IOIteratorNext(it))) { on = 1; IOObjectRelease(o); }
        IOObjectRelease(it);
    }
    return on;
}

// ---- NVIDIA's closed legacy thermal / pmgr controls (probed on RM 615.78.08, nvsmi-probe.c) ----------------------------
// Not in the open SDK headers. If NVRM does not dispatch a control it returns an error and we keep whatever the open path
// already filled in. Sizes are exact (RM checks paramsSize == ctrlParamsSize).
#define NVSMI_CTRL_COOLER_STATUS      0x2080852fu   // 844 B cooler status; in: dword[1] = cooler mask; out (cooler 0): dword[5] RPM, dword[8] level (16.16, 0x10000 = 100 %)
#define NVSMI_CTRL_PMGR_POWER_TABLE   0x2080a618u   // 20512 B power-domain table; 65-dword records, type at [+51], id at [+52], {min,def,max} mW at [+53..+55]

// fan 0's level in %, as NVIDIA's fan.speed (RTX 4090 idle: 0x543d = 32.9 % -> 33 %, the same as NVIDIA's Linux driver)
static int fan_duty(void)
{
    static NvU32 b[844 / 4];
    memset(b, 0, sizeof b);
    b[1] = 1;   // without a mask the RM answers OK with an empty table
    NV_STATUS st = rm_ctrl(hSubdev, NVSMI_CTRL_COOLER_STATUS, b, sizeof b);
    if (st != NV_OK) { if (gVerbose) fprintf(stderr, "nvidia-smi: ctrl 0x%08x (cooler status) -> 0x%x\n", NVSMI_CTRL_COOLER_STATUS, st); return NA; }
    if (!(b[1] & 1) || b[7] != 0x10000u || b[8] > 0x10000u) { if (gVerbose) fprintf(stderr, "nvidia-smi: cooler status: unexpected layout\n"); return NA; }
    return (int)(((unsigned long long)b[8] * 100 + 0x8000) >> 16);
}

static void pmgr_power_limits(Gpu *g)
{
    static NvU32 t[20512 / 4];
    memset(t, 0, sizeof t);
    NV_STATUS st = rm_ctrl(hSubdev, NVSMI_CTRL_PMGR_POWER_TABLE, t, sizeof t);
    if (st != NV_OK) { if (gVerbose) fprintf(stderr, "nvidia-smi: ctrl 0x%08x (pmgr power table) -> 0x%x\n", NVSMI_CTRL_PMGR_POWER_TABLE, st); return; }
    for (unsigned r = 0; r + 65 <= sizeof t / 4; r += 65) {
        NvU32 type = t[r + 51], id = t[r + 52];
        if (type != 0x30u || id != 0u) continue;
        NvU32 mn = t[r + 53], def = t[r + 54], mx = t[r + 55];
        if (mn < 1000 || mn > 1000000 || def < 1000 || def > 1000000 || mx < 1000 || mx > 1000000 || mn > def || def > mx) continue;
        g->powerLimitMin = mn; g->powerLimitDef = def; g->powerLimitMax = mx;
        return;
    }
    if (gVerbose) fprintf(stderr, "nvidia-smi: ctrl 0x%08x (pmgr power table): no id-0 power-limit entry\n", NVSMI_CTRL_PMGR_POWER_TABLE);
}

static void query(Gpu *g, const nv_ioctl_card_info_t *ci)
{
    memset(g, 0, sizeof *g);
    g->temp = g->tempIoreg = g->pstate = NA;
    g->memTotal = g->memUsed = g->memFree = g->memReserved = g->bar1Total = g->bar1Used = g->busWidth = NA;
    g->utilGpu = g->utilMem = g->utilEnc = g->utilDec = g->utilJpg = g->utilOfa = NA;
    g->clkGr = g->clkMem = g->clkVid = NA; g->clkGrMax = g->clkMemMax = g->clkVidMax = g->clkGrDef = g->clkMemDef = NA;
    g->pcieGenCur = g->pcieGenMax = g->pcieWidthCur = g->pcieWidthMax = NA;
    g->encSessions = g->encFps = g->encLatencyUs = NA;
    g->powerInst = g->powerAvg = g->powerLimitReq = g->powerLimitEnf = NA;
    g->powerLimitDef = g->powerLimitMin = g->powerLimitMax = NA; g->tempMem = g->tempBoard = g->fanDuty = g->fanPct = NA;

    g->devId = ci->pci_info.device_id;
    snprintf(g->arch, sizeof g->arch, "%s", arch_of(g->devId));
    snprintf(g->busId, sizeof g->busId, "%08X:%02X:%02X.%X", ci->pci_info.domain, ci->pci_info.bus, ci->pci_info.slot, ci->pci_info.function);

    NV2080_CTRL_GPU_GET_NAME_STRING_PARAMS nm = { .gpuNameStringFlags = NV2080_CTRL_GPU_GET_NAME_STRING_FLAGS_TYPE_ASCII };
    if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_GPU_GET_NAME_STRING, &nm, sizeof nm) == NV_OK) snprintf(g->name, sizeof g->name, "%s", (const char *)nm.gpuNameString.ascii);
    else snprintf(g->name, sizeof g->name, "NVIDIA GPU");

    static NV2080_CTRL_GPU_GET_GID_INFO_PARAMS gid; memset(&gid, 0, sizeof gid); gid.flags = NV2080_GPU_CMD_GPU_GET_GID_FLAGS_FORMAT_ASCII;
    if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_GPU_GET_GID_INFO, &gid, sizeof gid) == NV_OK) snprintf(g->uuid, sizeof g->uuid, "%.*s", (int)gid.length, (char *)gid.data);
    else snprintf(g->uuid, sizeof g->uuid, "N/A");

    static NV2080_CTRL_BIOS_GET_INFO_V2_PARAMS bi; memset(&bi, 0, sizeof bi); bi.biosInfoListSize = 2;
    bi.biosInfoList[0].index = NV2080_CTRL_BIOS_INFO_INDEX_REVISION; bi.biosInfoList[1].index = NV2080_CTRL_BIOS_INFO_INDEX_OEM_REVISION;
    if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_BIOS_GET_INFO_V2, &bi, sizeof bi) == NV_OK) {
        NvU32 r = bi.biosInfoList[0].data;   // 0x95023c80 + oem 0x9a = 95.02.3C.80.9A, as NVIDIA prints it
        snprintf(g->vbios, sizeof g->vbios, "%02X.%02X.%02X.%02X.%02X", r >> 24, (r >> 16) & 0xff, (r >> 8) & 0xff, r & 0xff, bi.biosInfoList[1].data & 0xff);
    } else snprintf(g->vbios, sizeof g->vbios, "N/A");

    NV2080_CTRL_GPU_GET_OEM_BOARD_INFO_PARAMS oem; memset(&oem, 0, sizeof oem);
    if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_GPU_GET_OEM_BOARD_INFO, &oem, sizeof oem) == NV_OK) {
        snprintf(g->serial, sizeof g->serial, "%.*s", (int)sizeof oem.serialNumber, (char *)oem.serialNumber);
        snprintf(g->partNumber, sizeof g->partNumber, "%.*s", (int)sizeof oem.productPartNumber, (char *)oem.productPartNumber);
    }

    NV0000_CTRL_SYSTEM_GET_BUILD_VERSION_V2_PARAMS bv; memset(&bv, 0, sizeof bv);
    if (rm_ctrl(hClient, NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION_V2, &bv, sizeof bv) == NV_OK) snprintf(g->rmVersion, sizeof g->rmVersion, "%s", bv.driverVersionBuffer);
    else snprintf(g->rmVersion, sizeof g->rmVersion, "N/A");
    snprintf(g->driver, sizeof g->driver, "N/A");   // "Driver Version" = NullMoth's release; the RM inside it is the NVIDIA-SMI version
    FILE *dv = fopen("/Library/NullMoth/driver-version", "r");
    if (dv) { if (fgets(g->driver, sizeof g->driver, dv)) g->driver[strcspn(g->driver, " \r\n")] = 0; fclose(dv); }

    // temperature: the RM's thermal "execute" (sensor 0 = GPU core), and NVAccel's PerformanceStatistics as a fallback
    static NV2080_CTRL_THERMAL_SYSTEM_EXECUTE_V2_PARAMS th; memset(&th, 0, sizeof th);
    th.clientAPIVersion = THERMAL_SYSTEM_API_VER; th.clientAPIRevision = THERMAL_SYSTEM_API_REV;
    th.clientInstructionSizeOf = sizeof(NV2080_CTRL_THERMAL_SYSTEM_INSTRUCTION);   // RM rejects the call without it
    th.instructionListSize = 1;
    th.instructionList[0].opcode = NV2080_CTRL_THERMAL_SYSTEM_GET_STATUS_SENSOR_READING_OPCODE;
    th.instructionList[0].operands.getStatusSensorReading.sensorIndex = 0;
    if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2, &th, sizeof th) == NV_OK &&
        th.successfulInstructions >= 1 && th.instructionList[0].executed && th.instructionList[0].result == NV_OK)
        g->temp = th.instructionList[0].operands.getStatusSensorReading.value;
    g->tempIoreg = ioreg_temperature();
    if (g->temp == NA) g->temp = g->tempIoreg;

    NV2080_CTRL_PERF_GET_CURRENT_PSTATE_PARAMS ps = {0};
    if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE, &ps, sizeof ps) == NV_OK && ps.currPstate) g->pstate = __builtin_ctz(ps.currPstate);

    static NV2080_CTRL_FB_GET_INFO_V2_PARAMS fb; memset(&fb, 0, sizeof fb); fb.fbInfoListSize = 6;
    fb.fbInfoList[0].index = NV2080_CTRL_FB_INFO_INDEX_TOTAL_RAM_SIZE;   // KiB
    fb.fbInfoList[1].index = NV2080_CTRL_FB_INFO_INDEX_HEAP_SIZE;
    fb.fbInfoList[2].index = NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE;
    fb.fbInfoList[3].index = NV2080_CTRL_FB_INFO_INDEX_BAR1_SIZE;
    fb.fbInfoList[4].index = NV2080_CTRL_FB_INFO_INDEX_BAR1_AVAIL_SIZE;
    fb.fbInfoList[5].index = NV2080_CTRL_FB_INFO_INDEX_BUS_WIDTH;
    if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_FB_GET_INFO_V2, &fb, sizeof fb) == NV_OK) {
        long long total = fb.fbInfoList[0].data, heap = fb.fbInfoList[1].data, freeK = fb.fbInfoList[2].data;
        g->memTotal = total / 1024; g->memFree = freeK / 1024;
        g->memReserved = (total - heap) / 1024;          // what RM keeps outside the heap, as nvidia-smi's "Reserved"
        g->memUsed = (heap - freeK) / 1024;
        g->bar1Total = fb.fbInfoList[3].data / 1024; g->bar1Used = (fb.fbInfoList[3].data - fb.fbInfoList[4].data) / 1024;
        g->busWidth = fb.fbInfoList[5].data;
    }

    // utilization: the newest GPUMON sample (graphics, frame buffer, NVENC, NVDEC, NVJPG, NVOFA; units of 0.01 %)
    static NV2080_CTRL_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2_PARAMS us; memset(&us, 0, sizeof us);
    us.type = NV2080_CTRL_GPUMON_SAMPLE_TYPE_PERFMON_UTIL; us.bufSize = sizeof us.samples;
    if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2, &us, sizeof us) == NV_OK) {
        NvU64 newest = 0; int best = -1;
        for (int i = 0; i < NV2080_CTRL_PERF_GPUMON_SAMPLE_COUNT_PERFMON_UTIL; i++) if (us.samples[i].base.timeStamp > newest) { newest = us.samples[i].base.timeStamp; best = i; }
        if (best >= 0) {
            const NV2080_CTRL_PERF_GPUMON_PERFMON_UTIL_SAMPLE *s = &us.samples[best];
            g->utilGpu = s->gr.util / 100; g->utilMem = s->fb.util / 100; g->utilEnc = s->nvenc.util / 100;
            g->utilDec = s->nvdec.util / 100; g->utilJpg = s->nvjpg.util / 100; g->utilOfa = s->nvofa.util / 100;
        } else g->utilGpu = g->utilMem = g->utilEnc = g->utilDec = g->utilJpg = g->utilOfa = 0;
    }
    // the video engines' own busy counters where GPUMON had none
    const struct { int eng; int *out; } ve[] = { { NV2080_CTRL_CMD_PERF_VID_ENG_NVENC, &g->utilEnc }, { NV2080_CTRL_CMD_PERF_VID_ENG_NVDEC, &g->utilDec },
                                                 { NV2080_CTRL_CMD_PERF_VID_ENG_NVJPG, &g->utilJpg }, { NV2080_CTRL_CMD_PERF_VID_ENG_NVOFA, &g->utilOfa } };
    for (unsigned i = 0; i < 4; i++) if (*ve[i].out <= 0) {
        NV2080_CTRL_PERF_GET_VID_ENG_PERFMON_SAMPLE_PARAMS v = { .engineType = ve[i].eng };
        if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_PERF_GET_VID_ENG_PERFMON_SAMPLE, &v, sizeof v) == NV_OK) *ve[i].out = (int)v.clkPercentBusy;
    }

    // clocks: the newest PMU clock-domain sample
    static NV2080_CTRL_CLK_PMUMON_CLK_DOMAINS_GET_SAMPLES_PARAMS ck; memset(&ck, 0, sizeof ck);
    if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_CLK_PMUMON_CLK_DOMAINS_GET_SAMPLES, &ck, sizeof ck) == NV_OK && ck.super.numSamples) {
        // samples[0 .. numSamples-1] are copied in chronological order: the last one is the newest
        NvU32 last = ck.super.numSamples < NV2080_CTRL_CLK_PMUMON_CLK_DOMAINS_SAMPLE_COUNT ? ck.super.numSamples - 1 : NV2080_CTRL_CLK_PMUMON_CLK_DOMAINS_SAMPLE_COUNT - 1;
        const NV2080_CTRL_CLK_PMUMON_CLK_DOMAINS_SAMPLE *s = &ck.samples[last];
        g->clkGr = s->gpcClkFreqKHz / 1000; g->clkMem = s->dramClkFreqKHz / 1000; g->clkVid = s->nvdClkFreqKHz / 1000;
    }

    // max / default clocks from perf level info (public SDK, pre-GSP — may not answer on all RMs)
    static NV2080_CTRL_PERF_GET_LEVEL_INFO_V2_PARAMS li; memset(&li, 0, sizeof li);
    li.level = NV2080_CTRL_PERF_GET_LEVEL_INFO_LEVEL_STANDARD;
    li.perfGetClkInfoListSize = NV2080_CTRL_PERF_CLK_MAX_DOMAINS;
    if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_PERF_GET_LEVEL_INFO_V2, &li, sizeof li) == NV_OK) {
        for (unsigned d = 0; d < li.perfGetClkInfoListSize && d < NV2080_CTRL_PERF_CLK_MAX_DOMAINS; d++) {
            const NV2080_CTRL_PERF_GET_CLK_INFO *ci = &li.perfGetClkInfoList[d];
            if (!ci->maxFreq) continue;
            if (d == 0) { g->clkGrMax = (int)(ci->maxFreq / 1000); g->clkGrDef = ci->defaultFreq ? (int)(ci->defaultFreq / 1000) : NA; }
            else if (d == 1) { g->clkMemMax = (int)(ci->maxFreq / 1000); g->clkMemDef = ci->defaultFreq ? (int)(ci->defaultFreq / 1000) : NA; }
            else if (d == 4 || d == 3) { if (g->clkVidMax == NA) g->clkVidMax = (int)(ci->maxFreq / 1000); }
        }
    }

    // PCIe link: current speed / width and the GPU's maximum
    static NV2080_CTRL_BUS_GET_INFO_V2_PARAMS bus; memset(&bus, 0, sizeof bus); bus.busInfoListSize = 2;
    bus.busInfoList[0].index = NV2080_CTRL_BUS_INFO_INDEX_PCIE_GPU_LINK_CTRL_STATUS;
    bus.busInfoList[1].index = NV2080_CTRL_BUS_INFO_INDEX_PCIE_GPU_LINK_CAPS;
    if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_BUS_GET_INFO_V2, &bus, sizeof bus) == NV_OK) {
        NvU32 st = bus.busInfoList[0].data, cap = bus.busInfoList[1].data;
        g->pcieGenCur = pcie_gen((st >> 16) & 0xf); g->pcieWidthCur = (int)((st >> 20) & 0x3f);
        g->pcieGenMax = pcie_gen(cap & 0xf);       g->pcieWidthMax = (int)((cap >> 4) & 0x3f);
    }

    NV2080_CTRL_GPU_GET_NVENC_SW_SESSION_STATS_PARAMS ns = {0};
    if (rm_ctrl(hSubdev, NV2080_CTRL_GPU_GET_NVENC_SW_SESSION_STATS, &ns, sizeof ns) == NV_OK) {
        g->encSessions = (int)ns.encoderSessionCount; g->encFps = (int)ns.averageEncodeFps; g->encLatencyUs = (int)ns.averageEncodeLatency;
    }

    g->dispActive = display_active();

    // the RM's user shared data page: power, power limits, fan RPM, board / memory temperatures
    if (gRusd) {
        RUSD_INST_POWER_USAGE ip; RUSD_AVG_POWER_USAGE ap; RUSD_POWER_LIMITS pl; RUSD_FAN_COOLER_STATUS fc; RUSD_TEMPERATURE t;
        if (RUSD_READ_DATA(gRusd, instPowerUsage, &ip) && ip.info.instGpuPower) g->powerInst = ip.info.instGpuPower;
        if (RUSD_READ_DATA(gRusd, avgPowerUsage, &ap) && ap.info.averageGpuPower) g->powerAvg = ap.info.averageGpuPower;
        if (RUSD_READ_DATA(gRusd, powerLimitGpu, &pl)) {
            if (pl.info.requestedmW) g->powerLimitReq = pl.info.requestedmW;
            if (pl.info.enforcedmW) g->powerLimitEnf = pl.info.enforcedmW;
        }
        if (RUSD_READ_DATA(gRusd, fanCoolerStatus, &fc))
            for (unsigned i = 0; i < RUSD_FAN_COOLER_MAX_COOLERS; i++) if (fc.info.rpmCurr[i]) g->fanRpm[g->nFan++] = (int)fc.info.rpmCurr[i];
        // fan speed %: the shared data page carries RPM only, so estimate it against a configured maximum RPM (--fan-max-rpm /
        // NVSMI_FAN_MAX_RPM). NVIDIA's own % is the requested duty cycle, not RPM/max, so this is an approximation.
        if (gFanMaxRpm > 0 && g->nFan) { int p = (g->fanRpm[0] * 100 + gFanMaxRpm / 2) / gFanMaxRpm; g->fanPct = p < 0 ? 0 : p > 100 ? 100 : p; }
        if (RUSD_READ_DATA(gRusd, temperatures[RUSD_TEMPERATURE_TYPE_MEMORY], &t) && t.temperature > 0) g->tempMem = NV_TYPES_NV_TEMP_TO_CELSIUS_ROUNDED(t.temperature);
        if (RUSD_READ_DATA(gRusd, temperatures[RUSD_TEMPERATURE_TYPE_BOARD], &t) && t.temperature > 0) g->tempBoard = NV_TYPES_NV_TEMP_TO_CELSIUS_ROUNDED(t.temperature);
        if (g->powerInst == NA && g->powerAvg != NA) g->powerInst = g->powerAvg;
    }

    // the closed thermal / pmgr controls: the real fan duty cycle and the default/min/max power limits
    g->fanDuty = fan_duty();
    pmgr_power_limits(g);

    // processes: PIDs that own 3D / compute objects, and their video memory
    const struct { NvU32 cls; char type; } kinds[] = { { 0xC997u, 'G' }, { 0xC9C0u, 'C' }, { 0xC597u, 'G' }, { 0xC5C0u, 'C' }, { 0xC797u, 'G' }, { 0xC7C0u, 'C' } };
    for (unsigned k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        static NV2080_CTRL_GPU_GET_PIDS_PARAMS pp; memset(&pp, 0, sizeof pp);
        pp.idType = NV2080_CTRL_GPU_GET_PIDS_ID_TYPE_CLASS; pp.id = kinds[k].cls;
        if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_GPU_GET_PIDS, &pp, sizeof pp) != NV_OK) continue;
        for (NvU32 i = 0; i < pp.pidTblCount && g->nproc < 256; i++) {
            int dup = 0;
            for (int j = 0; j < g->nproc; j++) if (g->procs[j].pid == pp.pidTbl[i]) { if (g->procs[j].type != kinds[k].type) g->procs[j].type = 'X'; dup = 1; }
            if (!dup) g->procs[g->nproc++] = (Proc){ .pid = pp.pidTbl[i], .type = kinds[k].type, .memMiB = NA };
        }
    }
    for (int base = 0; base < g->nproc; base += NV2080_CTRL_GPU_GET_PID_INFO_MAX_COUNT) {
        static NV2080_CTRL_GPU_GET_PID_INFO_PARAMS pi; memset(&pi, 0, sizeof pi);
        int n = g->nproc - base; if (n > NV2080_CTRL_GPU_GET_PID_INFO_MAX_COUNT) n = NV2080_CTRL_GPU_GET_PID_INFO_MAX_COUNT;
        pi.pidInfoListCount = n;
        for (int i = 0; i < n; i++) { pi.pidInfoList[i].pid = g->procs[base + i].pid; pi.pidInfoList[i].index = NV2080_CTRL_GPU_PID_INFO_INDEX_VIDEO_MEMORY_USAGE; }
        if (rm_ctrl(hSubdev, NV2080_CTRL_CMD_GPU_GET_PID_INFO, &pi, sizeof pi) != NV_OK) continue;
        for (int i = 0; i < n; i++) if (pi.pidInfoList[i].result == NV_OK) {
            const NV2080_CTRL_GPU_PID_INFO_VIDEO_MEMORY_USAGE_DATA *v = &pi.pidInfoList[i].data.vidMemUsage;
            g->procs[base + i].memMiB = (long long)((v->memPrivate + v->memSharedOwned) >> 20);
        }
    }
}

// ---- output -----------------------------------------------------------------------------------------------------------
static const char *num(char *buf, size_t n, long long v, const char *unit) { if (v == NA) snprintf(buf, n, "N/A"); else snprintf(buf, n, "%lld%s", v, unit); return buf; }
static void proc_label(unsigned pid, char *out, size_t n)
{
    char path[PROC_PIDPATHINFO_MAXSIZE] = "";
    if (proc_pidpath((int)pid, path, sizeof path) <= 0) proc_name((int)pid, path, sizeof path);
    if (!path[0]) snprintf(path, sizeof path, "[pid %u]", pid);
    snprintf(out, n, "%s", path);
}
static void cell(char *out, int width, const char *left, const char *right)
{
    int l = (int)strlen(left), r = (int)strlen(right), gap = width - 1 - l - r;
    if (gap < 1) gap = 1;
    sprintf(out, "%s%*s%s ", left, gap, "", right);
}

static void print_table(const Gpu *g)
{
    time_t t = time(NULL); char ts[64]; strftime(ts, sizeof ts, "%a %b %e %H:%M:%S %Y", localtime(&t));
    printf("%s       \n", ts);
    printf("+-----------------------------------------------------------------------------------------+\n");
    char hdr[128]; snprintf(hdr, sizeof hdr, "NVIDIA-SMI %-22s Driver Version: %-14s CUDA Version: N/A", g->rmVersion, g->driver);
    printf("| %-87s |\n", hdr);
    printf("|-----------------------------------------+------------------------+----------------------+\n");
    printf("| GPU  Name                 Persistence-M | Bus-Id          Disp.A | Volatile Uncorr. ECC |\n");
    printf("| Fan  Temp   Perf          Pwr:Usage/Cap |           Memory-Usage | GPU-Util  Compute M. |\n");
    printf("|                                         |                        |               MIG M. |\n");
    printf("|=========================================+========================+======================|\n");
    char a[96], b[64], c[64], l[96], m[64], r[64];
    snprintf(a, sizeof a, "%4d  %.26s", 0, g->name); cell(l, 41, a, "N/A");
    snprintf(a, sizeof a, "   %s", g->busId); cell(m, 24, a, g->dispActive ? "On" : "Off");
    cell(r, 22, "", "N/A");
    printf("|%s|%s|%s|\n", l, m, r);
    char temp[16], perf[16];
    if (g->temp != NA) snprintf(temp, sizeof temp, "%dC", g->temp); else snprintf(temp, sizeof temp, "N/A");
    if (g->pstate != NA) snprintf(perf, sizeof perf, "P%d", g->pstate); else snprintf(perf, sizeof perf, "N/A");
    char pwr[32];
    snprintf(pwr, sizeof pwr, "%s / %s", g->powerInst != NA ? (snprintf(b, sizeof b, "%lldW", (g->powerInst + 500) / 1000), b) : "N/A",
             g->powerLimitEnf != NA ? (snprintf(c, sizeof c, "%lldW", (g->powerLimitEnf + 500) / 1000), c) : "N/A");
    int fan = g->fanDuty != NA ? g->fanDuty : g->fanPct; char fans[8];
    if (fan != NA) snprintf(fans, sizeof fans, "%d%%", fan); else snprintf(fans, sizeof fans, "N/A");
    snprintf(a, sizeof a, "%4s  %5s    %-4s", fans, temp, perf); cell(l, 41, a, pwr);
    if (g->memTotal != NA) snprintf(b, sizeof b, "%lldMiB / %6lldMiB", g->memUsed, g->memTotal);
    else snprintf(b, sizeof b, "N/A / N/A");
    cell(m, 24, "", b);
    if (g->utilGpu != NA) snprintf(c, sizeof c, "%8d%%", g->utilGpu); else snprintf(c, sizeof c, "%9s", "N/A");
    cell(r, 22, c, "Default");
    printf("|%s|%s|%s|\n", l, m, r);
    cell(l, 41, "", ""); cell(m, 24, "", ""); cell(r, 22, "", "N/A");
    printf("|%s|%s|%s|\n", l, m, r);
    printf("+-----------------------------------------+------------------------+----------------------+\n");
    printf("%91s\n", "");
    printf("+-----------------------------------------------------------------------------------------+\n");
    printf("| Processes:                                                                              |\n");
    printf("|  GPU   GI   CI              PID   Type   Process name                        GPU Memory |\n");
    printf("|        ID   ID                                                               Usage      |\n");
    printf("|=========================================================================================|\n");
    if (!g->nproc) printf("|  No running processes found                                                             |\n");
    for (int i = 0; i < g->nproc; i++) {
        char path[PROC_PIDPATHINFO_MAXSIZE], name[40], mem[16], type[4];
        proc_label(g->procs[i].pid, path, sizeof path);
        size_t n = strlen(path);
        if (n > 33) snprintf(name, sizeof name, "...%s", path + n - 30); else snprintf(name, sizeof name, "%s", path);
        num(mem, sizeof mem, g->procs[i].memMiB, "MiB");
        if (g->procs[i].type == 'X') snprintf(type, sizeof type, "C+G"); else snprintf(type, sizeof type, "%c", g->procs[i].type);
        printf("|  %3d   N/A  N/A  %15u  %5s   %-33s %10s |\n", 0, g->procs[i].pid, type, name, mem);
    }
    printf("+-----------------------------------------------------------------------------------------+\n");
}

static int want(const char *sections, const char *s)
{
    if (!sections) return 1;
    size_t n = strlen(s);
    for (const char *p = sections; (p = strstr(p, s)); p += n) {
        int left = (p == sections || p[-1] == ',' || p[-1] == ' ');
        int right = (p[n] == 0 || p[n] == ',' || p[n] == ' ');
        if (left && right) return 1;
    }
    return 0;
}
static void print_q(const Gpu *g, const char *sections)
{
    char x[64], y[64];
    time_t t = time(NULL); char ts[64]; strftime(ts, sizeof ts, "%a %b %e %H:%M:%S %Y", localtime(&t));
    printf("\n==============NVSMI LOG==============\n\n");
    printf("Timestamp                                 : %s\n", ts);
    printf("Driver Version                            : %s\n", g->driver);
    printf("NVIDIA RM Version                         : %s\n", g->rmVersion);
    printf("CUDA Version                              : N/A\n\n");
    printf("Attached GPUs                             : 1\n");
    printf("GPU %s\n", g->busId);
    printf("    Product Name                          : %s\n", g->name);
    printf("    Product Architecture                  : %s\n", g->arch);
    printf("    Display Active                        : %s\n", g->dispActive ? "Enabled" : "Disabled");
    printf("    GPU UUID                              : %s\n", g->uuid);
    printf("    VBIOS Version                         : %s\n", g->vbios);
    if (g->serial[0]) printf("    Serial Number                         : %s\n", g->serial);
    if (g->partNumber[0]) printf("    Board Part Number                     : %s\n", g->partNumber);
    if (want(sections, "PCI")) {
        printf("    PCI\n");
        printf("        Bus                               : 0x%02X\n", (unsigned)strtoul(g->busId + 9, NULL, 16));
        printf("        Device Id                         : 0x%04X10DE\n", g->devId);
        printf("        Bus Id                            : %s\n", g->busId);
        printf("        GPU Link Info\n");
        printf("            PCIe Generation\n");
        printf("                Max                       : %s\n", num(x, sizeof x, g->pcieGenMax, ""));
        printf("                Current                   : %s\n", num(x, sizeof x, g->pcieGenCur, ""));
        printf("            Link Width\n");
        printf("                Max                       : %s\n", g->pcieWidthMax != NA ? (snprintf(x, sizeof x, "%dx", g->pcieWidthMax), x) : "N/A");
        printf("                Current                   : %s\n", g->pcieWidthCur != NA ? (snprintf(y, sizeof y, "%dx", g->pcieWidthCur), y) : "N/A");
    }
    // the real duty cycle from the cooler-status control; without it, an RPM/max estimate when a max is configured
    if (g->fanDuty != NA) printf("    Fan Speed                             : %d %%\n", g->fanDuty);
    else if (g->fanPct != NA) printf("    Fan Speed                             : %d %% (estimated, %d/%d RPM)\n", g->fanPct, g->nFan ? g->fanRpm[0] : 0, gFanMaxRpm);
    else printf("    Fan Speed                             : N/A\n");
    for (int i = 0; i < g->nFan; i++) { char lab[48]; snprintf(lab, sizeof lab, "Fan %d Speed (tachometer)", i); printf("    %-38s: %d RPM\n", lab, g->fanRpm[i]); }
    if (want(sections, "PERFORMANCE")) printf("    Performance State                     : %s\n", g->pstate != NA ? (snprintf(x, sizeof x, "P%d", g->pstate), x) : "N/A");
    if (want(sections, "MEMORY")) {
        printf("    FB Memory Usage\n");
        printf("        Total                             : %s\n", num(x, sizeof x, g->memTotal, " MiB"));
        printf("        Reserved                          : %s\n", num(x, sizeof x, g->memReserved, " MiB"));
        printf("        Used                              : %s\n", num(x, sizeof x, g->memUsed, " MiB"));
        printf("        Free                              : %s\n", num(x, sizeof x, g->memFree, " MiB"));
        printf("    BAR1 Memory Usage\n");
        printf("        Total                             : %s\n", num(x, sizeof x, g->bar1Total, " MiB"));
        printf("        Used                              : %s\n", num(x, sizeof x, g->bar1Used, " MiB"));
        printf("        Free                              : %s\n", g->bar1Total != NA ? (snprintf(x, sizeof x, "%lld MiB", g->bar1Total - g->bar1Used), x) : "N/A");
        printf("    Memory Bus Width                      : %s\n", num(x, sizeof x, g->busWidth, " bits"));
    }
    if (want(sections, "UTILIZATION")) {
        printf("    Utilization\n");
        printf("        Gpu                               : %s\n", num(x, sizeof x, g->utilGpu, " %"));
        printf("        Memory                            : %s\n", num(x, sizeof x, g->utilMem, " %"));
        printf("        Encoder                           : %s\n", num(x, sizeof x, g->utilEnc, " %"));
        printf("        Decoder                           : %s\n", num(x, sizeof x, g->utilDec, " %"));
        printf("        JPEG                              : %s\n", num(x, sizeof x, g->utilJpg, " %"));
        printf("        OFA                               : %s\n", num(x, sizeof x, g->utilOfa, " %"));
    }
    if (want(sections, "ENCODER_STATS")) {
        printf("    Encoder Stats\n");
        printf("        Active Sessions                   : %s\n", num(x, sizeof x, g->encSessions, ""));
        printf("        Average FPS                       : %s\n", num(x, sizeof x, g->encFps, ""));
        printf("        Average Latency                   : %s\n", num(x, sizeof x, g->encLatencyUs, ""));
    }
    if (want(sections, "TEMPERATURE")) {
        printf("    Temperature\n");
        printf("        GPU Current Temp                  : %s\n", num(x, sizeof x, g->temp, " C"));
        printf("        GPU Temp (IORegistry)             : %s\n", num(x, sizeof x, g->tempIoreg, " C"));
        printf("        GPU T.Limit Temp                  : N/A\n");
        printf("        Memory Current Temp               : %s\n", num(x, sizeof x, g->tempMem, " C"));
        printf("        Board Temp                        : %s\n", num(x, sizeof x, g->tempBoard, " C"));
    }
    if (want(sections, "POWER")) {
        #define W(v) ((v) == NA ? "N/A" : (snprintf(x, sizeof x, "%.2f W", (v) / 1000.0), x))
        printf("    GPU Power Readings\n");
        printf("        Average Power Draw                : %s\n", W(g->powerAvg));
        printf("        Instantaneous Power Draw          : %s\n", W(g->powerInst));
        printf("        Current Power Limit               : %s\n", W(g->powerLimitEnf));
        printf("        Requested Power Limit             : %s\n", W(g->powerLimitReq));
        printf("        Default Power Limit               : %s\n", W(g->powerLimitDef));
        printf("        Min Power Limit                   : %s\n", W(g->powerLimitMin));
        printf("        Max Power Limit                   : %s\n", W(g->powerLimitMax));
        #undef W
    }
    if (want(sections, "CLOCK")) {
        printf("    Clocks\n");
        printf("        Graphics                          : %s\n", num(x, sizeof x, g->clkGr, " MHz"));
        printf("        SM                                : %s\n", num(x, sizeof x, g->clkGr, " MHz"));
        printf("        Memory                            : %s\n", num(x, sizeof x, g->clkMem, " MHz"));
        printf("        Video                             : %s\n", num(x, sizeof x, g->clkVid, " MHz"));
        printf("    Max Clocks\n");
        printf("        Graphics                          : %s\n", num(x, sizeof x, g->clkGrMax, " MHz"));
        printf("        SM                                : %s\n", num(x, sizeof x, g->clkGrMax, " MHz"));
        printf("        Memory                            : %s\n", num(x, sizeof x, g->clkMemMax, " MHz"));
        printf("        Video                             : %s\n", num(x, sizeof x, g->clkVidMax, " MHz"));
    }
    if (want(sections, "ECC")) {
        printf("    Ecc Mode\n");
        printf("        Current                           : N/A\n");
        printf("        Pending                           : N/A\n");
    }
    if (want(sections, "PIDS")) {
        printf("    Processes\n");
        if (!g->nproc) printf("        None\n");
        for (int i = 0; i < g->nproc; i++) {
            char path[PROC_PIDPATHINFO_MAXSIZE]; proc_label(g->procs[i].pid, path, sizeof path);
            printf("        Process ID                        : %u\n", g->procs[i].pid);
            printf("            Type                          : %s\n", g->procs[i].type == 'X' ? "C+G" : g->procs[i].type == 'C' ? "C" : "G");
            printf("            Name                          : %s\n", path);
            printf("            Used GPU Memory               : %s\n", num(x, sizeof x, g->procs[i].memMiB, " MiB"));
        }
    }
    printf("\n");
}

// --query-gpu fields: name, NVIDIA's unit, description
typedef struct { const char *name, *unit, *desc; } Field;
static const Field kFields[] = {
    { "timestamp", "", "Current timestamp" },
    { "count", "", "Number of GPUs" },
    { "index", "", "Zero-based index of the GPU" },
    { "name", "", "Official product name" },
    { "uuid", "", "Globally unique immutable identifier" },
    { "serial", "", "Board serial number (N/A on consumer cards)" },
    { "gpu_bus_id", "", "PCI bus id (same as pci.bus_id)" },
    { "pci.bus_id", "", "PCI bus id as domain:bus:device.function" },
    { "pci.device_id", "", "PCI vendor device id" },
    { "pci.sub_device_id", "", "PCI sub-system id" },
    { "driver_version", "", "Installed driver version" },
    { "vbios_version", "", "VBIOS version of the GPU" },
    { "display_active", "", "Whether a display is initialized on the GPU" },
    { "display_mode", "", "Whether a physical display is connected" },
    { "pstate", "", "Current performance state (P0-P12)" },
    { "temperature.gpu", "", "Core GPU temperature in degrees C" },
    { "temperature.memory", "", "HBM memory temperature in degrees C" },
    { "temperature.board", "", "Board temperature in degrees C" },
    { "fan.speed", " %", "Fan speed as a percentage of maximum" },
    { "fan.rpm", " RPM", "Fan speed in RPM (tachometer)" },
    { "utilization.gpu", " %", "Percent of time the GPU was busy" },
    { "utilization.memory", " %", "Percent of time the memory controller was busy" },
    { "utilization.encoder", " %", "Percent of time the encoder was busy" },
    { "utilization.decoder", " %", "Percent of time the decoder was busy" },
    { "utilization.jpeg", " %", "Percent of time the JPEG engine was busy" },
    { "utilization.ofa", " %", "Percent of time the OFA engine was busy" },
    { "memory.total", " MiB", "Total installed GPU memory" },
    { "memory.reserved", " MiB", "Memory reserved by the driver" },
    { "memory.used", " MiB", "Memory allocated by active contexts" },
    { "memory.free", " MiB", "Unallocated GPU memory" },
    { "memory.bus_width", " bits", "Memory bus width" },
    { "bar1.total", " MiB", "Total BAR1 memory" },
    { "bar1.used", " MiB", "Used BAR1 memory" },
    { "bar1.free", " MiB", "Free BAR1 memory" },
    { "clocks.gr", " MHz", "Current frequency of graphics clock" },
    { "clocks.sm", " MHz", "Current frequency of SM clock" },
    { "clocks.mem", " MHz", "Current frequency of memory clock" },
    { "clocks.video", " MHz", "Current frequency of video clock" },
    { "clocks.current.graphics", " MHz", "Current frequency of graphics clock" },
    { "clocks.current.sm", " MHz", "Current frequency of SM clock" },
    { "clocks.current.memory", " MHz", "Current frequency of memory clock" },
    { "clocks.current.video", " MHz", "Current frequency of video clock" },
    { "clocks.max.graphics", " MHz", "Maximum frequency of graphics clock" },
    { "clocks.max.sm", " MHz", "Maximum frequency of SM clock" },
    { "clocks.max.memory", " MHz", "Maximum frequency of memory clock" },
    { "clocks.max.video", " MHz", "Maximum frequency of video clock" },
    { "pcie.link.gen.current", "", "Current PCIe link generation" },
    { "pcie.link.gen.max", "", "Maximum PCIe link generation" },
    { "pcie.link.width.current", "", "Current PCIe link width" },
    { "pcie.link.width.max", "", "Maximum PCIe link width" },
    { "encoder.stats.sessionCount", "", "Number of active encoder sessions" },
    { "encoder.stats.averageFps", "", "Average FPS of active encoder sessions" },
    { "encoder.stats.averageLatency", "", "Average latency of active encoder sessions" },
    { "power.draw", " W", "Power draw for the entire board" },
    { "power.draw.average", " W", "Average power draw" },
    { "power.draw.instant", " W", "Instantaneous power draw" },
    { "power.limit", " W", "Software power limit" },
    { "enforced.power.limit", " W", "Enforced power limit" },
    { "power.default_limit", " W", "Default power limit" },
    { "power.min_limit", " W", "Minimum power limit" },
    { "power.max_limit", " W", "Maximum power limit" },
};
static int field_value(const Gpu *g, const char *f, char *out, size_t n)
{
    long long v = NA; const char *s = NULL;
    if (!strcmp(f, "timestamp")) { time_t t = time(NULL); strftime(out, n, "%Y/%m/%d %H:%M:%S.000", localtime(&t)); return 0; }
    else if (!strcmp(f, "count")) v = 1;
    else if (!strcmp(f, "index")) v = 0; else if (!strcmp(f, "name")) s = g->name; else if (!strcmp(f, "uuid")) s = g->uuid;
    else if (!strcmp(f, "serial")) s = g->serial[0] ? g->serial : "[N/A]";
    else if (!strcmp(f, "gpu_bus_id") || !strcmp(f, "pci.bus_id")) s = g->busId;
    else if (!strcmp(f, "driver_version")) s = g->driver;
    else if (!strcmp(f, "vbios_version")) s = g->vbios;
    else if (!strcmp(f, "display_active")) s = g->dispActive ? "Enabled" : "Disabled";
    else if (!strcmp(f, "display_mode")) s = g->dispActive ? "Enabled" : "Disabled";
    else if (!strcmp(f, "pci.device_id")) { snprintf(out, n, "0x%04X10DE", g->devId); return 0; }
    else if (!strcmp(f, "pci.sub_device_id")) s = "[N/A]";
    else if (!strcmp(f, "pstate")) { if (g->pstate == NA) s = "[N/A]"; else { snprintf(out, n, "P%d", g->pstate); return 0; } }
    else if (!strcmp(f, "temperature.gpu")) v = g->temp;
    else if (!strcmp(f, "temperature.memory")) v = g->tempMem;
    else if (!strcmp(f, "temperature.board")) v = g->tempBoard;
    else if (!strcmp(f, "utilization.gpu")) v = g->utilGpu; else if (!strcmp(f, "utilization.memory")) v = g->utilMem;
    else if (!strcmp(f, "utilization.encoder")) v = g->utilEnc; else if (!strcmp(f, "utilization.decoder")) v = g->utilDec;
    else if (!strcmp(f, "utilization.jpeg")) v = g->utilJpg; else if (!strcmp(f, "utilization.ofa")) v = g->utilOfa;
    else if (!strcmp(f, "memory.total")) v = g->memTotal; else if (!strcmp(f, "memory.reserved")) v = g->memReserved;
    else if (!strcmp(f, "memory.used")) v = g->memUsed; else if (!strcmp(f, "memory.free")) v = g->memFree;
    else if (!strcmp(f, "memory.bus_width")) v = g->busWidth;
    else if (!strcmp(f, "bar1.total")) v = g->bar1Total;
    else if (!strcmp(f, "bar1.used")) v = g->bar1Used;
    else if (!strcmp(f, "bar1.free")) v = (g->bar1Total != NA ? g->bar1Total - g->bar1Used : NA);
    else if (!strcmp(f, "clocks.gr") || !strcmp(f, "clocks.sm") || !strcmp(f, "clocks.current.graphics") || !strcmp(f, "clocks.current.sm")) v = g->clkGr;
    else if (!strcmp(f, "clocks.mem") || !strcmp(f, "clocks.current.memory")) v = g->clkMem;
    else if (!strcmp(f, "clocks.video") || !strcmp(f, "clocks.current.video")) v = g->clkVid;
    else if (!strcmp(f, "clocks.max.graphics") || !strcmp(f, "clocks.max.sm")) v = g->clkGrMax;
    else if (!strcmp(f, "clocks.max.memory")) v = g->clkMemMax;
    else if (!strcmp(f, "clocks.max.video")) v = g->clkVidMax;
    else if (!strcmp(f, "pcie.link.gen.current")) v = g->pcieGenCur; else if (!strcmp(f, "pcie.link.gen.max")) v = g->pcieGenMax;
    else if (!strcmp(f, "pcie.link.width.current")) v = g->pcieWidthCur; else if (!strcmp(f, "pcie.link.width.max")) v = g->pcieWidthMax;
    else if (!strcmp(f, "encoder.stats.sessionCount")) v = g->encSessions; else if (!strcmp(f, "encoder.stats.averageFps")) v = g->encFps;
    else if (!strcmp(f, "encoder.stats.averageLatency")) v = g->encLatencyUs;
    else if (!strcmp(f, "fan.speed")) v = g->fanDuty != NA ? g->fanDuty : g->fanPct;
    else if (!strcmp(f, "fan.rpm")) v = g->nFan ? g->fanRpm[0] : NA;
    else if (!strcmp(f, "power.draw") || !strcmp(f, "power.draw.instant") || !strcmp(f, "power.draw.average") || !strcmp(f, "power.limit") || !strcmp(f, "enforced.power.limit") ||
             !strcmp(f, "power.default_limit") || !strcmp(f, "power.min_limit") || !strcmp(f, "power.max_limit")) {
        const long long mw = !strcmp(f, "power.draw.average") ? g->powerAvg : !strncmp(f, "power.draw", 10) ? g->powerInst
                           : !strcmp(f, "power.limit") && g->powerLimitReq != NA ? g->powerLimitReq
                           : !strcmp(f, "power.default_limit") ? g->powerLimitDef : !strcmp(f, "power.min_limit") ? g->powerLimitMin
                           : !strcmp(f, "power.max_limit") ? g->powerLimitMax : g->powerLimitEnf;
        if (mw == NA) s = "[N/A]"; else { snprintf(out, n, "%.2f", mw / 1000.0); return 0; }
    }
    else return -1;
    if (s) snprintf(out, n, "%s", s); else if (v == NA) snprintf(out, n, "[N/A]"); else snprintf(out, n, "%lld", v);
    return 0;
}
static int print_query(const Gpu *g, const char *fields, int header, int units)
{
    char list[1024]; snprintf(list, sizeof list, "%s", fields);
    char *names[64]; int nf = 0;
    for (char *t = strtok(list, ","); t && nf < 64; t = strtok(NULL, ",")) { while (*t == ' ') t++; names[nf++] = t; }
    for (int i = 0; i < nf; i++) { char v[128]; if (field_value(g, names[i], v, sizeof v)) { fprintf(stderr, "Field \"%s\" is not a valid field to query.\n", names[i]); return 2; } }
    if (header) for (int i = 0; i < nf; i++) {
        const char *u = ""; for (unsigned k = 0; k < sizeof kFields / sizeof kFields[0]; k++) if (!strcmp(kFields[k].name, names[i])) u = kFields[k].unit;
        printf("%s%s%s%s%s", i ? ", " : "", names[i], units && *u ? " [" : "", units && *u ? u + 1 : "", units && *u ? "]" : "");
    }
    if (header) printf("\n");
    for (int i = 0; i < nf; i++) {
        char v[128]; field_value(g, names[i], v, sizeof v);
        const char *u = ""; for (unsigned k = 0; k < sizeof kFields / sizeof kFields[0]; k++) if (!strcmp(kFields[k].name, names[i])) u = kFields[k].unit;
        printf("%s%s%s", i ? ", " : "", v, units && v[0] != '[' ? u : "");
    }
    printf("\n");
    return 0;
}

// --list: every value we read, one "key : value" per line (the --query-gpu fields, then what has no NVIDIA field name)
static void print_list(const Gpu *g)
{
    int w = 0;
    for (unsigned k = 0; k < sizeof kFields / sizeof kFields[0]; k++) { int l = (int)strlen(kFields[k].name); if (l > w) w = l; }
    char v[PROC_PIDPATHINFO_MAXSIZE + 16], key[64];
    for (unsigned k = 0; k < sizeof kFields / sizeof kFields[0]; k++) {
        if (field_value(g, kFields[k].name, v, sizeof v)) continue;
        int na = v[0] == '[';
        printf("%-*s : %s%s\n", w, kFields[k].name, na ? "N/A" : v, na ? "" : kFields[k].unit);
    }
    #define KV(k, ...) (snprintf(v, sizeof v, __VA_ARGS__), printf("%-*s : %s\n", w, k, v))
    #define KN(k, x, unit) ((x) == NA ? KV(k, "N/A") : KV(k, "%lld%s", (long long)(x), unit))
    KV("architecture", "%s", g->arch);
    KV("rm_version", "%s", g->rmVersion);
    KV("board_part_number", "%s", g->partNumber[0] ? g->partNumber : "N/A");
    KN("temperature.gpu.ioreg", g->tempIoreg, "");
    KN("fan.count", g->nFan, "");
    for (int i = 0; i < g->nFan; i++) { snprintf(key, sizeof key, "fan.%d.rpm", i); KN(key, g->fanRpm[i], " RPM"); }
    KN("process.count", g->nproc, "");
    for (int i = 0; i < g->nproc; i++) {
        const Proc *p = &g->procs[i]; char path[PROC_PIDPATHINFO_MAXSIZE];
        proc_label(p->pid, path, sizeof path);
        snprintf(key, sizeof key, "process.%d.pid", i);  KV(key, "%u", p->pid);
        snprintf(key, sizeof key, "process.%d.type", i); KV(key, "%s", p->type == 'X' ? "C+G" : p->type == 'C' ? "C" : "G");
        snprintf(key, sizeof key, "process.%d.name", i); KV(key, "%s", path);
        snprintf(key, sizeof key, "process.%d.used_memory", i); KN(key, p->memMiB, " MiB");
    }
    #undef KN
    #undef KV
}

static void dmon_header(void)
{
    printf("# gpu    pwr  gtemp  mtemp     sm    mem    enc    dec    jpg    ofa   mclk   pclk\n");
    printf("# Idx      W      C      C      %%      %%      %%      %%      %%      %%    MHz    MHz\n");
}
static void dmon_line(const Gpu *g)
{
    char p[8], t[8], mt[8], s[8], m[8], e[8], d[8], j[8], o[8], mc[8], pc[8];
    #define F(buf, v) (v == NA ? snprintf(buf, sizeof buf, "-") : snprintf(buf, sizeof buf, "%d", v), buf)
    const int pw = g->powerInst == NA ? NA : (int)((g->powerInst + 500) / 1000);
    printf("%5d %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s\n", 0, F(p, pw), F(t, g->temp), F(mt, g->tempMem), F(s, g->utilGpu), F(m, g->utilMem), F(e, g->utilEnc),
           F(d, g->utilDec), F(j, g->utilJpg), F(o, g->utilOfa), F(mc, g->clkMem), F(pc, g->clkGr));
    #undef F
}

static void pmon_header(void)
{
    printf("# gpu        pid  type    sm   mem   enc   dec   jpg   ofa    fb   command\n");
    printf("# Idx          #     -     %%     %%     %%     %%     %%     %%   MiB   name\n");
}
static void pmon_line(const Gpu *g)
{
    if (!g->nproc) { printf("%5d          -     -     -     -     -     -     -     -     -   -\n", 0); return; }
    for (int i = 0; i < g->nproc; i++) {
        char path[PROC_PIDPATHINFO_MAXSIZE], name[16]; proc_label(g->procs[i].pid, path, sizeof path);
        const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
        snprintf(name, sizeof name, "%s", base);
        char mem[8]; if (g->procs[i].memMiB != NA) snprintf(mem, sizeof mem, "%lld", g->procs[i].memMiB); else snprintf(mem, sizeof mem, "-");
        printf("%5d %10u  %4s     -     -     -     -     -     - %5s   %s\n", 0, g->procs[i].pid,
               g->procs[i].type == 'X' ? "C+G" : g->procs[i].type == 'C' ? "  C" : "  G", mem, name);
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: nvidia-smi [-L] [--list] [-q [-d SECTIONS]] [--query-gpu=FIELDS --format=csv[,noheader][,nounits]]\n"
                    "                  [dmon [-d SEC] [-c N]] [pmon [-d SEC] [-c N]] [-l [SEC] | -lms [MS]] [--fan-max-rpm N] [-v]\n"
                    "                  [--help-query-gpu]\n"
                    "  --fan-max-rpm N (or NVSMI_FAN_MAX_RPM): show fan.speed %% as an estimate of RPM / N (the page carries RPM only)\n");
}

int main(int argc, char **argv)
{
    int list = 0, kv = 0, full = 0, dmon = 0, pmon = 0, count = 0, header = 1, units = 1, hasFormat = 0, dmonDelay = 1; long loopMs = 0;
    const char *sections = NULL, *fields = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-L") || !strcmp(a, "--list-gpus")) list = 1;
        else if (!strcmp(a, "--list")) kv = 1;
        else if (!strcmp(a, "-q") || !strcmp(a, "--query")) full = 1;
        else if ((!strcmp(a, "-d") || !strcmp(a, "--display")) && !(dmon || pmon) && i + 1 < argc) sections = argv[++i];
        else if (!strncmp(a, "--display=", 10)) sections = a + 10;
        else if (!strcmp(a, "dmon")) dmon = 1;
        else if (!strcmp(a, "pmon")) pmon = 1;
        else if (!strcmp(a, "-c") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(a, "-d") && (dmon || pmon) && i + 1 < argc) { int d = atoi(argv[++i]); if (d > 0) dmonDelay = d; }
        else if (!strncmp(a, "--query-gpu=", 12)) fields = a + 12;
        else if (!strncmp(a, "--format=", 9)) { hasFormat = 1; if (strstr(a, "noheader")) header = 0; if (strstr(a, "nounits")) units = 0; }
        else if (!strcmp(a, "-l") || !strcmp(a, "--loop")) { loopMs = 1000; if (i + 1 < argc && atoi(argv[i + 1]) > 0) loopMs = 1000L * atoi(argv[++i]); }
        else if (!strncmp(a, "--loop=", 7)) loopMs = 1000L * (atoi(a + 7) > 0 ? atoi(a + 7) : 1);
        else if (!strcmp(a, "-lms") || !strcmp(a, "--loop-ms")) { loopMs = 1000; if (i + 1 < argc && atoi(argv[i + 1]) > 0) loopMs = atoi(argv[++i]); }
        else if (!strcmp(a, "-v")) gVerbose = 1;
        else if (!strcmp(a, "--fan-max-rpm") && i + 1 < argc) gFanMaxRpm = atoi(argv[++i]);
        else if (!strncmp(a, "--fan-max-rpm=", 14)) gFanMaxRpm = atoi(a + 14);
        else if (!strcmp(a, "--help-query-gpu")) {
            int maxw = 0;
            for (unsigned k = 0; k < sizeof kFields / sizeof kFields[0]; k++) { int w = (int)strlen(kFields[k].name); if (w > maxw) maxw = w; }
            printf("List of valid properties to query for the switch \"--query-gpu\":\n\n");
            for (unsigned k = 0; k < sizeof kFields / sizeof kFields[0]; k++) printf("  \"%-*s\" - %s\n", maxw, kFields[k].name, kFields[k].desc);
            return 0;
        }
        else if (!strcmp(a, "-i")) { if (i + 1 < argc) i++; }   // GPU index selector: ignored (single GPU), swallow the argument
        else { usage(); return 2; }
    }
    if (fields && !hasFormat) { fprintf(stderr, "\"--format=csv\" is required when \"--query-gpu\" is specified.\nRun 'nvidia-smi --help-query-gpu' for more info.\n"); return 2; }
    if (!gFanMaxRpm) { const char *e = getenv("NVSMI_FAN_MAX_RPM"); if (e && atoi(e) > 0) gFanMaxRpm = atoi(e); }
    signal(SIGINT, sig_stop); signal(SIGTERM, sig_stop);
    nv_ioctl_card_info_t ci;
    if (rm_open(&ci)) { rm_close(); return 9; }
    static Gpu g;
    int rc = 0, n = 0;
    if (list) { query(&g, &ci); printf("GPU 0: %s (UUID: %s)\n", g.name, g.uuid); }
    else if (dmon) {
        dmon_header();
        while (gRunning) { query(&g, &ci); dmon_line(&g); fflush(stdout); if (count && ++n >= count) break; if (n % 20 == 0) dmon_header(); sleep(dmonDelay); }
    } else if (pmon) {
        pmon_header();
        while (gRunning) { query(&g, &ci); pmon_line(&g); fflush(stdout); if (count && ++n >= count) break; if (n % 20 == 0) pmon_header(); sleep(dmonDelay); }
    } else do {
        query(&g, &ci);
        if (fields) rc = print_query(&g, fields, header, units);
        else if (kv) { print_list(&g); if (loopMs) printf("\n"); }
        else if (full) print_q(&g, sections);
        else { if (loopMs) printf("\033[H\033[2J"); print_table(&g); }
        fflush(stdout);
        if (rc || !loopMs) break;
        header = 0;
        usleep((useconds_t)(loopMs * 1000));
    } while (gRunning);
    rm_close();
    return rc;
}
