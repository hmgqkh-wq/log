/* bc_emulate.c - comprehensive BCn fallback & compile queue utilities
 *
 * Provides:
 * - detection of HW vs SW path
 * - lookup and loading of SPIR-V fallback shaders
 * - synchronous compile queue simulation for autotune prewarm
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

static void bclog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); fprintf(stderr, "bc: "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap);
}

static int file_exists(const char* p) { struct stat st; return p && stat(p,&st)==0; }

const char* prepare_decoder_for_format(const char* vk_format_name) {
    if (!vk_format_name) return NULL;
    /* Check env override */
    const char* env = getenv("XCLIPSE_FORCE_HW_BC");
    if (env && strcmp(env,"0")==0) {
        bclog("HW BC disabled by env");
        /* fall through to search fallback */
    } else {
        /* default: return NULL to use HW */
        return NULL;
    }
    const char* names[] = {"bc1.spv","bc2.spv","bc3.spv","bc4.spv","bc5.spv","bc6h.spv","bc7.spv", NULL};
    int idx = -1;
    if (strstr(vk_format_name,"BC1")) idx = 0; else if (strstr(vk_format_name,"BC2")) idx = 1; else if (strstr(vk_format_name,"BC3")) idx = 2;
    else if (strstr(vk_format_name,"BC4")) idx = 3; else if (strstr(vk_format_name,"BC5")) idx = 4; else if (strstr(vk_format_name,"BC6")) idx = 5; else if (strstr(vk_format_name,"BC7")) idx = 6;
    if (idx < 0) return NULL;
    const char* search_paths[] = {"/usr/share/exynostools/shaders/pipeline_cache/","/assets/shaders/decode/","/usr/share/xclipse/shaders/",NULL};
    static char buf[1024];
    for (const char** p = search_paths; *p; ++p) {
        snprintf(buf,sizeof(buf),"%s%s", *p, names[idx]);
        if (file_exists(buf)) return strdup(buf);
    }
    bclog("No fallback SPV found for %s", vk_format_name);
    return NULL;
}

unsigned char* load_fallback_spv_blob(const char* vk_format_name, size_t* out_size) {
    const char* path = prepare_decoder_for_format(vk_format_name);
    if (!path) return NULL;
    FILE* f = fopen(path,"rb"); if (!f) return NULL;
    fseek(f,0,SEEK_END); long s = ftell(f); rewind(f); unsigned char* buf = malloc(s); if (!buf) { fclose(f); return NULL; }
    fread(buf,1,s,f); fclose(f); if (out_size) *out_size = s; return buf;
}

/* simple synchronous compile queue */
typedef struct job { char fmt[64]; char* path; unsigned char* blob; size_t size; int status; struct job* next; } job_t;
static job_t* head = NULL;
int ensure_fallback_decoder_ready(const char* vk_format_name) {
    job_t* j = calloc(1,sizeof(job_t)); if (!j) return -1; strncpy(j->fmt, vk_format_name?vk_format_name:"",63);
    j->path = NULL; const char* p = prepare_decoder_for_format(vk_format_name); if (p) j->path = strdup(p);
    j->status = 0; j->next = head; head = j;
    /* process synchronously */
    job_t* cur = head;
    while (cur) {
        if (cur->status == 0 && cur->path) {
            size_t s=0; unsigned char* b = load_fallback_spv_blob(cur->fmt, &s);
            if (b) { cur->blob = b; cur->size = s; cur->status = 1; bclog("Prepared fallback %s size=%zu", cur->fmt, s); }
            else { cur->status = -1; bclog("Failed to prepare fallback %s", cur->fmt); }
        }
        cur = cur->next;
    }
    return 0;
}

int bc_emulate_selftest(void) {
    bclog("bc_emulate selftest");
    ensure_fallback_decoder_ready("BC1_UNORM");
    ensure_fallback_decoder_ready("BC3_UNORM");
    ensure_fallback_decoder_ready("BC7_UNORM");
    bclog("bc_emulate selftest complete");
    return 0;
}
