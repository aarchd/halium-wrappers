/*
 * Based on: libhybris/hybris/properties/hybris_properties.c:
 * Copyright (c) 2018 Jolla Ltd. <franz.haider@jolla.com>
 * Copyright (c) 2020 UBports foundation <marius@ubports.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fnmatch.h>
#include <dlfcn.h>
#include <unistd.h>

#include <hybris/properties/properties.h>
#include <hybris/common/binding.h>

static void *libc = NULL;

static int tripped = 0;

static const char* property_value;

static int (*bionic___system_property_wait_any)(int __old_serial) = NULL;

typedef struct {
    int count;
    char **argv;
} arguments_t;

static void unload_libc(void)
{
    if (libc) {
        android_dlclose(libc);
    }
}

#define LIBC_DLSYM(func) {*(void **)(&bionic_##func) = (void*)android_dlsym(libc, #func); \
                              if (!bionic_##func) { \
                                  fprintf(stderr, "failed to load " #func " from bionic libcutils\n"); \
                                  abort(); \
                              }}

static void ensure_bionic_libc_initialized()
{
    if (!libc) {
        libc = android_dlopen("libc.so", RTLD_LAZY);
        if (libc) {
            LIBC_DLSYM(__system_property_wait_any);
            atexit(unload_libc);
        } else {
            fprintf(stderr, "failed to load bionic libc.so, falling back own property implementation\n");
            abort();
        }
    }
}

static void wait_for_property_service()
{
    int success;

    do {
        success = access("/dev/socket/property_service", F_OK);

        if (success == -1) {
            usleep(100000);
        }

    } while (success == -1);
}

static void parse_properties(const char* key, const char* name, void* cookie)
{
    arguments_t *args = (arguments_t *)cookie;

    /* Don't bother if we've been tripped */
    if (tripped) {
        return;
    }

    for (int i = 1; i < args->count; i++) {
        if (fnmatch(args->argv[i], key, FNM_NOESCAPE | FNM_EXTMATCH) == 0 &&
                strcmp(name, property_value) == 0) {
            /* Found something! */
            fprintf(stdout, "%s: %s\n", key, name);
            tripped = 1;
            break;
        }
    }
}

int main(int argc, char **argv)
{
    arguments_t arguments = { .count = argc, .argv = argv };
    unsigned serial;

    if (argc == 1) {
        fprintf(stderr, "USAGE: waitforservice PROP1 PROP2 ... PROPN\n");
        return 1;
    }

    const char* selected_property_value = getenv("WAITFORSERVICE_VALUE");
    if (selected_property_value != NULL) {
        property_value = selected_property_value;
    } else {
        property_value = "running";
    }

    wait_for_property_service();

    ensure_bionic_libc_initialized();

    /*
     * Logic of this is trivial: wait until at least one of the supplied
     * system properties is set to 'running'.
     * This is especially useful when trying to determine whether a
     * service started by the Android init is now running.
     * For example,
     *     waitforservice \
     *         init.svc.vendor.hwcomposer-2-1 \
     *         init.svc.vendor.hwcomposer-2-2 \
     *         init.svc.vendor.hwcomposer-2-*
     * Will return once at least one of the supplied properties compares
     * to 'running', i.e. Android init reported that as running.
     * Wildcards are supported, pattern matching is being done with
     * fnmatch().
     *
     * This is accomplished via bionic's __system_property_wait_any()
     * function, which is deprecated (but still available at least up
     * to Android R).
     * There is no way to set a timeout yet.
    */

    for (serial = 0;;) {
        serial = bionic___system_property_wait_any(serial);

        property_list(parse_properties, &arguments);

        if (tripped) {
            return 0;
        }
    }
}
