// ============================================================
// LcRyp Solo Miner - GPU Module Bridge
// https://lcryp.com
// Version 2.2
// ============================================================

#include <windows.h>
#include <stdint.h>
#include <vector>
#include <string.h>
#include <map>

#define TYPE_INTEGRATED 0  // Added for integrated graphics
#define TYPE_NVIDIA     1
#define TYPE_AMD        2

// NVML constants
#define NVML_SUCCESS 0
#define NVML_TEMPERATURE_GPU 0
#define NVML_CLOCK_GRAPHICS 0
#define NVML_CLOCK_MEM 1

// --- ADDED: AMD ADL Structures for real data ---
typedef void* (*ADL_MAIN_MALLOC_CALLBACK)(int);
typedef int (*ADL_MAIN_CONTROL_CREATE)(ADL_MAIN_MALLOC_CALLBACK, int);
typedef int (*ADL_MAIN_CONTROL_DESTROY)();
typedef int (*ADL_OVERDRIVE5_TEMPERATURE_GET)(int, int, int*);
typedef int (*ADL_OVERDRIVE5_FANSPEED_GET)(int, int, void*);
typedef int (*ADL_OVERDRIVE5_PMACTIVITY_GET)(int, void*);

typedef struct { int iSize; int iSpeedType; int iFanSpeed; int iFlags; } ADLFanSpeedValue;
typedef struct { int iSize; int iEngineClock; int iMemoryClock; int iVddc; int iActivityPercent; int iCurrentPerformanceLevel; int iCurrentBusSpeed; int iCurrentBusLanes; int iMaximumBusLanes; int iReserved; } ADLPMActivity;

// NVML structures
typedef struct nvmlDevice_st* nvmlDevice_t;
typedef struct { unsigned long long total; unsigned long long free; unsigned long long used; } nvmlMemory_t;
typedef struct { unsigned int gpu; unsigned int memory; } nvmlUtilization_t;

// NVML functions
typedef int (*nvmlInit_t)();
typedef int (*nvmlShutdown_t)();
typedef int (*nvmlDeviceGetHandleByIndex_t)(int, nvmlDevice_t*);
typedef int (*nvmlDeviceGetTemperature_t)(nvmlDevice_t, int, unsigned int*);
typedef int (*nvmlDeviceGetFanSpeed_t)(nvmlDevice_t, unsigned int*);
typedef int (*nvmlDeviceGetPowerUsage_t)(nvmlDevice_t, unsigned int*);
typedef int (*nvmlDeviceGetMemoryInfo_t)(nvmlDevice_t, nvmlMemory_t*);
typedef int (*nvmlDeviceGetUtilizationRates_t)(nvmlDevice_t, nvmlUtilization_t*);
typedef int (*nvmlDeviceGetClockInfo_t)(nvmlDevice_t, int, unsigned int*);

// ADDED: For integrated graphics (Intel)
typedef void* (*INTEL_GET_METRICS)(int);

// Global pointers
static nvmlInit_t p_nvmlInit = NULL;
static nvmlShutdown_t p_nvmlShutdown = NULL;
static nvmlDeviceGetHandleByIndex_t p_nvmlDeviceGetHandleByIndex = NULL;
static nvmlDeviceGetTemperature_t p_nvmlDeviceGetTemperature = NULL;
static nvmlDeviceGetFanSpeed_t p_nvmlDeviceGetFanSpeed = NULL;
static nvmlDeviceGetPowerUsage_t p_nvmlDeviceGetPowerUsage = NULL;
static nvmlDeviceGetMemoryInfo_t p_nvmlDeviceGetMemoryInfo = NULL;
static nvmlDeviceGetUtilizationRates_t p_nvmlDeviceGetUtilizationRates = NULL;
static nvmlDeviceGetClockInfo_t p_nvmlDeviceGetClockInfo = NULL;
static HMODULE nvml_lib = NULL;
static bool nvml_loaded = false;

// AMD pointers
static HMODULE adl_lib = NULL;
static bool adl_loaded = false;
static ADL_OVERDRIVE5_TEMPERATURE_GET p_ADL_Temp = NULL;
static ADL_OVERDRIVE5_FANSPEED_GET p_ADL_Fan = NULL;
static ADL_OVERDRIVE5_PMACTIVITY_GET p_ADL_Act = NULL;
static ADL_MAIN_CONTROL_DESTROY p_ADL_Destroy = NULL;

// ADDED: For integrated graphics
static HMODULE intel_lib = NULL;
static bool intel_loaded = false;

void* __stdcall ADL_Alloc(int iSize) { return malloc(iSize); }

struct GpuDevice { 
    int type;           // TYPE_INTEGRATED, TYPE_NVIDIA, TYPE_AMD
    int vendor_index;   // Local index inside vendor (0, 1, 2...)
    char name[256]; 
    int global_id;      // Unique identifier for API (type + index combination)
};

struct GpuMetrics { 
    int temperature; 
    int fan_speed; 
    float power_usage; 
    int memory_used; 
    int utilization; 
    int clock_core; 
    int clock_memory; 
    int vendor_specific; 
};

// ADDED: Separate vectors for each GPU type
std::vector<GpuDevice> integrated_devices;  // TYPE_INTEGRATED
std::vector<GpuDevice> nvidia_devices;      // TYPE_NVIDIA
std::vector<GpuDevice> amd_devices;         // TYPE_AMD

bool is_initialized = false;
typedef uint32_t (*MiningFunction)(int, uint32_t*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t*);
std::map<int, HMODULE> mining_drivers;
std::map<int, MiningFunction> mining_functions;

// ==============================================
// HELPER FUNCTIONS FOR INDEXING
// ==============================================

// Generate global_id: (type << 16) | local_index
inline int make_global_id(int type, int vendor_index) {
    return (type << 16) | (vendor_index & 0xFFFF);
}

// Get type from global_id
inline int get_type_from_id(int global_id) {
    return (global_id >> 16) & 0xFF;
}

// Get local vendor index from global_id
inline int get_vendor_index_from_id(int global_id) {
    return global_id & 0xFFFF;
}

// Find device by global_id
const GpuDevice* find_device_by_id(int global_id) {
    int type = get_type_from_id(global_id);
    int vendor_index = get_vendor_index_from_id(global_id);
    
    switch (type) {
        case TYPE_INTEGRATED:
            if (vendor_index >= 0 && vendor_index < (int)integrated_devices.size()) {
                return &integrated_devices[vendor_index];
            }
            break;
        case TYPE_NVIDIA:
            if (vendor_index >= 0 && vendor_index < (int)nvidia_devices.size()) {
                return &nvidia_devices[vendor_index];
            }
            break;
        case TYPE_AMD:
            if (vendor_index >= 0 && vendor_index < (int)amd_devices.size()) {
                return &amd_devices[vendor_index];
            }
            break;
    }
    return nullptr;
}

// ==============================================
// INITIALIZATION WITH 3 GPU TYPES DETECTION
// ==============================================

bool load_nvml_functions() {
    if (nvml_loaded) return true;
    nvml_lib = LoadLibraryA("nvml.dll");
    if (!nvml_lib) nvml_lib = LoadLibraryA("nvidia-ml.dll");
    if (!nvml_lib) return false;
    
    p_nvmlInit = (nvmlInit_t)GetProcAddress(nvml_lib, "nvmlInit");
    p_nvmlShutdown = (nvmlShutdown_t)GetProcAddress(nvml_lib, "nvmlShutdown");
    p_nvmlDeviceGetHandleByIndex = (nvmlDeviceGetHandleByIndex_t)GetProcAddress(nvml_lib, "nvmlDeviceGetHandleByIndex");
    p_nvmlDeviceGetTemperature = (nvmlDeviceGetTemperature_t)GetProcAddress(nvml_lib, "nvmlDeviceGetTemperature");
    p_nvmlDeviceGetFanSpeed = (nvmlDeviceGetFanSpeed_t)GetProcAddress(nvml_lib, "nvmlDeviceGetFanSpeed");
    p_nvmlDeviceGetPowerUsage = (nvmlDeviceGetPowerUsage_t)GetProcAddress(nvml_lib, "nvmlDeviceGetPowerUsage");
    p_nvmlDeviceGetMemoryInfo = (nvmlDeviceGetMemoryInfo_t)GetProcAddress(nvml_lib, "nvmlDeviceGetMemoryInfo");
    p_nvmlDeviceGetUtilizationRates = (nvmlDeviceGetUtilizationRates_t)GetProcAddress(nvml_lib, "nvmlDeviceGetUtilizationRates");
    p_nvmlDeviceGetClockInfo = (nvmlDeviceGetClockInfo_t)GetProcAddress(nvml_lib, "nvmlDeviceGetClockInfo");
    
    if (!p_nvmlInit || !p_nvmlDeviceGetHandleByIndex || p_nvmlInit() != 0) return false;
    nvml_loaded = true; 
    return true;
}

bool load_adl_functions() {
    if (adl_loaded) return true;
    adl_lib = LoadLibraryA("atiadlxx.dll");
    if (!adl_lib) adl_lib = LoadLibraryA("atiadlxy.dll");
    if (!adl_lib) return false;
    
    auto create = (ADL_MAIN_CONTROL_CREATE)GetProcAddress(adl_lib, "ADL_Main_Control_Create");
    p_ADL_Destroy = (ADL_MAIN_CONTROL_DESTROY)GetProcAddress(adl_lib, "ADL_Main_Control_Destroy");
    p_ADL_Temp = (ADL_OVERDRIVE5_TEMPERATURE_GET)GetProcAddress(adl_lib, "ADL_Overdrive5_Temperature_Get");
    p_ADL_Fan = (ADL_OVERDRIVE5_FANSPEED_GET)GetProcAddress(adl_lib, "ADL_Overdrive5_FanSpeed_Get");
    p_ADL_Act = (ADL_OVERDRIVE5_PMACTIVITY_GET)GetProcAddress(adl_lib, "ADL_Overdrive5_PMActivity_Get");
    
    if (create && create(ADL_Alloc, 1) == 0) { 
        adl_loaded = true; 
        return true; 
    }
    return false;
}

// ADDED: Functions for integrated graphics
bool load_intel_functions() {
    if (intel_loaded) return true;
    // Intel SDK loading can be added here if needed
    intel_loaded = true; // Mark as loaded even without library
    return true;
}

void unload_drivers() {
    if (nvml_loaded) { 
        if (p_nvmlShutdown) p_nvmlShutdown(); 
        FreeLibrary(nvml_lib); 
        nvml_loaded = false; 
    }
    if (adl_loaded) { 
        if (p_ADL_Destroy) p_ADL_Destroy(); 
        FreeLibrary(adl_lib); 
        adl_loaded = false; 
    }
    if (intel_lib) {
        FreeLibrary(intel_lib);
        intel_lib = NULL;
        intel_loaded = false;
    }
}

// ==============================================
// METRICS FUNCTIONS FOR EACH TYPE
// ==============================================

GpuMetrics get_integrated_metrics(int device_index) {
    GpuMetrics metrics = {0}; 
    metrics.vendor_specific = TYPE_INTEGRATED;
    
    // Basic implementation for integrated graphics
    // Intel API calls can be added here if available
    metrics.temperature = 40; // Example value
    metrics.utilization = 15; // Example value
    
    return metrics;
}

GpuMetrics get_nvidia_metrics(int device_index) {
    GpuMetrics metrics = {0}; 
    metrics.vendor_specific = TYPE_NVIDIA;
    
    if (!load_nvml_functions()) return metrics;
    nvmlDevice_t device;
    if (p_nvmlDeviceGetHandleByIndex(device_index, &device) != 0) return metrics;
    
    unsigned int val;
    if (p_nvmlDeviceGetTemperature && p_nvmlDeviceGetTemperature(device, 0, &val) == 0) 
        metrics.temperature = (int)val;
    if (p_nvmlDeviceGetFanSpeed && p_nvmlDeviceGetFanSpeed(device, &val) == 0) 
        metrics.fan_speed = (int)val;
    if (p_nvmlDeviceGetPowerUsage && p_nvmlDeviceGetPowerUsage(device, &val) == 0) 
        metrics.power_usage = val / 1000.0f;
    
    nvmlMemory_t memory; 
    if (p_nvmlDeviceGetMemoryInfo && p_nvmlDeviceGetMemoryInfo(device, &memory) == 0) 
        metrics.memory_used = (int)(memory.used / (1024 * 1024));
    
    nvmlUtilization_t utilization; 
    if (p_nvmlDeviceGetUtilizationRates && p_nvmlDeviceGetUtilizationRates(device, &utilization) == 0) 
        metrics.utilization = (int)utilization.gpu;
    
    if (p_nvmlDeviceGetClockInfo && p_nvmlDeviceGetClockInfo(device, 0, &val) == 0) 
        metrics.clock_core = (int)val;
    if (p_nvmlDeviceGetClockInfo && p_nvmlDeviceGetClockInfo(device, 1, &val) == 0) 
        metrics.clock_memory = (int)val;
    
    return metrics;
}

GpuMetrics get_amd_metrics(int device_index) {
    GpuMetrics metrics = {0}; 
    metrics.vendor_specific = TYPE_AMD;
    
    if (!load_adl_functions()) return metrics;
    
    int temp = 0; 
    if (p_ADL_Temp && p_ADL_Temp(device_index, 0, &temp) == 0) 
        metrics.temperature = temp / 1000;
    
    ADLFanSpeedValue fan = {sizeof(fan), 1}; 
    if (p_ADL_Fan && p_ADL_Fan(device_index, 0, &fan) == 0) 
        metrics.fan_speed = fan.iFanSpeed;
    
    ADLPMActivity act = {sizeof(act)}; 
    if (p_ADL_Act && p_ADL_Act(device_index, &act) == 0) {
        metrics.utilization = act.iActivityPercent;
        metrics.clock_core = act.iEngineClock / 100;
        metrics.clock_memory = act.iMemoryClock / 100;
        metrics.power_usage = (float)act.iVddc / 10.0f;
    }
    
    return metrics;
}

// ==============================================
// HARDWARE DETECTION WITH 3 GPU TYPES
// ==============================================

bool load_mining_driver(int type) {
    if (mining_drivers.find(type) != mining_drivers.end()) return true;
    
    const char* dll_name = NULL;
    switch (type) {
        case TYPE_INTEGRATED: dll_name = "GpuModuleIntegrated.dll"; break;
        case TYPE_NVIDIA: dll_name = "GpuModuleGreen.dll"; break;
        case TYPE_AMD: dll_name = "GpuModuleRed.dll"; break;
        default: return false;
    }
    
    HMODULE dll = LoadLibraryA(dll_name);
    if (!dll) return false;
    
    MiningFunction func = (MiningFunction)GetProcAddress(dll, "run_mining_batch_on_device");
    if (!func) { 
        FreeLibrary(dll); 
        return false; 
    }
    
    mining_drivers[type] = dll;
    mining_functions[type] = func;
    return true;
}

void discover_hardware() {
    if (is_initialized) return;
    
    // Clear all vectors
    integrated_devices.clear();
    nvidia_devices.clear();
    amd_devices.clear();
    
    // 1. DETECT INTEGRATED GRAPHICS (Intel)
    // Try to find Intel graphics via DXGI or OpenCL
    HMODULE dxgi_lib = LoadLibraryA("dxgi.dll");
    if (dxgi_lib) {
        // DXGI calls for Intel GPU can be added here
        FreeLibrary(dxgi_lib);
    }
    
    // Simple way - search for Intel via OpenCL
    HMODULE ocl_lib = LoadLibraryA("OpenCL.dll");
    if (ocl_lib) {
        typedef int (*OCL_PLATFORMS)(uint32_t, void**, uint32_t*); 
        typedef int (*OCL_DEVICES)(void*, uint64_t, uint32_t, void**, uint32_t*); 
        typedef int (*OCL_INFO)(void*, uint32_t, size_t, void*, size_t*);
        
        auto getPlatforms = (OCL_PLATFORMS)GetProcAddress(ocl_lib, "clGetPlatformIDs");
        auto getDevices = (OCL_DEVICES)GetProcAddress(ocl_lib, "clGetDeviceIDs");
        auto getPlatformInfo = (OCL_INFO)GetProcAddress(ocl_lib, "clGetPlatformInfo");
        auto getDeviceInfo = (OCL_INFO)GetProcAddress(ocl_lib, "clGetDeviceInfo");
        
        uint32_t num_platforms = 0; 
        void* platforms[10];
        
        if (getPlatforms && getPlatforms(10, platforms, &num_platforms) == 0) {
            for (uint32_t i = 0; i < num_platforms; i++) {
                char vendor[128] = {0}; 
                if (getPlatformInfo) 
                    getPlatformInfo(platforms[i], 0x0903, sizeof(vendor), vendor, NULL);
                
                // Search for Intel platforms
                if (strstr(vendor, "Intel") != NULL) {
                    uint32_t num_devices = 0; 
                    void* device_ids[10];
                    
                    if (getDevices && getDevices(platforms[i], 0x4, 10, device_ids, &num_devices) == 0) {
                        for (uint32_t j = 0; j < num_devices; j++) {
                            GpuDevice dev;
                            dev.type = TYPE_INTEGRATED;
                            dev.vendor_index = (int)integrated_devices.size(); // Local index from 0
                            strcpy_s(dev.name, "Intel Integrated Graphics");
                            
                            // Try to get real name
                            if (getDeviceInfo) 
                                getDeviceInfo(device_ids[j], 0x102B, 256, dev.name, NULL);
                            
                            dev.global_id = make_global_id(TYPE_INTEGRATED, dev.vendor_index);
                            integrated_devices.push_back(dev);
                        }
                    }
                }
            }
        }
        FreeLibrary(ocl_lib);
    }
    
    // 2. DETECT NVIDIA
    HMODULE nv_lib = LoadLibraryA("nvcuda.dll");
    if (nv_lib) {
        typedef int (*CU_INIT)(unsigned int); 
        typedef int (*CU_COUNT)(int*); 
        typedef int (*CU_DEVICE)(int*, int); 
        typedef int (*CU_NAME)(char*, int, int);
        
        auto cuInit = (CU_INIT)GetProcAddress(nv_lib, "cuInit");
        auto cuCount = (CU_COUNT)GetProcAddress(nv_lib, "cuDeviceGetCount");
        auto cuDevice = (CU_DEVICE)GetProcAddress(nv_lib, "cuDeviceGet");
        auto cuName = (CU_NAME)GetProcAddress(nv_lib, "cuDeviceGetName");
        
        int count = 0;
        if (cuInit && cuInit(0) == 0 && cuCount && cuCount(&count) == 0) {
            for (int i = 0; i < count; i++) {
                GpuDevice dev;
                dev.type = TYPE_NVIDIA;
                dev.vendor_index = i; // Local NVIDIA index from 0
                strcpy_s(dev.name, "Unknown NVIDIA GPU");
                
                int handle; 
                if (cuDevice && cuDevice(&handle, i) == 0 && cuName) 
                    cuName(dev.name, 256, handle);
                
                dev.global_id = make_global_id(TYPE_NVIDIA, dev.vendor_index);
                nvidia_devices.push_back(dev);
            }
        }
        FreeLibrary(nv_lib);
    }
    
    // 3. DETECT AMD (OpenCL)
    ocl_lib = LoadLibraryA("OpenCL.dll");
    if (ocl_lib) {
        typedef int (*OCL_PLATFORMS)(uint32_t, void**, uint32_t*); 
        typedef int (*OCL_DEVICES)(void*, uint64_t, uint32_t, void**, uint32_t*); 
        typedef int (*OCL_INFO)(void*, uint32_t, size_t, void*, size_t*);
        
        auto getPlatforms = (OCL_PLATFORMS)GetProcAddress(ocl_lib, "clGetPlatformIDs");
        auto getDevices = (OCL_DEVICES)GetProcAddress(ocl_lib, "clGetDeviceIDs");
        auto getPlatformInfo = (OCL_INFO)GetProcAddress(ocl_lib, "clGetPlatformInfo");
        auto getDeviceInfo = (OCL_INFO)GetProcAddress(ocl_lib, "clGetDeviceInfo");
        
        uint32_t num_platforms = 0; 
        void* platforms[10];
        
        if (getPlatforms && getPlatforms(10, platforms, &num_platforms) == 0) {
            for (uint32_t i = 0; i < num_platforms; i++) {
                char vendor[128] = {0}; 
                if (getPlatformInfo) 
                    getPlatformInfo(platforms[i], 0x0903, sizeof(vendor), vendor, NULL);
                
                // Search for AMD platforms (not NVIDIA and not Intel)
                if (strstr(vendor, "AMD") != NULL || strstr(vendor, "Advanced Micro Devices") != NULL) {
                    uint32_t num_devices = 0; 
                    void* device_ids[10];
                    
                    if (getDevices && getDevices(platforms[i], 0x4, 10, device_ids, &num_devices) == 0) {
                        for (uint32_t j = 0; j < num_devices; j++) {
                            GpuDevice dev;
                            dev.type = TYPE_AMD;
                            dev.vendor_index = (int)amd_devices.size(); // Local AMD index from 0
                            strcpy_s(dev.name, "Unknown AMD GPU");
                            
                            if (getDeviceInfo) 
                                getDeviceInfo(device_ids[j], 0x102B, 256, dev.name, NULL);
                            
                            dev.global_id = make_global_id(TYPE_AMD, dev.vendor_index);
                            amd_devices.push_back(dev);
                        }
                    }
                }
            }
        }
        FreeLibrary(ocl_lib);
    }
    
    is_initialized = true;
}

// ==============================================
// EXPORTED DLL FUNCTIONS
// ==============================================

extern "C" {
    // Get total number of GPUs of all types
    __declspec(dllexport) int count_all_gpus() { 
        discover_hardware(); 
        return (int)(integrated_devices.size() + nvidia_devices.size() + amd_devices.size()); 
    }
    
    // Get number of GPUs by type
    __declspec(dllexport) int count_gpus_by_type(int type) {
        discover_hardware();
        switch (type) {
            case TYPE_INTEGRATED: return (int)integrated_devices.size();
            case TYPE_NVIDIA: return (int)nvidia_devices.size();
            case TYPE_AMD: return (int)amd_devices.size();
            default: return 0;
        }
    }
    
    // Get global_id for specific type and index
    __declspec(dllexport) int get_gpu_id(int type, int vendor_index) {
        discover_hardware();
        switch (type) {
            case TYPE_INTEGRATED: 
                if (vendor_index >= 0 && vendor_index < (int)integrated_devices.size())
                    return integrated_devices[vendor_index].global_id;
                break;
            case TYPE_NVIDIA: 
                if (vendor_index >= 0 && vendor_index < (int)nvidia_devices.size())
                    return nvidia_devices[vendor_index].global_id;
                break;
            case TYPE_AMD: 
                if (vendor_index >= 0 && vendor_index < (int)amd_devices.size())
                    return amd_devices[vendor_index].global_id;
                break;
        }
        return -1;
    }
    
    // Get GPU name by global_id
    __declspec(dllexport) const char* get_gpu_name(int global_id) { 
        if (!is_initialized) discover_hardware();
        const GpuDevice* dev = find_device_by_id(global_id);
        return dev ? dev->name : "Invalid ID";
    }
    
    // Get GPU type by global_id
    __declspec(dllexport) int get_gpu_type(int global_id) { 
        if (!is_initialized) discover_hardware();
        const GpuDevice* dev = find_device_by_id(global_id);
        return dev ? dev->type : -1;
    }
    
    // Get local vendor index by global_id
    __declspec(dllexport) int get_gpu_vendor_index(int global_id) { 
        if (!is_initialized) discover_hardware();
        const GpuDevice* dev = find_device_by_id(global_id);
        return dev ? dev->vendor_index : -1;
    }
    
    // Get full GPU metrics
    __declspec(dllexport) GpuMetrics get_gpu_full_stats(int global_id) {
        GpuMetrics m = {0}; 
        if (!is_initialized) discover_hardware();
        
        const GpuDevice* dev = find_device_by_id(global_id);
        if (!dev) return m;
        
        switch (dev->type) {
            case TYPE_INTEGRATED: 
                return get_integrated_metrics(dev->vendor_index);
            case TYPE_NVIDIA: 
                return get_nvidia_metrics(dev->vendor_index);
            case TYPE_AMD: 
                return get_amd_metrics(dev->vendor_index);
            default: 
                return m;
        }
    }
    
    // Separate functions for metrics (for backward compatibility)
    __declspec(dllexport) int get_gpu_temperature(int global_id) { 
        return get_gpu_full_stats(global_id).temperature; 
    }
    __declspec(dllexport) int get_gpu_fan_speed(int global_id) { 
        return get_gpu_full_stats(global_id).fan_speed; 
    }
    __declspec(dllexport) float get_gpu_power_usage(int global_id) { 
        return get_gpu_full_stats(global_id).power_usage; 
    }
    __declspec(dllexport) int get_gpu_utilization(int global_id) { 
        return get_gpu_full_stats(global_id).utilization; 
    }
    
    // Mining function
    __declspec(dllexport) uint32_t run_mining_batch_on_device(int global_id, uint32_t* h, uint32_t s, 
                                                              uint32_t b, uint32_t t, uint32_t r, uint32_t* tg) {
        if (!is_initialized) discover_hardware();
        
        const GpuDevice* dev = find_device_by_id(global_id);
        if (!dev) return 0;
        
        if (!load_mining_driver(dev->type)) return 0;
        
        // Pass local vendor index to mining function
        return mining_functions[dev->type](dev->vendor_index, h, s, b, t, r, tg);
    }
    
    // Refresh statistics
    __declspec(dllexport) void refresh_gpu_stats() {
        // Forced data reload can be added here
    }
    
    // Cleanup resources
    __declspec(dllexport) void cleanup_mining_drivers() {
        for (auto& pair : mining_drivers) 
            if (pair.second) 
                FreeLibrary(pair.second);
        
        mining_drivers.clear(); 
        mining_functions.clear(); 
        unload_drivers();
    }
}

// ==============================================
// DLL ENTRY POINT
// ==============================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        cleanup_mining_drivers();
    }
    return TRUE;
}
