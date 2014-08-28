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

 #ifndef _HW_MISC_ANDROID_OPENGLES_H
 #define _HW_MISC_ANDROID_OPENGLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Declarations related to Android-specific GPU emulation support.
 *
 * This works as follows:
 *
 * - GPU emulation support is implemented by an external shared library
 *   (e.g. libOpenglRender.so on Linux), which provides a small set of
 *   well-known entry points.
 *
 * - QEMU itself acts as a 'dumb pipe' between the guest system, and
 *   the GPU emulation library. More specifically:
 *
 *     o The guest EGL/GLES system libraries marshall all requests into
 *       a specific wire protocol stream of bytes. The corresponding data is
 *       sent directly to QEMU through the "opengles" Android pipe service.
 *
 *     o QEMU sends the data directly to the GPU emulation library,
 *       without trying to process or interpret it. Note that traffic goes
 *       both ways.
 *
 * This design avoids the need for a specific GPU driver in the kernel,
 * or any knowledge of the wire protocol being used in QEMU itself.
 *
 * - The GPU emulation library will display an OpenGL window _on_ _top_ of
 *   the current window, which will hide the framebuffer completely.
 *
 *   To do so, QEMU needs to provide the platform-specific 'id' of the
 *   current window. See android_gles_show_window() documentation for
 *   more details.
 */

/* Opaque data structure modelling the state of GPU emulation support for
 * Android. */
typedef struct AndroidGLES AndroidGLES;

/* Initialize the Android GPU emulation support. This function tries to
 * locate, load and initialize the GPU emulation library, and returns,
 * on success, a new AndroidGLES instance that can be used to call other
 * functions below. Return NULL/errno on failure.
 */
AndroidGLES* android_gles_init(void);

/* Start GPU emulation support. Returns 0 on success, -1/errno on failure. */
int android_gles_start(AndroidGLES* state, int width, int height);

/* Retrieve the Vendor/Renderer/Version strings describing the underlying GL
 * implementation. The call only works while the renderer is started.
 *
 * Each string is copied into the corresponding buffer. If the original
 * string (including NUL terminator) is more than xxBufSize bytes, it will
 * be truncated. In all cases, including failure, the buffer will be NUL-
 * terminated when this function returns.
 */
void android_gles_get_hardware_strings(char *vendor,
                                       size_t vendor_buffer_size,
                                       char *renderer,
                                       size_t renderer_buffer_size,
                                       char *version,
                                       size_t version_buffer_size);

/* Pointer type to a function used to retrieve the content of
 * GPU framebuffer content. This is used in the Android emulator
 * to support displaying the framebuffer content to a remote device
 * for multi-touch support.
 * |context| is a client-provided value passed to
 * android_gles_set_post_callback().
 */
typedef void (*AndroidGLESOnPostFunc)(void *context,
                                      int width,
                                      int height,
                                      int ydir,
                                      int format,
                                      int type,
                                      unsigned char *pixels);

/* Enable GPU framebuffer retrieval. If |on_post| is not NULL, it will
 * be called periodically when the framebuffer content changes. Note that
 * each call can be very expensive, depending on the host GPU.
 * Set |on_post| to NULL to disable the feature at runtime.
 */
void android_gles_set_post_callback(AndroidGLES *gles,
                                    AndroidGLESOnPostFunc on_post,
                                    void* on_post_context);

/* Show an OpenGL window on top of the current QEMU UI window, at a
 * specific location.
 * |gles| is the current AndroidGLES state.
 * |window| is a platform-specific identifier for the current UI window
 * (see note below).
 * |x|, |y|, |width| and |height| provide the location and size of the
 * OpenGL window, relative to the current one.
 * |rotation| is used to provide a rotation angle. Valid values are
 * 0, 90, 180 and 270.
 * Returns 0 on success, -1/errno on failure.
 *
 * NOTE: The exact type and meaning of |window| depends on the platform:
 * - On Windows, this is the HWND of the current UI window.
 * - On Linux, it's the X11 Window identifier (really a uint32_t cast
 *   to void*).
 * - On OS X, it's a NSWindow* value.
 */
int android_gles_show_window(AndroidGLES *gles,
                             void *window,
                             int x,
                             int y,
                             int width,
                             int height,
                             float rotation);

/* Hide the OpenGL window. */
void android_gles_hide_window(AndroidGLES *gles);

void android_gles_redraw_window(AndroidGLES *gles);

/* Stop GPU emulation support. */
void android_gles_stop(AndroidGLES *gles);

/* Write the local GPU server path into buffer |buff| of
 * |buff_size| bytes. Result is zero-terminated. */
void android_gles_server_path(char *buff, size_t buff_size);

#endif  /* _HW_MISC_ANDROID_OPENGLES_H */
