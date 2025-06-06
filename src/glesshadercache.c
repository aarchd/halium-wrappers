#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include <GLES2/gl2.h>

#define GL_GLEXT_PROTOTYPES
#include <GLES2/gl2ext.h>

// #define _DEBUG

#ifdef _DEBUG
# define debug(fmt, ...) fprintf(stderr, "[gles-cache] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
# define debug(fmt, ...)
#endif

typedef void (GL_APIENTRYP PFNGLCOMPILESHADERPROC)(GLuint shader);
typedef void (GL_APIENTRYP PFNGLLINKPROGRAMPROC)(GLuint program);
typedef void (GL_APIENTRYP PFNGLGETSHADERIVPROC)(GLuint shader, GLenum pname, GLint *params);

static PFNGLCOMPILESHADERPROC real_glCompileShader;
static PFNGLLINKPROGRAMPROC real_glLinkProgram;
static PFNGLGETSHADERIVPROC real_glGetShaderiv;

static void init_original_functions(void) {
    real_glCompileShader = (PFNGLCOMPILESHADERPROC)dlsym(RTLD_NEXT, "glCompileShader");
    real_glLinkProgram = (PFNGLLINKPROGRAMPROC)dlsym(RTLD_NEXT, "glLinkProgram");
    real_glGetShaderiv = (PFNGLGETSHADERIVPROC)dlsym(RTLD_NEXT, "glGetShaderiv");

    if (!real_glCompileShader || !real_glLinkProgram || !real_glGetShaderiv) {
        debug("Failed to get original GL function pointers.");
        exit(EXIT_FAILURE);
    }
}

static const char *get_env_cache_path(void) {
    static char *cached = NULL;
    if (cached) return cached;

    const char *path = getenv("GLES_CACHE_PATH");
    if (path && access(path, W_OK) == 0)
        return cached = strdup(path);

    const char *xdg = getenv("XDG_CACHE_HOME");
    if (!xdg) {
        const char *home = getenv("HOME");
        if (!home) return NULL;

        size_t len = strlen(home) + strlen("/.cache") + 1;
        char *cache_dir = malloc(len);
        if (!cache_dir) return NULL;
        snprintf(cache_dir, len, "%s/.cache", home);
        xdg = cache_dir;
    }

    size_t len = strlen(xdg) + strlen("/gles-cache") + 1;
    cached = malloc(len);
    if (!cached) {
        if (!getenv("XDG_CACHE_HOME")) free((void*)xdg);
        return NULL;
    }
    snprintf(cached, len, "%s/gles-cache", xdg);
    mkdir(cached, 0755);

    if (!getenv("XDG_CACHE_HOME")) free((void*)xdg);
    return cached;
}

static unsigned long calculate_crc32(const char *data, size_t len) {
    uLong crc = crc32(0L, Z_NULL, 0);
    return crc32(crc, (const Bytef*)data, len);
}

#define MAX_SHADERS 1024
typedef struct { GLuint id; unsigned long crc; int spoofed; } ShaderInfo;

static ShaderInfo shader_cache[MAX_SHADERS];
static size_t shader_cache_count = 0;

static ShaderInfo* get_shader_info(GLuint id) {
    for (size_t i=0; i<shader_cache_count; ++i)
        if (shader_cache[i].id == id) return &shader_cache[i];
    if (shader_cache_count < MAX_SHADERS) {
        ShaderInfo *si = &shader_cache[shader_cache_count++];
        si->id = id; si->crc = 0; si->spoofed = 0;
        return si;
    }
    return NULL;
}

static unsigned long combine_crcs(const unsigned long *crcs, int n) {
    uLong combined = crc32(0L, Z_NULL, 0);
    for (int i=0; i<n; ++i)
        combined = crc32(combined, (const Bytef*)&crcs[i], sizeof(unsigned long));
    return combined;
}

void glCompileShader(GLuint shader) {
    if (!real_glCompileShader) init_original_functions();

    ShaderInfo *si = get_shader_info(shader);
    if (!si) { real_glCompileShader(shader); return; }

    const char *cache = get_env_cache_path();
    if (!cache) { real_glCompileShader(shader); return; }

    GLint len=0;
    real_glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &len);
    if (len <= 1) { real_glCompileShader(shader); return; }

    char *src = malloc(len);
    glGetShaderSource(shader, len, NULL, src);
    si->crc = calculate_crc32(src, len);
    free(src);

    char fn[512];
    snprintf(fn, sizeof(fn), "%s/%lu.shader.bin", cache, si->crc);
    if (access(fn, F_OK) == 0) {
        si->spoofed = 1;
        return;
    }

    real_glCompileShader(shader);
    GLint ok=0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok) {
        FILE *f = fopen(fn, "wb");
        if (f) { fwrite("1",1,1,f); fclose(f); }
    } else {
        debug("Shader %u compile failed", shader);
    }
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint *p) {
    if (!real_glGetShaderiv) init_original_functions();

    ShaderInfo *si = get_shader_info(shader);
    if (si && si->spoofed && pname == GL_COMPILE_STATUS) {
        *p = GL_TRUE;
        return;
    }
    real_glGetShaderiv(shader, pname, p);
}

static int load_program_binary(GLuint prog, unsigned long crc, const char *cache) {
    char fn[512];
    snprintf(fn, sizeof(fn), "%s/%lu.program.bin", cache, crc);
    FILE *f = fopen(fn,"rb");
    if (!f) return 0;

    GLenum fmt=0;
    fread(&fmt,sizeof(fmt),1,f);
    fseek(f,0,SEEK_END);
    long len = ftell(f) - sizeof(fmt);
    fseek(f,sizeof(fmt),SEEK_SET);

    void *bin = malloc(len);
    fread(bin,len,1,f);
    glProgramBinaryOES(prog, fmt, bin, len);
    free(bin);
    fclose(f);
    debug("Loaded program %lu binary", crc);
    return 1;
}

static void cache_program_binary(GLuint prog, unsigned long crc, const char *cache) {
    GLint len=0;
    glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH_OES, &len);
    if (len <= 0) return;

    void *bin = malloc(len);
    GLenum fmt=0;
    glGetProgramBinaryOES(prog, len, NULL, &fmt, bin);

    char fn[512];
    snprintf(fn, sizeof(fn), "%s/%lu.program.bin", cache, crc);
    FILE *f = fopen(fn, "wb");
    if (f) {
        fwrite(&fmt, sizeof(fmt), 1, f);
        fwrite(bin, len, 1, f);
        fclose(f);
        debug("Cached program %lu", crc);
    }
    free(bin);
}

void glLinkProgram(GLuint prog) {
    if (!real_glLinkProgram) init_original_functions();

    const char *cache = get_env_cache_path();
    if (!cache) { real_glLinkProgram(prog); return; }

    GLint ns=0;
    glGetProgramiv(prog, GL_ATTACHED_SHADERS, &ns);
    if (ns <= 0) { real_glLinkProgram(prog); return; }

    GLuint *sh = malloc(ns*sizeof(GLuint));
    glGetAttachedShaders(prog, ns, NULL, sh);

    unsigned long *crcs = malloc(ns*sizeof(unsigned long));
    int all_cached = 1;
    for (int i=0; i<ns; ++i) {
        ShaderInfo *si = get_shader_info(sh[i]);
        crcs[i] = si ? si->crc : 0;
        if (!si || !si->spoofed) all_cached = 0;
    }

    unsigned long pcrc = combine_crcs(crcs, ns);
    free(crcs);

    if (all_cached && load_program_binary(prog, pcrc, cache)) {
        free(sh);
        return;
    }

    for (int i=0; i<ns; ++i) {
        ShaderInfo *si = get_shader_info(sh[i]);
        if (si && si->spoofed) {
            debug("Late compile shader %u", sh[i]);
            real_glCompileShader(sh[i]);
            si->spoofed = 0;
        }
    }

    real_glLinkProgram(prog);
    GLint ok=0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok) cache_program_binary(prog, pcrc, cache);
    free(sh);
}

void **epoxy_glCompileShader = (void**)&glCompileShader;
void **epoxy_glGetShaderiv   = (void**)&glGetShaderiv;
void **epoxy_glLinkProgram   = (void**)&glLinkProgram;
