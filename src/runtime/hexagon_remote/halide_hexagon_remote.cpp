extern "C" {

#include "halide_hexagon_remote.h"
#include <sys/mman.h>
#include <memory.h>
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define FARF_LOW 1
#include "HAP_farf.h"
#include "HAP_power.h"

}

#include "../HalideRuntime.h"

typedef halide_hexagon_remote_handle_t handle_t;
typedef halide_hexagon_remote_buffer buffer;


void halide_print(void *user_context, const char *str) {
    FARF(LOW, "%s", str);
}

void halide_error(void *user_context, const char *str) {
    halide_print(user_context, str);
}

void *halide_malloc(void *user_context, size_t x) {
    // Allocate enough space for aligning the pointer we return.
    const size_t alignment = 128;
    void *orig = malloc(x + alignment);
    if (orig == NULL) {
        // Will result in a failed assertion and a call to halide_error
        return NULL;
    }
    // We want to store the original pointer prior to the pointer we return.
    void *ptr = (void *)(((size_t)orig + alignment + sizeof(void*) - 1) & ~(alignment - 1));
    ((void **)ptr)[-1] = orig;
    return ptr;
}

void halide_free(void *user_context, void *ptr) {
    free(((void**)ptr)[-1]);
}

int halide_do_task(void *user_context, halide_task_t f, int idx,
                   uint8_t *closure) {
    return f(user_context, idx, closure);
}

int halide_do_par_for(void *user_context, halide_task_t f,
                      int min, int size, uint8_t *closure) {
    for (int x = min; x < min + size; x++) {
        int result = halide_do_task(user_context, f, x, closure);
        if (result) {
            return result;
        }
    }
    return 0;
}

extern "C" {

const int map_alignment = 4096;

typedef int (*set_runtime_t)(halide_malloc_t user_malloc,
                             halide_free_t custom_free,
                             halide_print_t print,
                             halide_error_handler_t error_handler,
                             halide_do_par_for_t do_par_for,
                             halide_do_task_t do_task);

int context_count = 0;

int halide_hexagon_remote_initialize_kernels(const unsigned char *code, int codeLen,
                                             handle_t *module_ptr) {
#if 1  // Use shared object from file
    const char *filename = (const char *)code;
#else
    const char *filename = "/data/halide_kernels.so";
    FILE* fd = fopen(filename, "w");
    if (!fd) {
        FARF(LOW, "fopen failed");
        return -1;
    }

    fwrite(code, codeLen, 1, fd);
    fclose(fd);
#endif
    void *lib = dlopen(filename, RTLD_LOCAL | RTLD_LAZY);
    if (!lib) {
        FARF(LOW, "dlopen failed");
        return -1;
    }

    // Initialize the runtime. The Hexagon runtime can't call any
    // system functions (because we can't link them), so we put all
    // the implementations that need to do so here, and pass poiners
    // to them in here.
    set_runtime_t set_runtime = (set_runtime_t)dlsym(lib, "halide_noos_set_runtime");
    if (!set_runtime) {
        dlclose(lib);
        FARF(LOW, "halide_noos_set_runtime not found in shared object");
        return -1;
    }

    int result = set_runtime(halide_malloc,
                             halide_free,
                             halide_print,
                             halide_error,
                             halide_do_par_for,
                             halide_do_task);
    if (result != 0) {
        dlclose(lib);
        FARF(LOW, "set_runtime failed %d", result);
        return result;
    }
    *module_ptr = reinterpret_cast<handle_t>(lib);

    if (context_count == 0) {
        FARF(LOW, "Requesting power for HVX...");

        HAP_power_request_t request;
        request.type = HAP_power_set_HVX;
        request.hvx.power_up = TRUE;
        int retval = HAP_power_set(NULL, &request);
        if (0 != retval) {
            FARF(ERROR,"dspCV unable to power on HVX, status %d!", retval);
            return -1;
        }
    }
    context_count++;

    return 0;
}

handle_t halide_hexagon_remote_get_symbol(handle_t module_ptr, const char* name, int nameLen) {
    return reinterpret_cast<handle_t>(dlsym(reinterpret_cast<void*>(module_ptr), name));
}

int halide_hexagon_remote_run(handle_t module_ptr, handle_t function,
                              const buffer *input_buffersPtrs, int input_buffersLen,
                              const buffer *input_scalarsPtrs, int input_scalarsLen,
                              buffer *output_buffersPtrs, int output_buffersLen) {
    // Get a pointer to the argv version of the pipeline.
    typedef int (*pipeline_argv_t)(void **);
    pipeline_argv_t pipeline = reinterpret_cast<pipeline_argv_t>(function);

    // Construct a list of arguments. This is only part of a
    // buffer_t. We know that the only field of buffer_t that the
    // generated code should access is the host field (any other
    // fields should be passed as their own scalar parameters) so we
    // can just make this dummy buffer_t type.
    struct buffer_t {
        uint64_t dev;
        uint8_t* host;
    };
    void **args = (void **)__builtin_alloca((input_buffersLen + input_scalarsLen + output_buffersLen) * sizeof(void *));
    buffer_t *buffers = (buffer_t *)__builtin_alloca((input_buffersLen + output_buffersLen) * sizeof(buffer_t));

    void **next_arg = &args[0];
    buffer_t *next_buffer_t = &buffers[0];
    // Input buffers come first.
    for (int i = 0; i < input_buffersLen; i++, next_arg++, next_buffer_t++) {
        next_buffer_t->host = input_buffersPtrs[i].data;
        *next_arg = next_buffer_t;
    }
    // Input scalars are next.
    for (int i = 0; i < input_scalarsLen; i++, next_arg++) {
        *next_arg = input_scalarsPtrs[i].data;
    }
    // Output buffers are last.
    for (int i = 0; i < output_buffersLen; i++, next_arg++, next_buffer_t++) {
        next_buffer_t->host = output_buffersPtrs[i].data;
        *next_arg = next_buffer_t;
    }

    // Call the pipeline and return the result.
    return pipeline(args);
}

int halide_hexagon_remote_release_kernels(handle_t module_ptr, int codeLen) {
    dlclose(reinterpret_cast<void*>(module_ptr));

    if (context_count-- == 0) {
        HAP_power_request(0, 0, -1);
    }

    return 0;
}

}  // extern "C"