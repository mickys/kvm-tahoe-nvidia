// nvidia-smi for macOS on NullMoth's NVIDIA driver: NVIDIA's nvidia-smi output, read-only, from the GPU's resource manager
// (GSP-RM) through the NVRM/NVAccel user client (the RM controls NVML uses on Linux) and the IORegistry.
//
//   nvidia-smi                          the default table (GPU, temperature, P-state, memory, utilization, processes)
//   nvidia-smi -L                       list GPUs with their UUID
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
// refreshed and nvidia-smi maps it read-only, as NVIDIA's NVML does. The fan level in % is not on that page: N/A.
// Build: build.sh next to this file.
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
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
static NV_STATUS rm_ctrl(NvHandle obj, NvU32 cmd, void *params, NvU32 size)
{
    NVOS54_PARAMETERS p = { .hClient = hClient, .hObject = obj, .cmd = cmd, .params = params, .paramsSize = size };
    if (esc(gCtl, NV_ESC_RM_CONTROL, &p, sizeof p)) return NV_ERR_GENERIC;
    if (p.status && gVerbose) fprintf(stderr, "control 0x%08x -> status 0x%x\n", cmd, p.status);
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
    char name[96], uuid[64], vbios[32], busId[32], driver[64], rmVersion[64], arch[32];
    unsigned devId, subsys;
    int temp, tempIoreg, pstate, dispActive;
    long long memTotal, memUsed, memFree, memReserved, bar1Total, bar1Used, busWidth;   // MiB / bits, NA if unknown
    int utilGpu, utilMem, utilEnc, utilDec, utilJpg, utilOfa;                          // %
    int clkGr, clkMem, clkVid;                                                         // MHz
    int pcieGenCur, pcieGenMax, pcieWidthCur, pcieWidthMax;
    int encSessions, encFps, encLatencyUs;
    long long powerInst, powerAvg, powerLimitReq, powerLimitEnf;                        // mW, NA if unknown
    int fanRpm[RUSD_FAN_COOLER_MAX_COOLERS], nFan, tempMem, tempBoard;
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

static void query(Gpu *g, const nv_ioctl_card_info_t *ci)
{
    memset(g, 0, sizeof *g);
    g->temp = g->tempIoreg = g->pstate = NA;
    g->memTotal = g->memUsed = g->memFree = g->memReserved = g->bar1Total = g->bar1Used = g->busWidth = NA;
    g->utilGpu = g->utilMem = g->utilEnc = g->utilDec = g->utilJpg = g->utilOfa = NA;
    g->clkGr = g->clkMem = g->clkVid = NA;
    g->pcieGenCur = g->pcieGenMax = g->pcieWidthCur = g->pcieWidthMax = NA;
    g->encSessions = g->encFps = g->encLatencyUs = NA;
    g->powerInst = g->powerAvg = g->powerLimitReq = g->powerLimitEnf = NA; g->tempMem = g->tempBoard = NA;

    g->devId = ci->pci_info.device_id; g->subsys = 0;
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
        th.successfulInstructions == 1 && th.instructionList[0].executed && th.instructionList[0].result == NV_OK)
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
        if (RUSD_READ_DATA(gRusd, temperatures[RUSD_TEMPERATURE_TYPE_MEMORY], &t) && t.temperature > 0) g->tempMem = NV_TYPES_NV_TEMP_TO_CELSIUS_ROUNDED(t.temperature);
        if (RUSD_READ_DATA(gRusd, temperatures[RUSD_TEMPERATURE_TYPE_BOARD], &t) && t.temperature > 0) g->tempBoard = NV_TYPES_NV_TEMP_TO_CELSIUS_ROUNDED(t.temperature);
        if (g->powerInst == NA && g->powerAvg != NA) g->powerInst = g->powerAvg;
    }

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
    snprintf(a, sizeof a, "%4s  %5s    %-4s", "N/A", temp, perf); cell(l, 41, a, pwr);
    if (g->memTotal != NA) snprintf(b, sizeof b, "%lldMiB / %6lldMiB", g->memUsed, g->memTotal);   // like NVIDIA since R535: without "Reserved" else snprintf(b, sizeof b, "N/A / N/A");
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

static int want(const char *sections, const char *s) { return !sections || strstr(sections, s); }
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
    printf("    Fan Speed                             : N/A\n");   // the level in % is not on the RM's shared data page
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
        #undef W
    }
    if (want(sections, "CLOCK")) {
        printf("    Clocks\n");
        printf("        Graphics                          : %s\n", num(x, sizeof x, g->clkGr, " MHz"));
        printf("        SM                                : %s\n", num(x, sizeof x, g->clkGr, " MHz"));
        printf("        Memory                            : %s\n", num(x, sizeof x, g->clkMem, " MHz"));
        printf("        Video                             : %s\n", num(x, sizeof x, g->clkVid, " MHz"));
    }
    if (want(sections, "PIDS")) {
        printf("    Processes\n");
        if (!g->nproc) printf("        None\n");
        for (int i = 0; i < g->nproc; i++) {
            char path[PROC_PIDPATHINFO_MAXSIZE]; proc_label(g->procs[i].pid, path, sizeof path);
            printf("        GPU instance ID                   : N/A\n        Compute instance ID               : N/A\n");
            printf("        Process ID                        : %u\n", g->procs[i].pid);
            printf("            Type                          : %s\n", g->procs[i].type == 'X' ? "C+G" : g->procs[i].type == 'C' ? "C" : "G");
            printf("            Name                          : %s\n", path);
            printf("            Used GPU Memory               : %s\n", num(x, sizeof x, g->procs[i].memMiB, " MiB"));
        }
    }
    printf("\n");
}

// --query-gpu fields: name, NVIDIA's unit, value (NA = "[N/A]")
typedef struct { const char *name, *unit; } Field;
static const Field kFields[] = {
    { "index", "" }, { "name", "" }, { "uuid", "" }, { "pci.bus_id", "" }, { "pci.device_id", "" }, { "driver_version", "" },
    { "vbios_version", "" }, { "display_active", "" }, { "pstate", "" }, { "temperature.gpu", "" },
    { "utilization.gpu", " %" }, { "utilization.memory", " %" }, { "utilization.encoder", " %" }, { "utilization.decoder", " %" },
    { "utilization.jpeg", " %" }, { "utilization.ofa", " %" },
    { "memory.total", " MiB" }, { "memory.reserved", " MiB" }, { "memory.used", " MiB" }, { "memory.free", " MiB" },
    { "clocks.gr", " MHz" }, { "clocks.sm", " MHz" }, { "clocks.mem", " MHz" }, { "clocks.video", " MHz" },
    { "pcie.link.gen.current", "" }, { "pcie.link.gen.max", "" }, { "pcie.link.width.current", "" }, { "pcie.link.width.max", "" },
    { "encoder.stats.sessionCount", "" }, { "encoder.stats.averageFps", "" }, { "encoder.stats.averageLatency", "" },
    { "fan.speed", " %" }, { "fan.rpm", " RPM" }, { "power.draw", " W" }, { "power.draw.average", " W" }, { "power.draw.instant", " W" },
    { "power.limit", " W" }, { "enforced.power.limit", " W" }, { "temperature.memory", "" },
};
static int field_value(const Gpu *g, const char *f, char *out, size_t n)
{
    long long v = NA; const char *s = NULL;
    if (!strcmp(f, "index")) v = 0; else if (!strcmp(f, "name")) s = g->name; else if (!strcmp(f, "uuid")) s = g->uuid;
    else if (!strcmp(f, "pci.bus_id")) s = g->busId; else if (!strcmp(f, "driver_version")) s = g->driver;
    else if (!strcmp(f, "vbios_version")) s = g->vbios; else if (!strcmp(f, "display_active")) s = g->dispActive ? "Enabled" : "Disabled";
    else if (!strcmp(f, "pci.device_id")) { snprintf(out, n, "0x%04X10DE", g->devId); return 0; }
    else if (!strcmp(f, "pstate")) { if (g->pstate == NA) s = "[N/A]"; else { snprintf(out, n, "P%d", g->pstate); return 0; } }
    else if (!strcmp(f, "temperature.gpu")) v = g->temp;
    else if (!strcmp(f, "utilization.gpu")) v = g->utilGpu; else if (!strcmp(f, "utilization.memory")) v = g->utilMem;
    else if (!strcmp(f, "utilization.encoder")) v = g->utilEnc; else if (!strcmp(f, "utilization.decoder")) v = g->utilDec;
    else if (!strcmp(f, "utilization.jpeg")) v = g->utilJpg; else if (!strcmp(f, "utilization.ofa")) v = g->utilOfa;
    else if (!strcmp(f, "memory.total")) v = g->memTotal; else if (!strcmp(f, "memory.reserved")) v = g->memReserved;
    else if (!strcmp(f, "memory.used")) v = g->memUsed; else if (!strcmp(f, "memory.free")) v = g->memFree;
    else if (!strcmp(f, "clocks.gr") || !strcmp(f, "clocks.sm")) v = g->clkGr; else if (!strcmp(f, "clocks.mem")) v = g->clkMem;
    else if (!strcmp(f, "clocks.video")) v = g->clkVid;
    else if (!strcmp(f, "pcie.link.gen.current")) v = g->pcieGenCur; else if (!strcmp(f, "pcie.link.gen.max")) v = g->pcieGenMax;
    else if (!strcmp(f, "pcie.link.width.current")) v = g->pcieWidthCur; else if (!strcmp(f, "pcie.link.width.max")) v = g->pcieWidthMax;
    else if (!strcmp(f, "encoder.stats.sessionCount")) v = g->encSessions; else if (!strcmp(f, "encoder.stats.averageFps")) v = g->encFps;
    else if (!strcmp(f, "encoder.stats.averageLatency")) v = g->encLatencyUs;
    else if (!strcmp(f, "fan.speed")) s = "[N/A]";
    else if (!strcmp(f, "fan.rpm")) v = g->nFan ? g->fanRpm[0] : NA;
    else if (!strcmp(f, "temperature.memory")) v = g->tempMem;
    else if (!strcmp(f, "power.draw") || !strcmp(f, "power.draw.instant") || !strcmp(f, "power.draw.average") || !strcmp(f, "power.limit") || !strcmp(f, "enforced.power.limit")) {
        const long long mw = !strcmp(f, "power.draw.average") ? g->powerAvg : !strncmp(f, "power.draw", 10) ? g->powerInst
                           : !strcmp(f, "power.limit") && g->powerLimitReq != NA ? g->powerLimitReq : g->powerLimitEnf;
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

static void usage(void)
{
    fprintf(stderr, "usage: nvidia-smi [-L] [-q [-d SECTIONS]] [--query-gpu=FIELDS --format=csv[,noheader][,nounits]] [dmon [-c N]]\n"
                    "                  [-l [SEC] | -lms [MS]] [-v] [--help-query-gpu]\n");
}

int main(int argc, char **argv)
{
    int list = 0, full = 0, dmon = 0, count = 0, header = 1, units = 1; long loopMs = 0;
    const char *sections = NULL, *fields = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-L") || !strcmp(a, "--list-gpus")) list = 1;
        else if (!strcmp(a, "-q") || !strcmp(a, "--query")) full = 1;
        else if ((!strcmp(a, "-d") || !strcmp(a, "--display")) && i + 1 < argc) sections = argv[++i];
        else if (!strncmp(a, "--display=", 10)) sections = a + 10;
        else if (!strcmp(a, "dmon")) dmon = 1;
        else if (!strcmp(a, "-c") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strncmp(a, "--query-gpu=", 12)) fields = a + 12;
        else if (!strncmp(a, "--format=", 9)) { if (strstr(a, "noheader")) header = 0; if (strstr(a, "nounits")) units = 0; }
        else if (!strcmp(a, "-l") || !strcmp(a, "--loop")) { loopMs = 1000; if (i + 1 < argc && atoi(argv[i + 1]) > 0) loopMs = 1000L * atoi(argv[++i]); }
        else if (!strncmp(a, "--loop=", 7)) loopMs = 1000L * (atoi(a + 7) > 0 ? atoi(a + 7) : 1);
        else if (!strcmp(a, "-lms") || !strcmp(a, "--loop-ms")) { loopMs = 1000; if (i + 1 < argc && atoi(argv[i + 1]) > 0) loopMs = atoi(argv[++i]); }
        else if (!strcmp(a, "-v")) gVerbose = 1;
        else if (!strcmp(a, "--help-query-gpu")) { for (unsigned k = 0; k < sizeof kFields / sizeof kFields[0]; k++) printf("\"%s\"\n", kFields[k].name); return 0; }
        else { usage(); return 2; }
    }
    nv_ioctl_card_info_t ci;
    if (rm_open(&ci)) { rm_close(); return 9; }
    static Gpu g;
    int rc = 0, n = 0;
    if (list) { query(&g, &ci); printf("GPU 0: %s (UUID: %s)\n", g.name, g.uuid); }
    else if (dmon) {
        dmon_header();
        do { query(&g, &ci); dmon_line(&g); fflush(stdout); if (count && ++n >= count) break; sleep(1); } while (1);
    } else do {
        query(&g, &ci);
        if (fields) rc = print_query(&g, fields, header, units);
        else if (full) print_q(&g, sections);
        else { if (loopMs) printf("\033[H\033[2J"); print_table(&g); }
        fflush(stdout);
        if (rc || !loopMs) break;
        header = 0;   // like nvidia-smi: the csv header once
        usleep((useconds_t)(loopMs * 1000));
    } while (1);
    rm_close();
    return rc;
}
