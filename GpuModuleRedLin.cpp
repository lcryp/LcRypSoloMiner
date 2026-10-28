// ============================================================
// LcRyp Solo Miner - GPU Module Red Linear (AMD OpenCL)
// https://lcryp.com
// Version 2.2
// ============================================================

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════════
// SHARED MEMORY STRUCTURE (between processes!)
// ═══════════════════════════════════════════════════════════════════════════
struct SharedInitState {
    volatile LONG initialized;
    volatile LONG init_in_progress;
};

static HANDLE g_shared_mem = NULL;
static SharedInitState* g_shared_state = NULL;
static HANDLE g_global_mutex = NULL;

// ═══════════════════════════════════════════════════════════════════════════
// OpenCL TYPES
// ═══════════════════════════════════════════════════════════════════════════
typedef void* cl_platform_id;
typedef void* cl_device_id;
typedef void* cl_context;
typedef void* cl_command_queue;
typedef void* cl_program;
typedef void* cl_kernel;
typedef void* cl_mem;
typedef int32_t cl_int;
typedef uint32_t cl_uint;
typedef uint64_t cl_bitfield;

#define CL_SUCCESS 0
#define CL_DEVICE_TYPE_GPU (1 << 2)
#define CL_MEM_READ_ONLY (1 << 2)
#define CL_MEM_WRITE_ONLY (1 << 1)
#define CL_TRUE 1
#define CL_DEVICE_NAME 0x102B
#define CL_DEVICE_VENDOR 0x102C

typedef cl_int (*t_clGetPlatformIDs)(cl_uint, cl_platform_id*, cl_uint*);
typedef cl_int (*t_clGetDeviceIDs)(cl_platform_id, cl_bitfield, cl_uint, cl_device_id*, cl_uint*);
typedef cl_int (*t_clGetDeviceInfo)(cl_device_id, cl_uint, size_t, void*, size_t*);
typedef cl_context (*t_clCreateContext)(const cl_bitfield*, cl_uint, const cl_device_id*, void*, void*, cl_int*);
typedef cl_command_queue (*t_clCreateCommandQueue)(cl_context, cl_device_id, cl_bitfield, cl_int*);
typedef cl_program (*t_clCreateProgramWithSource)(cl_context, cl_uint, const char**, const size_t*, cl_int*);
typedef cl_int (*t_clBuildProgram)(cl_program, cl_uint, const cl_device_id*, const char*, void*, void*);
typedef cl_kernel (*t_clCreateKernel)(cl_program, const char*, cl_int*);
typedef cl_mem (*t_clCreateBuffer)(cl_context, cl_bitfield, size_t, void*, cl_int*);
typedef cl_int (*t_clSetKernelArg)(cl_kernel, cl_uint, size_t, const void*);
typedef cl_int (*t_clEnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const size_t*, const size_t*, const size_t*, cl_uint, const void*, void*);
typedef cl_int (*t_clEnqueueWriteBuffer)(cl_command_queue, cl_mem, cl_uint, size_t, size_t, const void*, cl_uint, const void*, void*);
typedef cl_int (*t_clEnqueueReadBuffer)(cl_command_queue, cl_mem, cl_uint, size_t, size_t, void*, cl_uint, const void*, void*);
typedef cl_int (*t_clFinish)(cl_command_queue);
typedef cl_int (*t_clReleaseMemObject)(cl_mem);
typedef cl_int (*t_clReleaseKernel)(cl_kernel);
typedef cl_int (*t_clReleaseProgram)(cl_program);
typedef cl_int (*t_clReleaseCommandQueue)(cl_command_queue);
typedef cl_int (*t_clReleaseContext)(cl_context);

t_clGetPlatformIDs p_clGetPlatformIDs;
t_clGetDeviceIDs p_clGetDeviceIDs;
t_clGetDeviceInfo p_clGetDeviceInfo;
t_clCreateContext p_clCreateContext;
t_clCreateCommandQueue p_clCreateCommandQueue;
t_clCreateProgramWithSource p_clCreateProgramWithSource;
t_clBuildProgram p_clBuildProgram;
t_clCreateKernel p_clCreateKernel;
t_clCreateBuffer p_clCreateBuffer;
t_clSetKernelArg p_clSetKernelArg;
t_clEnqueueNDRangeKernel p_clEnqueueNDRangeKernel;
t_clEnqueueWriteBuffer p_clEnqueueWriteBuffer;
t_clEnqueueReadBuffer p_clEnqueueReadBuffer;
t_clFinish p_clFinish;
t_clReleaseMemObject p_clReleaseMemObject;
t_clReleaseKernel p_clReleaseKernel;
t_clReleaseProgram p_clReleaseProgram;
t_clReleaseCommandQueue p_clReleaseCommandQueue;
t_clReleaseContext p_clReleaseContext;

// ═══════════════════════════════════════════════════════════════════════════
// KERNEL
// ═══════════════════════════════════════════════════════════════════════════
const char* opencl_source = R"(

#pragma OPENCL EXTENSION cl_khr_byte_addressable_store : enable
#pragma OPENCL EXTENSION cl_amd_media_ops : enable
#pragma OPENCL EXTENSION cl_amd_media_ops2 : enable

#define ROR(x, n) rotate(x, 32U-(n))
#define S0(x) (ROR(x, 2) ^ ROR(x, 13) ^ ROR(x, 22))
#define S1(x) (ROR(x, 6) ^ ROR(x, 11) ^ ROR(x, 25))
#define s0(x) (ROR(x, 7) ^ ROR(x, 18) ^ ((x) >> 3))
#define s1(x) (ROR(x, 17) ^ ROR(x, 19) ^ ((x) >> 10))
#define CH(x, y, z) bitselect(z, y, x)
#define MAJ(x, y, z) bitselect(x, y, z ^ x)

inline uint bswap32(uint x) {
    //return (x >> 24) | ((x >> 8) & 0xFF00) | ((x << 8) & 0xFF0000) | (x << 24);
    return as_uint(as_uchar4(x).wzyx);
}

__constant uint K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

void sha256_round(uint* state, uint* w) {
    uint a = state[0], b = state[1], c = state[2], d = state[3];
    uint e = state[4], f = state[5], g = state[6], h = state[7];
    #pragma unroll
    for (int i = 16; i < 64; i++) {
        w[i] = s1(w[i-2]) + w[i-7] + s0(w[i-15]) + w[i-16];
    }
    #pragma unroll
    for (int i = 0; i < 64; i++) {
        uint t1 = h + S1(e) + CH(e, f, g) + K[i] + w[i];
        uint t2 = S0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

__kernel void sha256_kernel(__global const uint* header, uint seed, __global const uint* target, __global uint* result, uint iters) {
    uint tid = get_global_id(0);
    uint nonce = seed + tid;

    __private uint pre_w[16];
    __private uint priv_target[8];

    #pragma unroll 16
    for (int j = 0; j < 16; j++) pre_w[j] = bswap32(header[j]);

    #pragma unroll 8
    for (int j = 0; j < 8; j++) priv_target[j] = target[j];

    const uint H_INIT[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };

    uint w[64];

    for (uint i = 0; i < iters; i++)
    {
        nonce ^= nonce << 13u;
        nonce ^= nonce >> 17u;
        nonce ^= nonce << 5u;

        // 1. First SHA-256 (64 bytes of header)
        uint state1[8] = {H_INIT[0], H_INIT[1], H_INIT[2], H_INIT[3],
                          H_INIT[4], H_INIT[5], H_INIT[6], H_INIT[7]};

        #pragma unroll 16
        for (int j = 0; j < 16; j++) w[j] = pre_w[j];

        sha256_round(state1, w);

        // 2. Second SHA-256 (last chunk + nonce + padding)
        uint state2[8];
        #pragma unroll 8
        for (int j = 0; j < 8; j++) state2[j] = state1[j];

        uint w2[64] = {0};
        w2[0]  = bswap32(header[16]);
        w2[1]  = bswap32(header[17]);
        w2[2]  = bswap32(header[18]);
        w2[3]  = bswap32(nonce);
        w2[4]  = 0x80000000u;
        w2[15] = 640u;

        sha256_round(state2, w2);

        // 3. Final SHA-256 (hash of second round)
        uint final_state[8] = {H_INIT[0], H_INIT[1], H_INIT[2], H_INIT[3],
                               H_INIT[4], H_INIT[5], H_INIT[6], H_INIT[7]};

        #pragma unroll 8
        for (int j = 0; j < 8; j++) w2[j] = state2[j];
        w2[8]  = 0x80000000u;
        w2[15] = 256u;

        sha256_round(final_state, w2);

        // Check
        bool matched = true;
        #pragma unroll 8
        for (int j = 7; j >= 0; j--)
        {
            uint val = bswap32(final_state[j]);
            if (val < priv_target[j]) break;
            if (val > priv_target[j]) { matched = false; break; }
        }

        if (matched)
        {
            atomic_xchg(result, nonce);
            return;
        }
    }
}
)";

// ═══════════════════════════════════════════════════════════════════════════
// LOCAL STATE (separate in each process)
// ═══════════════════════════════════════════════════════════════════════════
static bool g_local_opencl_loaded = false;
static std::vector<cl_device_id> g_amd_devices;

struct GpuContext {
    cl_context ctx;
    cl_command_queue queue;
    cl_program program;
    cl_kernel kernel;
    cl_mem d_h, d_t, d_r;
};

static std::map<int, GpuContext> g_gpu_map;

// ═══════════════════════════════════════════════════════════════════════════
// INIT OpenCL (WITH SHARED MEMORY SYNC!)
// ═══════════════════════════════════════════════════════════════════════════
bool init_opencl() {
    if (g_local_opencl_loaded) {
        return true;
    }
    if (g_shared_state->initialized == 1) {
        goto load_functions;
    }
    LONG prev_val = InterlockedCompareExchange(&g_shared_state->init_in_progress, 1, 0);
    if (prev_val != 0) {
        while (g_shared_state->initialized == 0) {
            Sleep(50);
        }
        goto load_functions;
    }

load_functions:
    HMODULE hCL = LoadLibraryA("OpenCL.dll");
    if (!hCL) {
        InterlockedExchange(&g_shared_state->init_in_progress, 0);
        return false;
    }
    #define LOAD(name) p_##name = (t_##name)GetProcAddress(hCL, #name); if (!p_##name) { InterlockedExchange(&g_shared_state->init_in_progress, 0); return false; }
    LOAD(clGetPlatformIDs); LOAD(clGetDeviceIDs); LOAD(clGetDeviceInfo);
    LOAD(clCreateContext); LOAD(clCreateCommandQueue); LOAD(clCreateProgramWithSource);
    LOAD(clBuildProgram); LOAD(clCreateKernel); LOAD(clCreateBuffer);
    LOAD(clSetKernelArg); LOAD(clEnqueueNDRangeKernel); LOAD(clEnqueueWriteBuffer);
    LOAD(clEnqueueReadBuffer); LOAD(clFinish); LOAD(clReleaseMemObject);
    LOAD(clReleaseKernel); LOAD(clReleaseProgram); LOAD(clReleaseCommandQueue);
    LOAD(clReleaseContext);
    if (g_shared_state->initialized == 1) {
        goto collect_devices;
    }

collect_devices:
    cl_uint num_platforms;
    cl_int cl_err = p_clGetPlatformIDs(0, NULL, &num_platforms);
    if (cl_err != CL_SUCCESS || num_platforms == 0) {
        InterlockedExchange(&g_shared_state->init_in_progress, 0);
        return false;
    }
    std::vector<cl_platform_id> platforms(num_platforms);
    p_clGetPlatformIDs(num_platforms, &platforms[0], NULL);
    for (cl_uint p = 0; p < num_platforms; p++) {
        cl_uint num_dev;
        cl_err = p_clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_GPU, 0, NULL, &num_dev);
        if (cl_err == CL_SUCCESS && num_dev > 0) {
            std::vector<cl_device_id> temp_devices(num_dev);
            p_clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_GPU, num_dev, &temp_devices[0], NULL);
            for (cl_uint d = 0; d < num_dev; d++) {
                char vendor[256] = {0};
                char name[256] = {0};
                p_clGetDeviceInfo(temp_devices[d], CL_DEVICE_VENDOR, sizeof(vendor), vendor, NULL);
                p_clGetDeviceInfo(temp_devices[d], CL_DEVICE_NAME, sizeof(name), name, NULL);
                if (strstr(vendor, "Advanced Micro Devices") || 
                    strstr(vendor, "AMD") || 
                    strstr(vendor, "ATI")) {
                    int virtual_id = (int)g_amd_devices.size();
                    g_amd_devices.push_back(temp_devices[d]);
                }
            }
        }
    }
    if (g_amd_devices.empty()) {
        InterlockedExchange(&g_shared_state->init_in_progress, 0);
        return false;
    }
    if (g_shared_state->initialized == 0) {
        InterlockedExchange(&g_shared_state->initialized, 1);
        InterlockedExchange(&g_shared_state->init_in_progress, 0);
    }
    g_local_opencl_loaded = true;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// INIT DEVICE
// ═══════════════════════════════════════════════════════════════════════════
bool init_device(int internal_id) {
    if (g_gpu_map.find(internal_id) != g_gpu_map.end()) {
        return true;
    }
    
    if (internal_id < 0 || internal_id >= (int)g_amd_devices.size()) {
        return false;
    }
    GpuContext gpu = {0};
    cl_int err;
    gpu.ctx = p_clCreateContext(NULL, 1, &g_amd_devices[internal_id], NULL, NULL, &err);
    if (err != CL_SUCCESS) {
        return false;
    }
    gpu.queue = p_clCreateCommandQueue(gpu.ctx, g_amd_devices[internal_id], 0, &err);
    if (err != CL_SUCCESS) {
        p_clReleaseContext(gpu.ctx);
        return false;
    }
    size_t src_len = strlen(opencl_source);
    gpu.program = p_clCreateProgramWithSource(gpu.ctx, 1, &opencl_source, &src_len, &err);
    if (err != CL_SUCCESS) {
        p_clReleaseCommandQueue(gpu.queue);
        p_clReleaseContext(gpu.ctx);
        return false;
    }
    const char* build_options = "-cl-std=CL1.2 -cl-fast-relaxed-math -cl-mad-enable -cl-no-signed-zeros -cl-unsafe-math-optimizations -cl-finite-math-only";
    if (p_clBuildProgram(gpu.program, 1, &g_amd_devices[internal_id], build_options, NULL, NULL) != CL_SUCCESS) {
        p_clReleaseProgram(gpu.program);
        p_clReleaseCommandQueue(gpu.queue);
        p_clReleaseContext(gpu.ctx);
        return false;
    }
    gpu.kernel = p_clCreateKernel(gpu.program, "sha256_kernel", &err);
    if (err != CL_SUCCESS) {
        p_clReleaseProgram(gpu.program);
        p_clReleaseCommandQueue(gpu.queue);
        p_clReleaseContext(gpu.ctx);
        return false;
    }
    gpu.d_h = p_clCreateBuffer(gpu.ctx, CL_MEM_READ_ONLY, 80, NULL, &err);
    gpu.d_t = p_clCreateBuffer(gpu.ctx, CL_MEM_READ_ONLY, 32, NULL, &err);
    gpu.d_r = p_clCreateBuffer(gpu.ctx, CL_MEM_WRITE_ONLY, 4, NULL, &err);
    if (err != CL_SUCCESS) {
        p_clReleaseKernel(gpu.kernel);
        p_clReleaseProgram(gpu.program);
        p_clReleaseCommandQueue(gpu.queue);
        p_clReleaseContext(gpu.ctx);
        return false;
    }
    g_gpu_map[internal_id] = gpu;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// MAIN ENTRY POINT
// ═══════════════════════════════════════════════════════════════════════════
extern "C" __declspec(dllexport) uint32_t run_mining_batch_on_device(
    int device_id,
    uint32_t* header_in,
    uint32_t start_nonce,
    uint32_t gpu_bpg,
    uint32_t gpu_tpb,
    uint32_t iterations,
    uint32_t* target
) {
    int internal_id = device_id;
    if (!g_local_opencl_loaded) {
        if (!init_opencl()) return 0;
    }
    if (!init_device(internal_id)) {
        return 0;
    }
    GpuContext& g = g_gpu_map[internal_id];
    uint32_t res = 0;
    p_clEnqueueWriteBuffer(g.queue, g.d_r, CL_TRUE, 0, 4, &res, 0, NULL, NULL);
    p_clEnqueueWriteBuffer(g.queue, g.d_h, CL_TRUE, 0, 80, header_in, 0, NULL, NULL);
    p_clEnqueueWriteBuffer(g.queue, g.d_t, CL_TRUE, 0, 32, target, 0, NULL, NULL);
    p_clSetKernelArg(g.kernel, 0, sizeof(cl_mem), &g.d_h);
    p_clSetKernelArg(g.kernel, 1, sizeof(uint32_t), &start_nonce);
    p_clSetKernelArg(g.kernel, 2, sizeof(cl_mem), &g.d_t);
    p_clSetKernelArg(g.kernel, 3, sizeof(cl_mem), &g.d_r);
    p_clSetKernelArg(g.kernel, 4, sizeof(uint32_t), &iterations);
    size_t global = gpu_bpg * gpu_tpb;
    size_t local = gpu_tpb;
    if (p_clEnqueueNDRangeKernel(g.queue, g.kernel, 1, NULL, &global, &local, 0, NULL, NULL) == CL_SUCCESS) {
        p_clFinish(g.queue);
        p_clEnqueueReadBuffer(g.queue, g.d_r, CL_TRUE, 0, 4, &res, 0, NULL, NULL);
    }
    return res;
}

// ═══════════════════════════════════════════════════════════════════════════
// CLEANUP
// ═══════════════════════════════════════════════════════════════════════════
extern "C" __declspec(dllexport) void cleanup_device(int dev_id) {
    int internal_id = dev_id;
    auto it = g_gpu_map.find(internal_id);
    if (it != g_gpu_map.end()) {
        GpuContext& g = it->second;
        if (p_clReleaseMemObject) {
            p_clReleaseMemObject(g.d_r);
            p_clReleaseMemObject(g.d_t);
            p_clReleaseMemObject(g.d_h);
        }
        if (p_clReleaseKernel) p_clReleaseKernel(g.kernel);
        if (p_clReleaseProgram) p_clReleaseProgram(g.program);
        if (p_clReleaseCommandQueue) p_clReleaseCommandQueue(g.queue);
        if (p_clReleaseContext) p_clReleaseContext(g.ctx);
        g_gpu_map.erase(it);
    }
}

extern "C" __declspec(dllexport) void cleanup_all_devices() {
    std::vector<int> ids;
    for (auto& kv : g_gpu_map) ids.push_back(kv.first);
    for (int id : ids) cleanup_device(id);
}

// ═══════════════════════════════════════════════════════════════════════════
// DLL MAIN
// ═══════════════════════════════════════════════════════════════════════════
BOOL APIENTRY DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_shared_mem = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(SharedInitState), "Global\\AmdMinerSharedState");
        if (!g_shared_mem) {
            g_shared_mem = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(SharedInitState), "AmdMinerSharedState");
        }
        if (g_shared_mem) {
            g_shared_state = (SharedInitState*)MapViewOfFile(g_shared_mem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedInitState));
            if (g_shared_state) {
                bool first_process = (GetLastError() != ERROR_ALREADY_EXISTS);
                if (first_process) {
                    g_shared_state->initialized = 0;
                    g_shared_state->init_in_progress = 0;
                }
            }
        }
        g_global_mutex = CreateMutexA(NULL, FALSE, "Global\\AmdGpuMinerMutex");
        if (!g_global_mutex) {
            g_global_mutex = CreateMutexA(NULL, FALSE, "AmdGpuMinerMutex");
        }
    }
    else if (reason == DLL_PROCESS_DETACH) {
        cleanup_all_devices();
        if (g_shared_state) {
            UnmapViewOfFile(g_shared_state);
        }
        if (g_shared_mem) {
            CloseHandle(g_shared_mem);
        }
        if (g_global_mutex) {
            CloseHandle(g_global_mutex);
        }
    }
    return TRUE;
}
