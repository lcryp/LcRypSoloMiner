// ============================================================
// LcRyp Solo Miner - GPU Module Green (NVIDIA)
// https://lcryp.com
// Version 2.2
// ============================================================

#include <windows.h>
#include <stdint.h>
#include <map>
#include <vector>
#include <time.h>

// CUDA API types
typedef void* CUdeviceptr;
typedef void* CUcontext;
typedef void* CUmodule;
typedef void* CUfunction;
typedef int CUdevice;
typedef void* nvrtcProgram;

typedef int (*t_cuInit)(unsigned int);
typedef int (*t_cuDeviceGet)(CUdevice*, int);
typedef int (*t_cuCtxCreate)(CUcontext*, unsigned int, CUdevice);
typedef int (*t_cuCtxSetCurrent)(CUcontext);
typedef int (*t_cuModuleLoadData)(CUmodule*, const void*);
typedef int (*t_cuModuleGetFunction)(CUfunction*, CUmodule, const char*);
typedef int (*t_cuMemAlloc)(CUdeviceptr*, size_t);
typedef int (*t_cuMemcpyHtoD)(CUdeviceptr, const void*, size_t);
typedef int (*t_cuMemcpyDtoH)(void*, CUdeviceptr, size_t);
typedef int (*t_cuLaunchKernel)(CUfunction, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, void*, void**, void**);
typedef int (*t_cuDeviceGetAttribute)(int*, int, CUdevice);
typedef int (*t_cuCtxSynchronize)();
typedef int (*t_cuMemFree)(CUdeviceptr);
typedef int (*t_nvrtcCreateProgram)(nvrtcProgram*, const char*, const char*, int, const char**, const char**);
typedef int (*t_nvrtcCompileProgram)(nvrtcProgram, int, const char**);
typedef int (*t_nvrtcGetPTXSize)(nvrtcProgram, size_t*);
typedef int (*t_nvrtcGetPTX)(nvrtcProgram, char*);
typedef int (*t_nvrtcDestroyProgram)(nvrtcProgram);

// Function pointers
t_cuInit p_cuInit; t_cuDeviceGet p_cuDeviceGet; t_cuCtxCreate p_cuCtxCreate;
t_cuCtxSetCurrent p_cuCtxSetCurrent; t_cuModuleLoadData p_cuModuleLoadData;
t_cuModuleGetFunction p_cuModuleGetFunction; t_cuMemAlloc p_cuMemAlloc;
t_cuMemcpyHtoD p_cuMemcpyHtoD; t_cuMemcpyDtoH p_cuMemcpyDtoH;
t_cuLaunchKernel p_cuLaunchKernel; t_cuDeviceGetAttribute p_cuDeviceGetAttribute;
t_cuCtxSynchronize p_cuCtxSynchronize; t_cuMemFree p_cuMemFree;
t_nvrtcCreateProgram p_nvrtcCreateProgram; t_nvrtcCompileProgram p_nvrtcCompileProgram;
t_nvrtcGetPTXSize p_nvrtcGetPTXSize; t_nvrtcGetPTX p_nvrtcGetPTX; t_nvrtcDestroyProgram p_nvrtcDestroyProgram;

const char* cuda_source = R"(
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

__device__ __forceinline__ uint32_t bswap32(uint32_t x) { return __byte_perm(x, 0, 0x0123); }
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define S0(x) (ROR(x, 2) ^ ROR(x, 13) ^ ROR(x, 22))
#define S1(x) (ROR(x, 6) ^ ROR(x, 11) ^ ROR(x, 25))
#define s0(x) (ROR(x, 7) ^ ROR(x, 18) ^ ((x) >> 3))
#define s1(x) (ROR(x, 17) ^ ROR(x, 19) ^ ((x) >> 10))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

__constant__ uint32_t K[64] = { 
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 
};

__device__ void sha256_round(uint32_t* state, uint32_t* w) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
    #pragma unroll
    for (int i = 16; i < 64; i++) w[i] = s1(w[i-2]) + w[i-7] + s0(w[i-15]) + w[i-16];
    #pragma unroll
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + S1(e) + CH(e, f, g) + K[i] + w[i];
        uint32_t t2 = S0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

extern "C" __global__ void sha256_kernel(const uint32_t* header, uint32_t seed, const uint32_t* target, uint32_t* result, uint32_t iters) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    
    // PRNG Xorshift32 for speed on GPU
    uint32_t nonce = seed + tid;
    
    uint32_t pre_w[16];
    #pragma unroll
    for (int j = 0; j < 16; j++) pre_w[j] = bswap32(header[j]);

    for(uint32_t i = 0; i < iters; i++) {
        // Simple nonce change (random sequence)
        nonce ^= nonce << 13;
        nonce ^= nonce >> 17;
        nonce ^= nonce << 5;

        uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        uint32_t w[64];
        #pragma unroll
        for (int j = 0; j < 16; j++) w[j] = pre_w[j];
        sha256_round(state, w);
        
        uint32_t w2[64] = {0};
        w2[0] = bswap32(header[16]); w2[1] = bswap32(header[17]); w2[2] = bswap32(header[18]);
        w2[3] = bswap32(nonce);
        w2[4] = 0x80000000; w2[15] = 640;
        
        uint32_t state2[8];
        #pragma unroll
        for(int j=0; j<8; j++) state2[j] = state[j];
        sha256_round(state2, w2);
        
        uint32_t final_state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        uint32_t w3[64] = {0};
        #pragma unroll
        for(int j=0; j<8; j++) w3[j] = state2[j];
        w3[8] = 0x80000000; w3[15] = 256;
        sha256_round(final_state, w3);
        
        bool matched = true;
        #pragma unroll
        for(int j=7; j>=0; j--) {
            uint32_t val = bswap32(final_state[j]);
            if (val < target[j]) break;
            if (val > target[j]) { matched = false; break; }
        }
        if (matched) atomicExch(result, nonce);
    }
}
)";

struct GpuContext {
    CUcontext ctx; CUmodule mod; CUfunction kernel;
    CUdeviceptr d_h, d_t, d_r;
};
std::map<int, GpuContext> g_gpu_map;

bool init_cuda_runtime() {
    HMODULE hC = LoadLibraryA("nvcuda.dll");
    HMODULE hN = LoadLibraryA("nvrtc64_120_0.dll");
    if (!hC || !hN) return false;

    p_cuInit = (t_cuInit)GetProcAddress(hC, "cuInit");
    p_cuDeviceGet = (t_cuDeviceGet)GetProcAddress(hC, "cuDeviceGet");
    p_cuCtxCreate = (t_cuCtxCreate)GetProcAddress(hC, "cuCtxCreate");
    p_cuCtxSetCurrent = (t_cuCtxSetCurrent)GetProcAddress(hC, "cuCtxSetCurrent");
    p_cuModuleLoadData = (t_cuModuleLoadData)GetProcAddress(hC, "cuModuleLoadData");
    p_cuModuleGetFunction = (t_cuModuleGetFunction)GetProcAddress(hC, "cuModuleGetFunction");
    p_cuMemAlloc = (t_cuMemAlloc)GetProcAddress(hC, "cuMemAlloc");
    p_cuMemcpyHtoD = (t_cuMemcpyHtoD)GetProcAddress(hC, "cuMemcpyHtoD");
    p_cuMemcpyDtoH = (t_cuMemcpyDtoH)GetProcAddress(hC, "cuMemcpyDtoH");
    p_cuLaunchKernel = (t_cuLaunchKernel)GetProcAddress(hC, "cuLaunchKernel");
    p_cuDeviceGetAttribute = (t_cuDeviceGetAttribute)GetProcAddress(hC, "cuDeviceGetAttribute");
    p_cuCtxSynchronize = (t_cuCtxSynchronize)GetProcAddress(hC, "cuCtxSynchronize");
    p_cuMemFree = (t_cuMemFree)GetProcAddress(hC, "cuMemFree");
    p_nvrtcCreateProgram = (t_nvrtcCreateProgram)GetProcAddress(hN, "nvrtcCreateProgram");
    p_nvrtcCompileProgram = (t_nvrtcCompileProgram)GetProcAddress(hN, "nvrtcCompileProgram");
    p_nvrtcGetPTXSize = (t_nvrtcGetPTXSize)GetProcAddress(hN, "nvrtcGetPTXSize");
    p_nvrtcGetPTX = (t_nvrtcGetPTX)GetProcAddress(hN, "nvrtcGetPTX");
    p_nvrtcDestroyProgram = (t_nvrtcDestroyProgram)GetProcAddress(hN, "nvrtcDestroyProgram");

    return (p_cuInit && p_cuInit(0) == 0);
}

extern "C" __declspec(dllexport) uint32_t run_mining_batch_on_device(
    int device_id, uint32_t* header_in, uint32_t start_nonce, 
    uint32_t gpu_bpg, uint32_t gpu_tpb, uint32_t iterations, uint32_t* target) {
    
    if (g_gpu_map.find(device_id) == g_gpu_map.end()) {
        if (!init_cuda_runtime()) return 0;
        
        GpuContext gpu = {0};
        CUdevice dev;
        if (p_cuDeviceGet(&dev, device_id) != 0) return 0;
        if (p_cuCtxCreate(&gpu.ctx, 0, dev) != 0) return 0;

        int major, minor;
        p_cuDeviceGetAttribute(&major, 75, dev); // CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR
        p_cuDeviceGetAttribute(&minor, 76, dev); // CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR

        nvrtcProgram prog;
        p_nvrtcCreateProgram(&prog, cuda_source, "m.cu", 0, NULL, NULL);
        char arch[32]; sprintf(arch, "--gpu-architecture=compute_%d%d", major, minor);
        const char* opts[] = { arch, "--use_fast_math" };
        if (p_nvrtcCompileProgram(prog, 2, opts) != 0) return 0;

        size_t ptx_size;
        p_nvrtcGetPTXSize(prog, &ptx_size);
        std::vector<char> ptx(ptx_size);
        p_nvrtcGetPTX(prog, ptx.data());
        p_nvrtcDestroyProgram(&prog);

        p_cuModuleLoadData(&gpu.mod, ptx.data());
        p_cuModuleGetFunction(&gpu.kernel, gpu.mod, "sha256_kernel");
        p_cuMemAlloc(&gpu.d_h, 80);
        p_cuMemAlloc(&gpu.d_t, 32);
        p_cuMemAlloc(&gpu.d_r, 4);
        
        g_gpu_map[device_id] = gpu;
    }

    GpuContext& g = g_gpu_map[device_id];
    p_cuCtxSetCurrent(g.ctx);

    uint32_t res_val = 0;
    p_cuMemcpyHtoD(g.d_r, &res_val, 4);
    p_cuMemcpyHtoD(g.d_h, header_in, 80);
    p_cuMemcpyHtoD(g.d_t, target, 32);

    void* args[] = { &g.d_h, &start_nonce, &g.d_t, &g.d_r, &iterations };
    if (p_cuLaunchKernel(g.kernel, gpu_bpg, 1, 1, gpu_tpb, 1, 1, 0, NULL, args, NULL) == 0) {
        p_cuCtxSynchronize();
        p_cuMemcpyDtoH(&res_val, g.d_r, 4);
    }

    return res_val;
}
