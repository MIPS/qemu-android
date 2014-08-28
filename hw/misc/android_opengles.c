/*
 * ARM Android emulator OpenGLES backend
 *
 * Copyright (c) 2014 Android Open Source Project
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Handle connections to the 'opengles' pipe from Android guests and route
 * traffic over this pipe to the GPU emulation libraries.
 */

#include "hw/misc/android_opengles.h"
#include "hw/misc/android_pipe.h"
#include "qemu/compiler.h"
#include "qemu-common.h"

#include <limits.h>

#ifdef _WIN32
#include <errno.h>
#include <windows.h>
#else
#include <dlfcn.h>
#endif


// #define DEBUG_OPENGLES

#ifdef DEBUG_OPENGLES
#define DPRINTF(fmt, ...) \
do { fprintf(stderr, "adb_debug: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do {} while(0)
#endif

// Loading shared libraries and probing their symbols.
typedef struct DynamicLibrary DynamicLibrary;

static DynamicLibrary* dynamic_library_open(const char* library_name)
{
    char path[PATH_MAX];

#ifdef _WIN32
    static const char kDllExtension[] = ".dll";
#elif defined(__APPLE__)
    static const char kDllExtension[] = ".dylib";
#else
    static const char kDllExtension[] = ".so";
#endif
    if (!strchr(library_name, '.')) {
        snprintf(path, sizeof path, "%s%s", library_name, kDllExtension);
    } else {
        snprintf(path, sizeof path, "%s", library_name);
    }
#ifdef _WIN32
    return (DynamicLibrary*)LoadLibrary(path);
#else
    return (DynamicLibrary*)dlopen(path, RTLD_NOW);
#endif
}

static void dynamic_library_close(DynamicLibrary* library)
{
#ifdef _WIN32
    if (library) {
        FreeLibrary((HMODULE)library);
    }
#else
    if (library) {
        dlclose(library);
    }
#endif
}

static void* dynamic_library_probe(DynamicLibrary* library,
                                   const char* symbol)
{
    if (!library || !symbol || !symbol[0]) {
        return NULL;
    }
#ifdef _WIN32
    return GetProcAddress((HMODULE)library, symbol);
#else
    return dlsym(library, symbol);
#endif
}

//
// Loading the GPU shared libraries
//

// Name of the GLES rendering library we're going to use.
#if UINTPTR_MAX == UINT32_MAX
#define RENDERER_LIB_NAME  "libOpenglRender"
#elif UINTPTR_MAX == UINT64_MAX
#define RENDERER_LIB_NAME  "lib64OpenglRender"
#else
#error Unknown UINTPTR_MAX
#endif

// NOTE: The declarations below should be equivalent to those in
// <libOpenglRender/render_api_platform_types.h>
#ifdef _WIN32
typedef HDC FBNativeDisplayType;
typedef HWND FBNativeWindowType;
#elif defined(__linux__)
// Really a Window, which is defined as 32-bit unsigned long on all platforms
// but we don't want to include the X11 headers here.
typedef uint32_t FBNativeWindowType;
#elif defined(__APPLE__)
typedef void* FBNativeWindowType;
#else
#warning "unsupported platform"
#endif

// List of GPU emulation library functions.
#define RENDERER_FUNCTIONS_LIST \
  FUNCTION_(int, initLibrary, (void)) \
  FUNCTION_(int, setStreamMode, (int mode)) \
  FUNCTION_(int, initOpenGLRenderer, (int width, int height, char* addr, size_t addrLen)) \
  FUNCTION_(void, getHardwareStrings, (const char** vendors, const char** renderer, const char** version)) \
  FUNCTION_(void, setPostCallback, (AndroidGLESOnPostFunc onPost, void* onPostContext)) \
  FUNCTION_(int, createOpenGLSubwindow, (FBNativeWindowType window, int x, int y, int width, int height, float zRot)) \
  FUNCTION_(int, destroyOpenGLSubwindow, (void)) \
  FUNCTION_(void, setOpenGLDisplayRotation, (float zRot)) \
  FUNCTION_(void, repaintOpenGLDisplay, (void)) \
  FUNCTION_(int, stopOpenGLRenderer, (void)) \

// Name of static variable that points to symbol |name| in the GPU
// emulation library.
#define EMUGL_WRAPPER(name)  emugl_ ## name

// Define the corresponding function pointers as global variables, with
// an 'emugl_' prefix.
#define FUNCTION_(ret, name, sig) \
        static ret (*EMUGL_WRAPPER(name)) sig = NULL;
RENDERER_FUNCTIONS_LIST
#undef FUNCTION_


// Define a function that initializes the function pointers by looking up
// the symbols from the shared library.
static int init_opengles_emulation_functions(DynamicLibrary* library)
{
    void *symbol;

#define FUNCTION_(ret, name, sig) \
    symbol = dynamic_library_probe(library, #name); \
    if (!symbol) { \
        DPRINTF("GLES emulation: Could not find required symbol (%s): %s\n", #name); \
        return -1; \
    } \
    EMUGL_WRAPPER(name) = symbol;

RENDERER_FUNCTIONS_LIST
#undef FUNCTION_

    return 0;
}

// list of constants to be passed to setStreamMode, which determines how
// to send/receive wire protocol data to/from the library.
// DEFAULT -> try to use the best for the current platform.
// TCP -> use a TCP socket to send the protocol bytes to the library.
// UNIX -> use a Unix domain socket (faster than TCP, but Unix-only).
// WIN32_PIPE -> use a Win32 PIPE (unsupported by the library for now!).
enum {
    STREAM_MODE_DEFAULT = 0,
    STREAM_MODE_TCP = 1,
    STREAM_MODE_UNIX = 2,
    STREAM_MODE_WIN32_PIPE = 3,
};

struct AndroidGLES {
    bool init;
    DynamicLibrary* renderer_lib;
    bool renderer_started;
    char renderer_address[256];
};

static AndroidGLES sState = {
    .init = false,
    .renderer_lib = NULL,
    .renderer_started = false,
};

AndroidGLES* android_gles_init(void)
{
    AndroidGLES* state = &sState;

    if (state->init) {
        return state;
    }

    // Try to load the library.
    state->renderer_lib = dynamic_library_open(RENDERER_LIB_NAME);
    if (!state->renderer_lib) {
        DPRINTF("Could not load GPU emulation library!\n");
        return NULL;
    }

    // Resolve all required symbols from it.
    if (init_opengles_emulation_functions(state->renderer_lib) < 0) {
        DPRINTF("OpenGLES emulation library mismatch. Bes ure to use the correct version!\n");
        goto BAD_EXIT;
    }

    // Call its initialization function.
    if (!EMUGL_WRAPPER(initLibrary)()) {
        DPRINTF("OpenGLES initialization failed!\n");
        goto BAD_EXIT;
    }

#ifdef _WIN32
    // NOTE: Win32 PIPE support is still not implemented.
    EMUGL_WRAPPER(setStreamMode)(STREAM_MODE_TCP);
#else
    EMUGL_WRAPPER(setStreamMode)(STREAM_MODE_UNIX);
#endif

    state->init = true;
    return state;

BAD_EXIT:
    DPRINTF("OpenGLES library could not be initialized\n");
    dynamic_library_close(state->renderer_lib);

    state->renderer_lib = NULL;
    return NULL;
}

static void extract_base_string(char* dst, const char* src, size_t dstSize)
{
    const char* begin = strchr(src, '(');
    const char* end = strrchr(src, ')');

    if (!begin || !end) {
        pstrcpy(dst, dstSize, src);
        return;
    }
    begin += 1;

    // "foo (bar)"
    //       ^  ^
    //       b  e
    //     = 5  8
    // substring with NUL-terminator is end-begin+1 bytes
    if (end - begin + 1 > dstSize) {
        end = begin + dstSize - 1;
    }

    pstrcpy(dst, end - begin + 1, begin);
}

void android_gles_get_hardware_strings(char *vendor,
                                       size_t vendor_buffer_size,
                                       char *renderer,
                                       size_t renderer_buffer_size,
                                       char *version,
                                       size_t version_buffer_size)
{
    const char *vendor_src, *renderer_src, *version_src;
    AndroidGLES *state = &sState;

    if (!state->renderer_started) {
        DPRINTF("Can't get OpenGL ES hardware strings when renderer not started");
        vendor[0] = renderer[0] = version[0] = '\0';
        return;
    }

    EMUGL_WRAPPER(getHardwareStrings)(&vendor_src,
                                      &renderer_src,
                                      &version_src);
    if (!vendor_src) vendor_src = "";
    if (!renderer_src) renderer_src = "";
    if (!version_src) version_src = "";

    /* Special case for the default ES to GL translators: extract the strings
     * of the underlying OpenGL implementation. */
    if (strncmp(vendor_src, "Google", 6) == 0 &&
            strncmp(renderer_src, "Android Emulator OpenGL ES Translator", 37) == 0) {
        extract_base_string(vendor, vendor_src, vendor_buffer_size);
        extract_base_string(renderer, renderer_src, renderer_buffer_size);
        extract_base_string(version, version_src, version_buffer_size);
    } else {
        pstrcpy(vendor, vendor_buffer_size, vendor_src);
        pstrcpy(renderer, renderer_buffer_size, renderer_src);
        pstrcpy(version, version_buffer_size, version_src);
    }
}

int android_gles_start(AndroidGLES* state, int width, int height)
{
    if (!state->renderer_lib) {
        DPRINTF("Can't start OpenGLES renderer without support libraries\n");
        return -1;
    }

    if (state->renderer_started) {
        // Already started.
        return 0;
    }

    if (!EMUGL_WRAPPER(initOpenGLRenderer)(
            width,
            height,
            state->renderer_address,
            sizeof(state->renderer_address))) {
        DPRINTF("Can't start OpenGLES renderer !?\n");
        return -1;
    }

    state->renderer_started = true;
    return 0;
}

void android_gles_set_post_callback(AndroidGLES* state,
                                    AndroidGLESOnPostFunc on_post,
                                    void* on_post_context)
{
    if (state->renderer_lib) {
        EMUGL_WRAPPER(setPostCallback)(on_post, on_post_context);
    }
}

int android_gles_show_window(AndroidGLES* state,
                             void* window,
                             int x,
                             int y,
                             int width,
                             int height,
                             float rotation)
{
    if (state->renderer_started) {
        int success = EMUGL_WRAPPER(createOpenGLSubwindow)(
                (FBNativeWindowType)(uintptr_t)window,
                x,
                y,
                width,
                height,
                rotation);
        return success ? 0 : -1;
    } else {
        return -1;
    }
}

void android_gles_hide_window(AndroidGLES* state)
{
    if (state->renderer_started) {
        EMUGL_WRAPPER(destroyOpenGLSubwindow)();
    }
}

void android_gles_redraw_window(AndroidGLES* state) {
    if (state->renderer_started) {
        EMUGL_WRAPPER(repaintOpenGLDisplay)();
    }
}

void android_gles_server_path(char* buff, size_t buff_size)
{
    AndroidGLES *state = &sState;
    pstrcpy(buff, buff_size, state->renderer_address);
}
