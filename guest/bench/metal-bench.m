// metal-bench.m: quick Metal throughput numbers for the default device (RTX 4090 through nullmoth's driver).
//   clang -fobjc-arc -O2 -o metal-bench metal-bench.m -framework Metal -framework Foundation && ./metal-bench
// Each test checks its result, so a fast but wrong driver shows up as FAIL instead of a good number.
// RTX 4090 reference (NVIDIA spec): 82.6 TFLOPS FP32, 1008 GB/s VRAM, PCIe 4.0 x8 here (~13 GB/s practical).
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <mach/mach_time.h>

static NSString *const kSrc = @R"(
#include <metal_stdlib>
using namespace metal;
kernel void fma_loop(device float *out [[buffer(0)]], constant uint &iters [[buffer(1)]], uint id [[thread_position_in_grid]]) {
    float a = 1.0f + id * 1e-7f, b = 0.999f, c = 0.001f, d = 1.0001f, e = 0.5f, f = 0.25f, g = 2.0f, h = 0.75f;
    for (uint i = 0; i < iters; i++) {           // 8 independent chains, 16 FMAs per iteration = 32 FLOP
        a = fma(a, b, c); d = fma(d, b, c); e = fma(e, b, c); f = fma(f, b, c);
        g = fma(g, b, c); h = fma(h, b, c); b = fma(b, 1.0f, 0.0f); c = fma(c, 1.0f, 0.0f);
        a = fma(a, b, c); d = fma(d, b, c); e = fma(e, b, c); f = fma(f, b, c);
        g = fma(g, b, c); h = fma(h, b, c); b = fma(b, 1.0f, 0.0f); c = fma(c, 1.0f, 0.0f);
    }
    out[id] = a + d + e + f + g + h;
}
kernel void copy4(device const float4 *src [[buffer(0)]], device float4 *dst [[buffer(1)]], uint id [[thread_position_in_grid]]) {
    dst[id] = src[id];
}
)";

static double now_s(void) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

int main(int argc, char **argv) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) { printf("no Metal device\n"); return 1; }
    printf("device  %s | recommendedMaxWorkingSet %.1f GB | maxBuffer %.1f GB | unified %d\n", dev.name.UTF8String,
           dev.recommendedMaxWorkingSetSize / 1e9, dev.maxBufferLength / 1e9, dev.hasUnifiedMemory);
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&err];
    if (!lib) { printf("compile FAILED: %s\n", err.description.UTF8String); return 1; }
    id<MTLComputePipelineState> pFma = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fma_loop"] error:&err];
    id<MTLComputePipelineState> pCopy = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"copy4"] error:&err];
    if (!pFma || !pCopy) { printf("pipeline FAILED: %s\n", err.description.UTF8String); return 1; }
    id<MTLCommandQueue> q = [dev newCommandQueue];

    // GPU time of one command buffer (falls back to wall time when the driver reports no GPU timestamps)
    double (^run)(void (^)(id<MTLCommandBuffer>)) = ^double(void (^enc)(id<MTLCommandBuffer>)) {
        id<MTLCommandBuffer> cb = [q commandBuffer]; enc(cb);
        double t0 = now_s(); [cb commit]; [cb waitUntilCompleted]; double wall = now_s() - t0;
        if (cb.status != MTLCommandBufferStatusCompleted) { printf("  command buffer status %ld error %s\n", (long)cb.status, cb.error.description.UTF8String); return -1; }
        double gpu = cb.GPUEndTime - cb.GPUStartTime;
        return gpu > 0 ? gpu : wall;
    };
    double (^best)(int, double (^)(void)) = ^double(int n, double (^f)(void)) {
        double b = 1e9; for (int i = 0; i < n; i++) { double t = f(); if (t < 0) return -1; if (t < b) b = t; } return b;
    };

    // 1. FP32 FMA throughput
    {
        const NSUInteger threads = 1u << 22; uint32_t iters = 4096;
        id<MTLBuffer> out = [dev newBufferWithLength:threads * 4 options:MTLResourceStorageModeShared];
        double t = best(5, ^double{ return run(^(id<MTLCommandBuffer> cb) {
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder]; [e setComputePipelineState:pFma];
            [e setBuffer:out offset:0 atIndex:0]; [e setBytes:&iters length:4 atIndex:1];
            [e dispatchThreads:MTLSizeMake(threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)]; [e endEncoding]; }); });
        // reference value for thread 0, computed on the CPU the same way
        float a = 1.0f, b = 0.999f, c = 0.001f, d = 1.0001f, e = 0.5f, f = 0.25f, g = 2.0f, h = 0.75f;
        for (uint32_t i = 0; i < iters * 2; i++) { a = fmaf(a, b, c); d = fmaf(d, b, c); e = fmaf(e, b, c); f = fmaf(f, b, c);
            g = fmaf(g, b, c); h = fmaf(h, b, c); b = fmaf(b, 1.0f, 0.0f); c = fmaf(c, 1.0f, 0.0f); }
        float want = a + d + e + f + g + h, got = ((float *)out.contents)[0];
        bool ok = t > 0 && fabsf(got - want) <= 1e-4f * fabsf(want);
        printf("fp32 fma      %8.2f TFLOPS   (%.2f ms, check %s: %g vs %g)\n", ok ? threads * (double)iters * 32 / t / 1e12 : 0, t * 1e3,
               ok ? "ok" : "FAIL", got, want);
    }

    // 2. VRAM bandwidth: kernel copy and blit copy between private buffers (read + write counted)
    {
        const NSUInteger bytes = 1ull << 30;
        id<MTLBuffer> a = [dev newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
        id<MTLBuffer> b = [dev newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
        id<MTLBuffer> seed = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        uint32_t *s = seed.contents; for (NSUInteger i = 0; i < bytes / 4; i++) s[i] = (uint32_t)(i * 2654435761u);
        run(^(id<MTLCommandBuffer> cb) { id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            [e copyFromBuffer:seed sourceOffset:0 toBuffer:a destinationOffset:0 size:bytes]; [e endEncoding]; });
        double tk = best(5, ^double{ return run(^(id<MTLCommandBuffer> cb) {
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder]; [e setComputePipelineState:pCopy];
            [e setBuffer:a offset:0 atIndex:0]; [e setBuffer:b offset:0 atIndex:1];
            [e dispatchThreads:MTLSizeMake(bytes / 16, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)]; [e endEncoding]; }); });
        double tb = best(5, ^double{ return run(^(id<MTLCommandBuffer> cb) { id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            [e copyFromBuffer:b sourceOffset:0 toBuffer:a destinationOffset:0 size:bytes]; [e endEncoding]; }); });
        // read back a and compare with the seed (a <- b <- a, so a must still equal the seed)
        id<MTLBuffer> back = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        run(^(id<MTLCommandBuffer> cb) { id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            [e copyFromBuffer:a sourceOffset:0 toBuffer:back destinationOffset:0 size:bytes]; [e endEncoding]; });
        bool ok = memcmp(back.contents, seed.contents, bytes) == 0;
        printf("vram kernel   %8.1f GB/s     (1 GiB copy, %.2f ms)\n", tk > 0 ? 2.0 * bytes / tk / 1e9 : 0, tk * 1e3);
        printf("vram blit     %8.1f GB/s     (1 GiB copy, %.2f ms, data check %s)\n", tb > 0 ? 2.0 * bytes / tb / 1e9 : 0, tb * 1e3, ok ? "ok" : "FAIL");

        // 3. PCIe: blit between shared (system RAM) and private (VRAM)
        double up = best(3, ^double{ return run(^(id<MTLCommandBuffer> cb) { id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            [e copyFromBuffer:seed sourceOffset:0 toBuffer:b destinationOffset:0 size:bytes]; [e endEncoding]; }); });
        double down = best(3, ^double{ return run(^(id<MTLCommandBuffer> cb) { id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            [e copyFromBuffer:b sourceOffset:0 toBuffer:back destinationOffset:0 size:bytes]; [e endEncoding]; }); });
        printf("upload        %8.1f GB/s     (shared -> private, 1 GiB, %.1f ms)\n", up > 0 ? bytes / up / 1e9 : 0, up * 1e3);
        printf("download      %8.1f GB/s     (private -> shared, 1 GiB, %.1f ms)\n", down > 0 ? bytes / down / 1e9 : 0, down * 1e3);
    }

    // 4. Dispatch latency: empty-ish command buffers back to back
    {
        id<MTLBuffer> out = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared]; uint32_t iters = 1;
        const int n = 200; double t0 = now_s();
        for (int i = 0; i < n; i++) run(^(id<MTLCommandBuffer> cb) {
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder]; [e setComputePipelineState:pFma];
            [e setBuffer:out offset:0 atIndex:0]; [e setBytes:&iters length:4 atIndex:1];
            [e dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)]; [e endEncoding]; });
        printf("round trip    %8.1f us       (commit + waitUntilCompleted of a tiny dispatch, mean of %d)\n", (now_s() - t0) / n * 1e6, n);
    }
    return 0;
} }
