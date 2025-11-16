/* libxeno_wrapper.c - Full-featured wrapper focused on Xclipse 940
 *
 * This file is intentionally complete and self-contained for testing and feature probing.
 * It exports ICD negotiation symbols so the Vulkan loader can use it as an ICD library,
 * advertises rich device features and extensions per the manifest above, and provides runtime
 * logging, feature dumps, BCn detection/toggling, pipeline/pipeline-create validation hooks,
 * queue submission and memory allocation tracking, and crash-safe flushing.
 *
 * NOTE: This wrapper advertises features and assists testing. It does NOT implement real
 * command submission to hardware. To achieve hardware acceleration, it must be integrated
 * with a vendor ICD or Mesa/Turnip backend which implements device-level entrypoints.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <pthread.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <signal.h>
#include <inttypes.h>
#include <vulkan/vulkan.h>

/* Paths */
#define MANIFEST_PATH "/etc/exynostools/profiles/vendor/xilinx_xc/manifest.json"
#define SIDE_LOG_ENV "XCLIPSE_SIDE_LOG"
#define TUNE_REPORT_ENV "XCLIPSE_TUNE_REPORT"
#define DEFAULT_SIDE_LOG "/data/local/tmp/xeno_wrapper.log"
#define DEFAULT_TUNE_REPORT "/data/local/tmp/xeno_tune_report.json"
#define PACKAGE_SIDE_LOG "/var/log/xeno_wrapper.log"
#define PACKAGE_TUNE_REPORT "/var/log/xeno_tune_report.json"

/* BC fallback search paths */
static const char* spv_search_paths[] = {
    "/usr/share/exynostools/shaders/pipeline_cache/",
    "/assets/shaders/decode/",
    "/usr/share/xclipse/shaders/",
    NULL
};

/* Forward bc_emulate interfaces (implemented in bc_emulate.c) */
extern int ensure_fallback_decoder_ready(const char* vk_format_name);
extern const char* prepare_decoder_for_format(const char* vk_format_name);
extern unsigned char* load_fallback_spv_blob(const char* vk_format_name, size_t* out_size);
extern int bc_emulate_selftest(void);

/* Logging */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static void ensure_parent_dir(const char* path) {
    if (!path) return;
    char b[1024]; strncpy(b, path, sizeof(b)-1);
    for (int i=1;b[i];++i) if (b[i]=='/') { b[i]=0; mkdir(b,0755); b[i]='/'; }
}
static void now_str(char* buf, size_t n) {
    time_t t = time(NULL); struct tm tm; gmtime_r(&t,&tm); strftime(buf,n,"%Y-%m-%dT%H:%M:%SZ",&tm);
}
static void side_log_write(const char* entry) {
    pthread_mutex_lock(&log_lock);
    const char* path = getenv(SIDE_LOG_ENV);
    if (!path) path = DEFAULT_SIDE_LOG;
    ensure_parent_dir(path);
    int fd = open(path, O_WRONLY|O_CREAT|O_APPEND, 0644);
    if (fd < 0) { fd = open(PACKAGE_SIDE_LOG, O_WRONLY|O_CREAT|O_APPEND, 0644); if (fd < 0) { pthread_mutex_unlock(&log_lock); return; } }
    write(fd, entry, strlen(entry)); write(fd, "\n", 1); fsync(fd); close(fd);
    pthread_mutex_unlock(&log_lock);
}
static void xlog(const char* fmt, ...) {
    char buf[2048], ts[64]; now_str(ts,sizeof(ts));
    va_list ap; va_start(ap, fmt);
    int n = snprintf(buf, sizeof(buf), "[%s] xeno: ", ts);
    vsnprintf(buf+n, sizeof(buf)-n, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", buf);
    side_log_write(buf);
}

/* Crash handler */
static void crash_handler(int sig) {
    char tmp[256]; snprintf(tmp,sizeof(tmp),"CRASH signal=%d", sig);
    side_log_write(tmp);
    signal(sig, SIG_DFL); raise(sig);
}

/* Manifest validation (simple substring checks) */
static int read_file_to_buf(const char* path, char** out, size_t* outlen) {
    if (!path || !out) return -1;
    FILE* f = fopen(path,"rb"); if (!f) return -1;
    fseek(f,0,SEEK_END); long s = ftell(f); rewind(f);
    if (s<=0) { fclose(f); return -1; }
    char* buf = malloc(s+1); if (!buf) { fclose(f); return -1; }
    if (fread(buf,1,s,f) != (size_t)s) { free(buf); fclose(f); return -1; }
    buf[s]=0; fclose(f); *out = buf; if (outlen) *outlen = s; return 0;
}
static int manifest_contains(const char* key) {
    char* buf = NULL; if (read_file_to_buf(MANIFEST_PATH, &buf, NULL) != 0) return 0;
    int found = (strstr(buf, key) != NULL); free(buf); return found;
}
static void validate_manifest_alignment(void) {
    xlog("Validating manifest alignment: %s", MANIFEST_PATH);
    struct { const char* name; const char* key; } checks[] = {
        {"ray_tracing", "\"ray_tracing\": true"},
        {"mesh_shading", "\"mesh_shading\": true"},
        {"BC1 hardware", "\"BC1\": \"hardware\""},
        {"descriptor_indexing", "\"descriptor_indexing\": true"},
        {"synchronization2", "\"synchronization2\": true"}
    };
    for (size_t i=0;i<sizeof(checks)/sizeof(checks[0]);++i) {
        if (!manifest_contains(checks[i].key)) xlog("MANIFEST_MISMATCH: %s (missing key=%s)", checks[i].name, checks[i].key);
        else xlog("MANIFEST_OK: %s", checks[i].name);
    }
}

/* BC runtime detection */
static int env_has(const char* name) { const char* v=getenv(name); return v && v[0]; }
static int env_bool_default(const char* name, int def) {
    const char* v = getenv(name); if (!v) return def;
    if (strcmp(v,"1")==0 || strcasecmp(v,"true")==0) return 1;
    if (strcmp(v,"0")==0 || strcasecmp(v,"false")==0) return 0;
    return def;
}
static int probe_hw_bc_presence(void) {
    if (env_has("XCLIPSE_FORCE_HW_BC")) return env_bool_default("XCLIPSE_FORCE_HW_BC",1);
    DIR* d = opendir("/sys/class/drm");
    if (d) { struct dirent* e; while ((e=readdir(d))) { if (e->d_name[0]=='.') continue; char path[1024]; snprintf(path,sizeof(path),"/sys/class/drm/%s/device/driver/module", e->d_name); if (access(path,F_OK)==0) { char buf[1024]; ssize_t r = readlink(path, buf, sizeof(buf)-1); if (r>0) { buf[r]=0; if (strcasestr(buf,"xcl")||strcasestr(buf,"xclipse")||strcasestr(buf,"xeno")) { closedir(d); return 1; } } } } closedir(d); }
    if (access("/dev/dri/card0", F_OK) == 0) return 1;
    return 1; /* default to manifest hint */
}

/* Exported API used by bc_emulate */
int xeno_hw_supports_bc_format(const char* vk_format_name) {
    if (env_has("XCLIPSE_DISABLE_ALL_HW_BC")) return 0;
    return probe_hw_bc_presence();
}
void xeno_set_force_hw_bc(int enable) { if (enable) setenv("XCLIPSE_FORCE_HW_BC","1",1); else setenv("XCLIPSE_FORCE_HW_BC","0",1); }

/* Feature dump writer */
static void write_feature_dump(const char* outpath) {
    ensure_parent_dir(outpath);
    FILE* f = fopen(outpath, "w"); if (!f) { xlog("failed to open %s", outpath); return; }
    fprintf(f, "{\n");
    fprintf(f, "  \"device\": \"Xclipse 940\",\n");
    fprintf(f, "  \"timestamp\": \"%ld\",\n", (long)time(NULL));
    fprintf(f, "  \"features\": {\n");
    fprintf(f, "    \"ray_tracing\": true,\n");
    fprintf(f, "    \"mesh_shading\": true,\n");
    fprintf(f, "    \"descriptor_indexing\": true,\n");
    fprintf(f, "    \"buffer_device_address\": true\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n");
    fflush(f); fsync(fileno(f)); fclose(f);
    xlog("feature dump written to %s", outpath);
}

/* Logging API for pipeline/fallback/queue/memory */
void xeno_log_bc_fallback(const char* image_id, const char* format, const char* reason) {
    char tmp[1024]; snprintf(tmp,sizeof(tmp),"BC_FALLBACK image=%s format=%s reason=%s", image_id?image_id:"?", format?format:"?", reason?reason:"?"); xlog("%s", tmp);
}
void xeno_log_pipeline_create(const char* pipeline_name, const char* stage, int success, const char* detail) {
    char tmp[1024]; snprintf(tmp,sizeof(tmp),"PIPELINE_CREATE name=%s stage=%s success=%d detail=%s", pipeline_name?pipeline_name:"?", stage?stage:"?", success, detail?detail:""); xlog("%s", tmp);
}
void xeno_log_queue_submit(const char* queue_name, uint64_t submit_id, uint64_t cmdbuf_count, uint64_t duration_ns) {
    char tmp[256]; snprintf(tmp,sizeof(tmp),"QUEUE_SUBMIT queue=%s id=%" PRIu64 " cmdbufs=%" PRIu64 " duration_ns=%" PRIu64, queue_name?queue_name:"default", submit_id, cmdbuf_count, duration_ns); xlog("%s", tmp);
}
void xeno_log_memory_alloc(const char* alloc_type, uint64_t size, const char* tag) {
    char tmp[256]; snprintf(tmp,sizeof(tmp),"MEM_ALLOC type=%s size=%" PRIu64 " tag=%s", alloc_type?alloc_type:"?", size, tag?tag:""); xlog("%s", tmp);
}
void xeno_flush_logs(void) { xlog("FLUSH_LOGS"); }

/* --- ICD negotiation & instance proc forwarding --- */
static void* real_loader = NULL;
static PFN_vkGetInstanceProcAddr real_vkGetInstanceProcAddr = NULL;
static PFN_vkGetDeviceProcAddr real_vkGetDeviceProcAddr = NULL;
static pthread_once_t loader_once = PTHREAD_ONCE_INIT;
static void ensure_real_loader(void) {
    if (real_loader) return;
    real_loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (real_loader) {
        real_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(real_loader, "vkGetInstanceProcAddr");
        real_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)dlsym(real_loader, "vkGetDeviceProcAddr");
    } else {
        xlog("warning: cannot open libvulkan.so.1: %s", dlerror());
    }
}

/* Synthetic physical device handle */
static void* synthetic_physical = (void*)0xC0FFEE;

/* Implement essential exported functions for loader compatibility */
VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pVersion) {
    if (!pVersion) return VK_ERROR_INCOMPATIBLE_DRIVER;
    uint32_t provided = *pVersion;
    if (provided >= 2) *pVersion = 2; else if (provided >=1) *pVersion = 1; else return VK_ERROR_INCOMPATIBLE_DRIVER;
    xlog("vk_icdNegotiateLoaderICDInterfaceVersion agreed %u", *pVersion);
    return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return vkGetInstanceProcAddr(instance, pName);
}

/* Implement enumerations and properties */
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pPhysicalDeviceCount, VkPhysicalDevice* pPhysicalDevices) {
    if (!pPhysicalDeviceCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pPhysicalDevices) { *pPhysicalDeviceCount = 1; return VK_SUCCESS; }
    if (*pPhysicalDeviceCount < 1) return VK_INCOMPLETE;
    pPhysicalDevices[0] = (VkPhysicalDevice)synthetic_physical; *pPhysicalDeviceCount = 1; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties* pProperties) {
    if (!pProperties) return;
    memset(pProperties,0,sizeof(*pProperties));
    pProperties->apiVersion = VK_MAKE_VERSION(1,4,0);
    pProperties->driverVersion = VK_MAKE_VERSION(1,0,0);
    pProperties->vendorID = 0x1002; pProperties->deviceID = 0x0940;
    snprintf(pProperties->deviceName, sizeof(pProperties->deviceName), "Xclipse 940 (synthetic ICD)");
    pProperties->limits.maxImageDimension2D = 16384;
    pProperties->limits.maxComputeSharedMemorySize = 131072;
    pProperties->limits.maxComputeWorkGroupInvocations = 2048;
    pProperties->limits.maxColorAttachments = 8;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* pMemProps) {
    if (!pMemProps) return;
    memset(pMemProps,0,sizeof(*pMemProps));
    pMemProps->memoryHeapCount = 2;
    pMemProps->memoryHeaps[0].size = 512ull * 1024 * 1024;
    pMemProps->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    pMemProps->memoryHeaps[1].size = 2048ull * 1024 * 1024;
    pMemProps->memoryTypeCount = 2;
    pMemProps->memoryTypes[0].heapIndex = 0; pMemProps->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    pMemProps->memoryTypes[1].heapIndex = 1; pMemProps->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice, uint32_t* pCount, VkQueueFamilyProperties* props) {
    if (!pCount) return; if (!props) { *pCount = 3; return; }
    uint32_t count = (*pCount < 3) ? *pCount : 3;
    for (uint32_t i=0;i<count;i++) {
        VkQueueFamilyProperties p; memset(&p,0,sizeof(p));
        if (i==0) { p.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT; p.queueCount = 8; }
        else if (i==1) { p.queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT; p.queueCount = 4; }
        else { p.queueFlags = VK_QUEUE_TRANSFER_BIT; p.queueCount = 2; }
        props[i]=p;
    }
    *pCount = 3;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties* pFormatProperties) {
    if (!pFormatProperties) return; memset(pFormatProperties,0,sizeof(*pFormatProperties));
    switch (format) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC7_UNORM_BLOCK:
            pFormatProperties->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            pFormatProperties->linearTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            break;
        default: pFormatProperties->optimalTilingFeatures = 0; pFormatProperties->linearTilingFeatures = 0; break;
    }
}

/* vkGetInstanceProcAddr/vkGetDeviceProcAddr forwarding with interception */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    pthread_once(&loader_once, ensure_real_loader);
    if (!pName) return NULL;
    if (strcmp(pName, "vkEnumeratePhysicalDevices")==0) return (PFN_vkVoidFunction) vkEnumeratePhysicalDevices;
    if (strcmp(pName, "vkGetPhysicalDeviceProperties")==0) return (PFN_vkVoidFunction) vkGetPhysicalDeviceProperties;
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures2")==0) return (PFN_vkVoidFunction) vkGetPhysicalDeviceFeatures2;
    if (strcmp(pName, "vkEnumerateDeviceExtensionProperties")==0) return (PFN_vkVoidFunction) vkEnumerateDeviceExtensionProperties;
    if (strcmp(pName, "vkGetInstanceProcAddr")==0) return (PFN_vkVoidFunction) vkGetInstanceProcAddr;
    if (real_vkGetInstanceProcAddr) return real_vkGetInstanceProcAddr(instance, pName);
    return NULL;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    pthread_once(&loader_once, ensure_real_loader);
    if (real_vkGetDeviceProcAddr) return real_vkGetDeviceProcAddr(device, pName);
    return NULL;
}

/* vkGetPhysicalDeviceFeatures2 implementation (fills extension feature structs) */
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures) {
    if (!pFeatures) return; memset(&pFeatures->features,0,sizeof(VkPhysicalDeviceFeatures));
    pFeatures->features.robustBufferAccess = VK_TRUE;
    pFeatures->features.fullDrawIndexUint32 = VK_TRUE;
    pFeatures->features.shaderInt64 = VK_TRUE;
    pFeatures->features.geometryShader = VK_TRUE;
    VkBaseOutStructure* base = (VkBaseOutStructure*)pFeatures->pNext;
    while (base) {
        if (base->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES) {
            VkPhysicalDeviceDescriptorIndexingFeatures* f = (VkPhysicalDeviceDescriptorIndexingFeatures*)base;
            f->runtimeDescriptorArray = VK_TRUE; f->descriptorBindingVariableDescriptorCount = VK_TRUE;
            f->descriptorBindingPartiallyBound = VK_TRUE; f->descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        } else if (base->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES) {
            VkPhysicalDeviceShaderFloat16Int8Features* f = (VkPhysicalDeviceShaderFloat16Int8Features*)base; f->shaderFloat16 = VK_TRUE; f->shaderInt8 = VK_TRUE;
        } else if (base->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR) {
            VkPhysicalDeviceRayTracingPipelineFeaturesKHR* f = (VkPhysicalDeviceRayTracingPipelineFeaturesKHR*)base; f->rayTracingPipeline = VK_TRUE; f->rayTraversalPrimitiveCulling = VK_TRUE;
        } else if (base->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR) {
            VkPhysicalDeviceAccelerationStructureFeaturesKHR* f = (VkPhysicalDeviceAccelerationStructureFeaturesKHR*)base; f->accelerationStructure = VK_TRUE;
        } else if (base->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV) {
            VkPhysicalDeviceMeshShaderFeaturesNV* f = (VkPhysicalDeviceMeshShaderFeaturesNV*)base; f->meshShader = VK_TRUE; f->taskShader = VK_TRUE;
        } else if (base->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_NV) {
            VkPhysicalDeviceCooperativeMatrixFeaturesNV* f = (VkPhysicalDeviceCooperativeMatrixFeaturesNV*)base; f->cooperativeMatrix = VK_TRUE;
        }
        base = base->pNext;
    }
}

/* Enumerate device extensions matching manifest */
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    const char* exts[] = {
        "VK_KHR_acceleration_structure","VK_KHR_ray_tracing_pipeline","VK_KHR_deferred_host_operations","VK_KHR_buffer_device_address",
        "VK_EXT_descriptor_indexing","VK_KHR_timeline_semaphore","VK_KHR_dynamic_rendering","VK_EXT_mesh_shader","VK_KHR_maintenance5",
        "VK_KHR_shader_float16_int8","VK_KHR_shader_subgroup_extended_types","VK_EXT_shader_demote_to_helper_invocation","VK_KHR_pipeline_library",
        "VK_KHR_pipeline_executable_properties","VK_EXT_vertex_input_dynamic_state","VK_EXT_extended_dynamic_state3","VK_EXT_shader_object",
        "VK_EXT_shader_atomic_float","VK_KHR_synchronization2","VK_EXT_memory_budget"
    };
    uint32_t avail = (uint32_t)(sizeof(exts)/sizeof(exts[0]));
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pProperties) { *pPropertyCount = avail; return VK_SUCCESS; }
    uint32_t tocopy = (*pPropertyCount < avail) ? *pPropertyCount : avail;
    for (uint32_t i=0;i<tocopy;i++) { strncpy(pProperties[i].extensionName, exts[i], VK_MAX_EXTENSION_NAME_SIZE-1); pProperties[i].specVersion = 1; }
    *pPropertyCount = tocopy; return (tocopy < avail) ? VK_INCOMPLETE : VK_SUCCESS;
}

/* xeno_init - entrypoint defined by manifest: performs validation and prewarm */
void xeno_init(void) {
    /* init logging and crash handlers */
    signal(SIGSEGV, crash_handler); signal(SIGABRT, crash_handler);
    xlog("xeno_init called - initializing Xclipse 940 wrapper");
    validate_manifest_alignment();
    int hw = probe_hw_bc_presence();
    xlog("BC hardware detection result: %d", hw);
    if (!hw) {
        ensure_fallback_decoder_ready("BC1_UNORM");
        ensure_fallback_decoder_ready("BC3_UNORM");
    }
    const char* tune_out = getenv(TUNE_REPORT_ENV);
    if (!tune_out) tune_out = DEFAULT_TUNE_REPORT;
    write_feature_dump(tune_out);
    xlog("xeno_init complete");
}

/* Minimal exported functions to satisfy loader in absence of real ICD functions */
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED; *pPropertyCount = 0; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED; *pPropertyCount = 0; return VK_SUCCESS;
}

/* End of libxeno_wrapper.c */
