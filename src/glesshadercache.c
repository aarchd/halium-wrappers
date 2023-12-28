#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include <signal.h>
#include <GLES2/gl2.h>

#define GL_GLEXT_PROTOTYPES
#include <GLES2/gl2ext.h>

// #define _DEBUG

#ifdef _DEBUG
#define debug(fmt, ...) fprintf(stderr, "[blueberry] " __FILE__ ":%d " fmt, __LINE__, ##__VA_ARGS__)
#else
#define debug(fmt, ...)
#endif

typedef void (GL_APIENTRYP PFNGLCOMPILESHADERPROC) (GLuint shader);
typedef void (GL_APIENTRYP PFNGLLINKPROGRAMPROC) (GLuint program);
typedef void (GL_APIENTRYP PFNGLGETSHADERIVPROC) (GLuint shader, GLenum pname, GLint *params);

static PFNGLCOMPILESHADERPROC real_glCompileShader = NULL;
static PFNGLLINKPROGRAMPROC real_glLinkProgram = NULL;
static PFNGLGETSHADERIVPROC real_glGetShaderiv = NULL;

void init_original_functions() {
    real_glCompileShader = (PFNGLCOMPILESHADERPROC)dlsym(RTLD_NEXT, "glCompileShader");
    real_glLinkProgram = (PFNGLLINKPROGRAMPROC)dlsym(RTLD_NEXT, "glLinkProgram");
    real_glGetShaderiv = (PFNGLGETSHADERIVPROC)dlsym(RTLD_NEXT, "glGetShaderiv");

    if (!real_glCompileShader || !real_glLinkProgram || !real_glGetShaderiv) {
        debug("Bad news: couldn't find these real function pointers:\n");
        if (!real_glCompileShader) debug("- glCompileShader\n");
        if (!real_glLinkProgram) debug("- glLinkProgram\n");
        if (!real_glGetShaderiv) debug("- glGetShaderiv\n");

        // TODO: do we actually need to exit? Could the program still work if we do nothing?
        // I assume we're FUBAR if we can't find the functions anyway...
        exit(EXIT_FAILURE);
    }
}

const char *get_env_cache_path() {
    static const char *cache_path;
    static int warned = 0;
    if (cache_path || warned) return cache_path;

    cache_path = getenv("GLES_CACHE_PATH");
    if (cache_path) {
        debug("Using GLES_CACHE_PATH %s\n", cache_path);
        return cache_path;
    }

    if (warned && !cache_path) return NULL;

    if (!cache_path) {
        warned = 1;
        debug("GLES_CACHE_PATH not specified.\n");
        const char *xdg_cache_home = getenv("XDG_CACHE_HOME");

        if (!xdg_cache_home) {
            debug("XDG_CACHE_HOME not specified either. Winging it.\n");
            const char *home = getenv("HOME");
            static const char *home_cache_postfix = ".cache";
            char *path = malloc(strlen(home) + strlen(home_cache_postfix) + 2);
            strcpy(path, home);
            strcat(path, "/");
            strcat(path, home_cache_postfix);

            xdg_cache_home = path;
        }

        if (xdg_cache_home) {
            static const char *cache_dir = "gles-cache";
            char *path = malloc(strlen(xdg_cache_home) + strlen(cache_dir) + 2);
            strcpy(path, xdg_cache_home);
            strcat(path, "/");
            strcat(path, cache_dir);

            debug("Trying to use %s as GLES_CACHE_PATH\n", path);

            // create the directory if it doesn't exist
            if (access(path, F_OK) != 0) {
                mkdir(path, 0755);
            }

            cache_path = path;
        }

        if (!cache_path) {
            debug("Tried to create gles-cache under XDG_CACHE_HOME, but that didn't work either. You're on your own.\n");
            return NULL;
        }
    }

    if (cache_path && access(cache_path, F_OK | W_OK) == 0) {
        return cache_path;
    } else {
        debug("Cache path not specified, or is not writable.\n");
        warned = 1;
        cache_path = NULL;

        return NULL;
    }
}

unsigned long calculate_crc32(const char *data, size_t length) {
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)data, length);
    return crc;
}

// TODO: Arrays like this maybe not the best choice
#define MAX_SHADERS 1024
int SPOOFED_SHADERS[MAX_SHADERS] = {0};
int SHADER_CRCS[MAX_SHADERS] = {0};

void glCompileShader(GLuint shader) {
    if (!real_glCompileShader) {
        init_original_functions();
    }

    const char *cache_path = get_env_cache_path();
    if (!cache_path) {
        real_glCompileShader(shader);
        return;
    }

    GLint length = 0;
    real_glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &length);

    GLchar *shader_source = (GLchar *)malloc(length);
    glGetShaderSource(shader, length, NULL, shader_source);

    unsigned long crc = calculate_crc32(shader_source, strlen(shader_source));
    SHADER_CRCS[shader] = crc;

    free(shader_source);

    // If we have a matching "cached shader", skip the compilation and lie through your teeth
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%lu.shader.bin", cache_path, crc);
    if (access(filepath, F_OK) == 0) {
        SPOOFED_SHADERS[shader] = 1;
        return;
    }

    // Otherwise, compile the shader as usual
    real_glCompileShader(shader);

    // And if it goes ok, cache the result
    GLint compileStatus = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compileStatus);

    if (compileStatus == GL_TRUE) {
        if (cache_path) {
            FILE *file = fopen(filepath, "wb");
            if (file) {
                fwrite("1", 1, 1, file);
                fclose(file);
            } else {
                debug("Failed to open file %s for writing.\n", filepath);
            }
        }
    } else {
        debug("Failed to compile shader.\n");
    }
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint *params) {
    if (!real_glGetShaderiv) {
        init_original_functions();
    }

    if (SPOOFED_SHADERS[shader]) {
        if (pname == GL_COMPILE_STATUS) {
            *params = GL_TRUE;
            return;
        }

        // TODO: what if someone asks about anything else? We should probably cache that too
        // For now, just pass it through and hope we don't crash and burn
        debug("glGetShaderiv called for spoofed shader and pname %d - this could be bad\n", pname);
    }

    real_glGetShaderiv(shader, pname, params);
}

void cache_program(GLuint program, unsigned long crc, const char *cache_path) {
    GLint length = 0;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH_OES, &length);

    GLvoid *binary = (GLvoid *) malloc(length);
    GLenum format = 0;
    glGetProgramBinaryOES(program, length, &length, &format, binary);

    if (length > 0) {
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%lu.program.bin", cache_path, crc);
        FILE *file = fopen(filepath, "wb");
        if (file) {
            debug("Caching program %lu with format %d and size %d.\n", crc, format, length);
            fwrite(&format, sizeof(GLenum), 1, file);
            fwrite(binary, length, 1, file);
            fclose(file);
        } else {
            debug("Failed to open file %s for writing.\n", filepath);
        }

        free(binary);
    }
}

int load_program_binary(GLuint program, unsigned long crc, const char *cache_path) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%lu.program.bin", cache_path, crc);
    FILE *file = fopen(filepath, "rb");
    if (file) {
        GLenum format = 0;
        if (fread(&format, sizeof(GLenum), 1, file) != 1) {
            debug("Error reading program binary format.\n");
            fclose(file);
            return 0;
        }

        fseek(file, 0, SEEK_END);
        long length = ftell(file) - sizeof(GLenum);
        fseek(file, sizeof(GLenum), SEEK_SET);

        void *binary = malloc(length);
        if (binary) {
            if (fread(binary, length, 1, file) != 1) {
                debug("Error reading program binary data.\n");
                fclose(file);
                free(binary);
                return 0;
            }

            debug("Loading program %lu with format %d and size %d.\n", crc, format, length);
            glProgramBinaryOES(program, format, binary, length);
            fclose(file);
            free(binary);
            return 1;
        } else {
            debug("Error allocating memory for program binary data. File might be corrupt.\n");
            fclose(file);
            return 0;
        }
    }
    return 0;
}

void glLinkProgram(GLuint program) {
    if (!real_glLinkProgram) {
        init_original_functions();
    }

    const char *cache_path = get_env_cache_path();
    if (!cache_path) {
        real_glLinkProgram(program);
        return;
    }

    GLint numShaders = 0;
    glGetProgramiv(program, GL_ATTACHED_SHADERS, &numShaders);
    GLuint *shaders = (GLuint *)malloc(numShaders * sizeof(GLuint));
    glGetAttachedShaders(program, numShaders, NULL, shaders);

    // Calculate the CRC of each shader and figure out if we have a cached version for everyone
    int all_cached = 1;
    int crc_of_crcs = 0;

    for (int i = 0; i < numShaders; i++) {
        // TODO: This "CRC of CRCs" is embarrassing
        crc_of_crcs += SHADER_CRCS[shaders[i]];

        if (!SPOOFED_SHADERS[shaders[i]]) {
            // Shoot, we don't have a cached version of this shader
            all_cached = 0;
        }
    }

    if (!all_cached || !load_program_binary(program, crc_of_crcs, cache_path)) {
        // If we lied about the compilation status of any of the shaders, we need to compile them now
        for (int i = 0; i < numShaders; i++) {
            if (SPOOFED_SHADERS[shaders[i]]) {
                debug("Late-compiling shader %d\n", shaders[i]);
                real_glCompileShader(shaders[i]);
                SPOOFED_SHADERS[shaders[i]] = 0;
            }
        }

        debug("Linking program as usual.\n");
        real_glLinkProgram(program);

        GLint linkStatus = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);

        if (linkStatus == GL_TRUE) {
            cache_program(program, crc_of_crcs, cache_path);
        } else {
            debug("Failed to link program.\n");
        }
    }

    free(shaders);
}

void **epoxy_glLinkProgram = (void **) &glLinkProgram;
void **epoxy_glCompileShader = (void **) &glCompileShader;
void **epoxy_glGetShaderiv = (void **) &glGetShaderiv;
