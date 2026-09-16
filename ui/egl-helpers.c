/*
 * Copyright (C) 2015-2016 Gerd Hoffmann <kraxel@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu/osdep.h"

#include "qemu/drm.h"
#include "qemu/error-report.h"
#include "ui/console.h"
#include "ui/egl-helpers.h"
#include "system/system.h"
#include "qapi/error.h"
#include "trace.h"
#include "standard-headers/drm/drm_fourcc.h"

EGLDisplay qemu_egl_display;
EGLConfig qemu_egl_config;
DisplayGLMode qemu_egl_mode;
bool qemu_egl_angle_d3d;

#ifndef EGL_PLATFORM_ANGLE_ANGLE
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE 0x320D
#endif

/* ------------------------------------------------------------------ */

const char *qemu_egl_get_error_string(void)
{
    EGLint error = eglGetError();

    switch (error) {
    case EGL_SUCCESS:
        return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED:
        return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS:
        return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC:
        return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE:
        return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONTEXT:
        return "EGL_BAD_CONTEXT";
    case EGL_BAD_CONFIG:
        return "EGL_BAD_CONFIG";
    case EGL_BAD_CURRENT_SURFACE:
        return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY:
        return "EGL_BAD_DISPLAY";
    case EGL_BAD_SURFACE:
        return "EGL_BAD_SURFACE";
    case EGL_BAD_MATCH:
        return "EGL_BAD_MATCH";
    case EGL_BAD_PARAMETER:
        return "EGL_BAD_PARAMETER";
    case EGL_BAD_NATIVE_PIXMAP:
        return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW:
        return "EGL_BAD_NATIVE_WINDOW";
    case EGL_CONTEXT_LOST:
        return "EGL_CONTEXT_LOST";
    default:
        return "Unknown EGL error";
    }
}

static void egl_fb_delete_texture(egl_fb *fb)
{
    if (!fb->delete_texture) {
        return;
    }

    glDeleteTextures(1, &fb->texture);
    fb->delete_texture = false;
}

void egl_fb_destroy(egl_fb *fb)
{
    if (!fb->framebuffer) {
        return;
    }

    egl_fb_delete_texture(fb);
    glDeleteFramebuffers(1, &fb->framebuffer);

    fb->width = 0;
    fb->height = 0;
    fb->x = 0;
    fb->y = 0;
    fb->texture = 0;
    fb->framebuffer = 0;
}

void egl_fb_setup_default(egl_fb *fb, int width, int height, int x, int y)
{
    fb->width = width;
    fb->height = height;
    fb->x = x;
    fb->y = y;
    fb->framebuffer = 0; /* default framebuffer */
}

void egl_fb_setup_for_tex(egl_fb *fb, int width, int height,
                          GLuint texture, bool delete)
{
    egl_fb_delete_texture(fb);

    fb->width = width;
    fb->height = height;
    fb->texture = texture;
    fb->delete_texture = delete;
    if (!fb->framebuffer) {
        glGenFramebuffers(1, &fb->framebuffer);
    }

    glBindFramebuffer(GL_FRAMEBUFFER_EXT, fb->framebuffer);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_2D, fb->texture, 0);
}

void egl_fb_setup_new_tex(egl_fb *fb, int width, int height)
{
    GLuint texture;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height,
                 0, GL_BGRA, GL_UNSIGNED_BYTE, 0);

    egl_fb_setup_for_tex(fb, width, height, texture, true);
}

void egl_fb_blit(egl_fb *dst, egl_fb *src, bool flip)
{
    GLuint x1 = 0;
    GLuint y1 = 0;
    GLuint x2, y2;
    GLuint w = src->width;
    GLuint h = src->height;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, src->framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst->framebuffer);
    glViewport(0, 0, dst->width, dst->height);
    glClear(GL_COLOR_BUFFER_BIT);

    if (src->dmabuf) {
        x1 = qemu_dmabuf_get_x(src->dmabuf);
        y1 = qemu_dmabuf_get_y(src->dmabuf);
        w = qemu_dmabuf_get_width(src->dmabuf);
        h = qemu_dmabuf_get_height(src->dmabuf);
    }

    w = (x1 + w) > src->width ? src->width - x1 : w;
    h = (y1 + h) > src->height ? src->height - y1 : h;

    y2 = flip ? y1 : h + y1;
    y1 = flip ? h + y1 : y1;
    x2 = x1 + w;

    glBlitFramebuffer(x1, y1, x2, y2,
                      dst->x, dst->y,
                      dst->x + dst->width, dst->y + dst->height,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

void egl_fb_read(DisplaySurface *dst, egl_fb *src)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src->framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    glReadPixels(0, 0, surface_width(dst), surface_height(dst),
                 GL_BGRA, GL_UNSIGNED_BYTE, surface_data(dst));
}

void egl_fb_read_rect(DisplaySurface *dst, egl_fb *src, int x, int y, int w, int h)
{
    assert(surface_width(dst) == src->width);
    assert(surface_height(dst) == src->height);
    assert(surface_format(dst) == PIXMAN_x8r8g8b8);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, src->framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    glPixelStorei(GL_PACK_ROW_LENGTH, surface_stride(dst) / 4);
    glReadPixels(x, y, w, h,
                 GL_BGRA, GL_UNSIGNED_BYTE, surface_data(dst) + x * 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
}

void egl_texture_blit(QemuGLShader *gls, egl_fb *dst, egl_fb *src, bool flip)
{
    glBindFramebuffer(GL_FRAMEBUFFER_EXT, dst->framebuffer);
    glViewport(0, 0, dst->width, dst->height);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, src->texture);
    qemu_gl_run_texture_blit(gls, flip);
}

void egl_texture_blend(QemuGLShader *gls, egl_fb *dst, egl_fb *src, bool flip,
                       int x, int y, double scale_x, double scale_y)
{
    glBindFramebuffer(GL_FRAMEBUFFER_EXT, dst->framebuffer);
    int w = scale_x * src->width;
    int h = scale_y * src->height;
    if (flip) {
        glViewport(x, y, w, h);
    } else {
        glViewport(x, dst->height - h - y, w, h);
    }
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, src->texture);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    qemu_gl_run_texture_blit(gls, flip);
    glDisable(GL_BLEND);
}

/* ---------------------------------------------------------------------- */

EGLContext qemu_egl_rn_ctx;

#ifdef CONFIG_GBM

int qemu_egl_rn_fd;
struct gbm_device *qemu_egl_rn_gbm_dev;

int egl_rendernode_init(const char *rendernode, DisplayGLMode mode)
{
    qemu_egl_rn_fd = -1;
    int rc;

    qemu_egl_rn_fd = qemu_drm_rendernode_open(rendernode);
    if (qemu_egl_rn_fd == -1) {
        error_report("egl: no drm render node available");
        goto err;
    }

    qemu_egl_rn_gbm_dev = gbm_create_device(qemu_egl_rn_fd);
    if (!qemu_egl_rn_gbm_dev) {
        error_report("egl: gbm_create_device failed");
        goto err;
    }

    rc = qemu_egl_init_dpy_mesa((EGLNativeDisplayType)qemu_egl_rn_gbm_dev,
                                mode);
    if (rc != 0) {
        /* qemu_egl_init_dpy_mesa reports error */
        goto err;
    }

    if (!epoxy_has_egl_extension(qemu_egl_display,
                                 "EGL_KHR_surfaceless_context")) {
        error_report("egl: EGL_KHR_surfaceless_context not supported");
        goto err;
    }
    if (!epoxy_has_egl_extension(qemu_egl_display,
                                 "EGL_MESA_image_dma_buf_export")) {
        error_report("egl: EGL_MESA_image_dma_buf_export not supported");
        goto err;
    }
    if (!epoxy_has_egl_extension(qemu_egl_display,
                                 "EGL_EXT_image_dma_buf_import_modifiers")) {
        error_report("egl: EGL_EXT_image_dma_buf_import_modifiers not supported");
        goto err;
    }

    qemu_egl_rn_ctx = qemu_egl_init_ctx();
    if (!qemu_egl_rn_ctx) {
        error_report("egl: egl_init_ctx failed");
        goto err;
    }

    return 0;

err:
    if (qemu_egl_rn_gbm_dev) {
        gbm_device_destroy(qemu_egl_rn_gbm_dev);
    }
    if (qemu_egl_rn_fd != -1) {
        close(qemu_egl_rn_fd);
    }

    return -1;
}

bool egl_dmabuf_export_texture(uint32_t tex_id, int *fd, EGLint *offset,
                               EGLint *stride, EGLint *fourcc, int *num_planes,
                               EGLuint64KHR *modifier)
{
    EGLImageKHR image;
    EGLuint64KHR modifiers[DMABUF_MAX_PLANES];
    int i;

    image = eglCreateImageKHR(qemu_egl_display, eglGetCurrentContext(),
                              EGL_GL_TEXTURE_2D_KHR,
                              (EGLClientBuffer)(unsigned long)tex_id,
                              NULL);
    if (!image) {
        return false;
    }

    eglExportDMABUFImageQueryMESA(qemu_egl_display, image, fourcc,
                                  num_planes, modifiers);
    eglExportDMABUFImageMESA(qemu_egl_display, image, fd, stride, offset);
    eglDestroyImageKHR(qemu_egl_display, image);

    /* Only first modifier matters. */
    if (modifier) {
        *modifier = modifiers[0];
    }

    for (i = 0; i < *num_planes; i++) {
        if (fd[i] < 0) {
            return false;
        }
    }
    return true;
}

void egl_dmabuf_import_texture(QemuDmaBuf *dmabuf)
{
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    EGLint attrs[64];
    int i = 0, j;
    uint64_t modifier = qemu_dmabuf_get_modifier(dmabuf);
    uint32_t texture = qemu_dmabuf_get_texture(dmabuf);
    int nfds, noffsets, nstrides;
    const int *fds = qemu_dmabuf_get_fds(dmabuf, &nfds);
    const uint32_t *offsets = qemu_dmabuf_get_offsets(dmabuf, &noffsets);
    const uint32_t *strides = qemu_dmabuf_get_strides(dmabuf, &nstrides);
    uint32_t num_planes = qemu_dmabuf_get_num_planes(dmabuf);

    EGLint fd_attrs[] = {
        EGL_DMA_BUF_PLANE0_FD_EXT,
        EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT,
        EGL_DMA_BUF_PLANE3_FD_EXT,
    };
    EGLint offset_attrs[] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT,
        EGL_DMA_BUF_PLANE3_OFFSET_EXT,
    };
    EGLint stride_attrs[] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT,
        EGL_DMA_BUF_PLANE3_PITCH_EXT,
    };
    EGLint modifier_lo_attrs[] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
    };
    EGLint modifier_hi_attrs[] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
    };

    if (texture != 0) {
        return;
    }

    assert(nfds >= num_planes);
    assert(noffsets >= num_planes);
    assert(nstrides >= num_planes);

    attrs[i++] = EGL_WIDTH;
    attrs[i++] = qemu_dmabuf_get_backing_width(dmabuf);
    attrs[i++] = EGL_HEIGHT;
    attrs[i++] = qemu_dmabuf_get_backing_height(dmabuf);
    attrs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
    attrs[i++] = qemu_dmabuf_get_fourcc(dmabuf);

    for (j = 0; j < num_planes; j++) {
        attrs[i++] = fd_attrs[j];
        /* fd[1-3] may be -1 if using a joint buffer for all planes */
        attrs[i++] = fds[j] >= 0 ? fds[j] : fds[0];
        attrs[i++] = stride_attrs[j];
        attrs[i++] = strides[j];
        attrs[i++] = offset_attrs[j];
        attrs[i++] = offsets[j];
        if (modifier != DRM_FORMAT_MOD_INVALID) {
            attrs[i++] = modifier_lo_attrs[j];
            attrs[i++] = (modifier >>  0) & 0xffffffff;
            attrs[i++] = modifier_hi_attrs[j];
            attrs[i++] = (modifier >> 32) & 0xffffffff;
        }
    }

    attrs[i++] = EGL_NONE;

    image = eglCreateImageKHR(qemu_egl_display,
                              EGL_NO_CONTEXT,
                              EGL_LINUX_DMA_BUF_EXT,
                              NULL, attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        error_report("eglCreateImageKHR failed");
        return;
    }

    glGenTextures(1, &texture);
    qemu_dmabuf_set_texture(dmabuf, texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)image);
    eglDestroyImageKHR(qemu_egl_display, image);
}

void egl_dmabuf_release_texture(QemuDmaBuf *dmabuf)
{
    uint32_t texture;

    texture = qemu_dmabuf_get_texture(dmabuf);
    if (texture == 0) {
        return;
    }

    glDeleteTextures(1, &texture);
    qemu_dmabuf_set_texture(dmabuf, 0);
}

EGLSyncKHR egl_create_sync(void)
{
    EGLSyncKHR sync;

    if (epoxy_has_egl_extension(qemu_egl_display,
                                "EGL_KHR_fence_sync") &&
        epoxy_has_egl_extension(qemu_egl_display,
                                "EGL_ANDROID_native_fence_sync")) {
        sync = eglCreateSyncKHR(qemu_egl_display,
                                EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
        if (sync != EGL_NO_SYNC_KHR) {
            return sync;
        }
    }

    return NULL;
}

int egl_create_fence(EGLSyncKHR sync)
{
    int fence_fd = -1;

    if (sync) {
        fence_fd = eglDupNativeFenceFDANDROID(qemu_egl_display,
                                              sync);
        eglDestroySyncKHR(qemu_egl_display, sync);
    }

    return fence_fd;
}

#endif /* CONFIG_GBM */

/* ---------------------------------------------------------------------- */

EGLSurface qemu_egl_init_surface(EGLContext ectx, EGLNativeWindowType win)
{
    EGLSurface esurface;
    EGLBoolean b;

    esurface = eglCreateWindowSurface(qemu_egl_display,
                                      qemu_egl_config,
                                      win, NULL);
    if (esurface == EGL_NO_SURFACE) {
        error_report("egl: eglCreateWindowSurface failed");
        return NULL;
    }

    b = eglMakeCurrent(qemu_egl_display, esurface, esurface, ectx);
    if (b == EGL_FALSE) {
        error_report("egl: eglMakeCurrent failed");
        return NULL;
    }

    return esurface;
}

/* ---------------------------------------------------------------------- */
/* ---------------------------------------------------------------------- */

EGLDisplay qemu_egl_get_display(EGLNativeDisplayType native,
                                EGLenum platform)
{
    EGLDisplay dpy = EGL_NO_DISPLAY;

    /* In practise any EGL 1.5 implementation would support the EXT extension */
    if (epoxy_has_egl_extension(NULL, "EGL_EXT_platform_base")) {
        if (platform != 0) {
            dpy = eglGetPlatformDisplayEXT(platform, (void *)(intptr_t)native, NULL);
        }
    }

    if (dpy == EGL_NO_DISPLAY) {
        /* fallback */
        dpy = eglGetDisplay(native);
    }
    return dpy;
}

static int qemu_egl_configure_display(DisplayGLMode mode);

static int qemu_egl_setup_display(EGLDisplay dpy, DisplayGLMode mode)
{
    if (dpy == EGL_NO_DISPLAY) {
        error_report("egl: eglGetDisplay failed: %s", qemu_egl_get_error_string());
        return -1;
    }

    qemu_egl_display = dpy;
    return qemu_egl_configure_display(mode);
}

static int qemu_egl_init_dpy(EGLNativeDisplayType dpy,
                             EGLenum platform,
                             DisplayGLMode mode)
{
    return qemu_egl_setup_display(qemu_egl_get_display(dpy, platform), mode);
}

static int qemu_egl_configure_display(DisplayGLMode mode)
{
    static const EGLint conf_att_core[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,   5,
        EGL_GREEN_SIZE, 5,
        EGL_BLUE_SIZE,  5,
        EGL_ALPHA_SIZE, 0,
        EGL_NONE,
    };
    static const EGLint conf_att_gles[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,   5,
        EGL_GREEN_SIZE, 5,
        EGL_BLUE_SIZE,  5,
        EGL_ALPHA_SIZE, 0,
        EGL_NONE,
    };
    EGLint major, minor;
    EGLBoolean b;
    EGLint n;
    bool gles = (mode == DISPLAY_GL_MODE_ES);

    b = eglInitialize(qemu_egl_display, &major, &minor);
    if (b == EGL_FALSE) {
        error_report("egl: eglInitialize failed: %s", qemu_egl_get_error_string());
        return -1;
    }

    b = eglBindAPI(gles ?  EGL_OPENGL_ES_API : EGL_OPENGL_API);
    if (b == EGL_FALSE) {
        error_report("egl: eglBindAPI failed (%s mode): %s",
                     gles ? "gles" : "core", qemu_egl_get_error_string());
        return -1;
    }

    b = eglChooseConfig(qemu_egl_display,
                        gles ? conf_att_gles : conf_att_core,
                        &qemu_egl_config, 1, &n);
    if (b == EGL_FALSE || n != 1) {
        error_report("egl: eglChooseConfig failed (%s mode): %s",
                     gles ? "gles" : "core", qemu_egl_get_error_string());
        return -1;
    }

    qemu_egl_mode = gles ? DISPLAY_GL_MODE_ES : DISPLAY_GL_MODE_CORE;
    return 0;
}

int qemu_egl_init_dpy_cocoa(DisplayGLMode mode)
{
    return qemu_egl_init_dpy(EGL_DEFAULT_DISPLAY, 0, mode);
}

#if defined(CONFIG_X11) || defined(CONFIG_GBM)
int qemu_egl_init_dpy_x11(EGLNativeDisplayType dpy, DisplayGLMode mode)
{
#ifdef EGL_KHR_platform_x11
    return qemu_egl_init_dpy(dpy, EGL_PLATFORM_X11_KHR, mode);
#else
    return qemu_egl_init_dpy(dpy, 0, mode);
#endif
}

int qemu_egl_init_dpy_mesa(EGLNativeDisplayType dpy, DisplayGLMode mode)
{
#ifdef EGL_MESA_platform_gbm
    return qemu_egl_init_dpy(dpy, EGL_PLATFORM_GBM_MESA, mode);
#else
    return qemu_egl_init_dpy(dpy, 0, mode);
#endif
}
#endif


#ifdef WIN32
int qemu_egl_init_dpy_win32(EGLNativeDisplayType dpy, DisplayGLMode mode)
{
    /* prefer GL ES, as that's what ANGLE supports */
    if (mode == DISPLAY_GL_MODE_ON) {
        mode = DISPLAY_GL_MODE_ES;
    }

    if (qemu_egl_init_dpy(dpy, 0, mode) < 0) {
        return -1;
    }

#ifdef EGL_D3D11_DEVICE_ANGLE
    if (epoxy_has_egl_extension(qemu_egl_display, "EGL_EXT_device_query")) {
        EGLDeviceEXT device;
        void *d3d11_device;

        if (!eglQueryDisplayAttribEXT(qemu_egl_display,
                                      EGL_DEVICE_EXT,
                                      (EGLAttrib *)&device)) {
            return 0;
        }

        if (!eglQueryDeviceAttribEXT(device,
                                     EGL_D3D11_DEVICE_ANGLE,
                                     (EGLAttrib *)&d3d11_device)) {
            return 0;
        }

        trace_egl_init_d3d11_device(device);
        qemu_egl_angle_d3d = device != NULL;
    }
#endif

    return 0;
}
#endif

bool qemu_egl_has_dmabuf(void)
{
    if (qemu_egl_display == EGL_NO_DISPLAY) {
        return false;
    }

    return epoxy_has_egl_extension(qemu_egl_display,
                                   "EGL_EXT_image_dma_buf_import");
}

EGLContext qemu_egl_init_ctx(void)
{
    static const EGLint ctx_att_core[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    static const EGLint ctx_att_gles[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    bool gles = (qemu_egl_mode == DISPLAY_GL_MODE_ES);
    EGLContext ectx;
    EGLBoolean b;

    ectx = eglCreateContext(qemu_egl_display, qemu_egl_config, EGL_NO_CONTEXT,
                            gles ? ctx_att_gles : ctx_att_core);
    if (ectx == EGL_NO_CONTEXT) {
        error_report("egl: eglCreateContext failed");
        return NULL;
    }

    b = eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, ectx);
    if (b == EGL_FALSE) {
        error_report("egl: eglMakeCurrent failed");
        return NULL;
    }

    return ectx;
}

bool egl_init(const char *rendernode, DisplayGLMode mode, Error **errp)
{
    ERRP_GUARD();

    if (mode == DISPLAY_GL_MODE_OFF) {
        error_setg(errp, "egl: turning off GL doesn't make sense");
        return false;
    }

#ifdef WIN32
    if (qemu_egl_init_dpy_win32(EGL_DEFAULT_DISPLAY, mode) < 0) {
        error_setg(errp, "egl: init failed");
        return false;
    }
    qemu_egl_rn_ctx = qemu_egl_init_ctx();
    if (!qemu_egl_rn_ctx) {
        error_setg(errp, "egl: egl_init_ctx failed");
        return false;
    }
#elif defined(CONFIG_GBM)
    if (egl_rendernode_init(rendernode, mode) < 0) {
        error_setg(errp, "egl: render node init failed");
        return false;
    }
#elif defined(__APPLE__)
    /* ANGLE has no core profile config on Apple, GL ES is what it renders */
    if (mode == DISPLAY_GL_MODE_ON) {
        mode = DISPLAY_GL_MODE_ES;
    }
    if (qemu_egl_init_dpy_cocoa(mode) < 0) {
        error_setg(errp, "egl: init failed");
        return false;
    }
    qemu_egl_rn_ctx = qemu_egl_init_ctx();
    if (!qemu_egl_rn_ctx) {
        error_setg(errp, "egl: egl_init_ctx failed");
        return false;
    }
    qemu_egl_angle_dispatch();
#endif

    if (!qemu_egl_rn_ctx) {
        error_setg(errp, "egl: not available on this platform");
        return false;
    }

    display_opengl = 1;
    return true;
}

void egl_cleanup(void)
{
    if (qemu_egl_display) {
        eglReleaseThread();
    }

    if (qemu_egl_rn_ctx) {
        eglDestroyContext(qemu_egl_display, qemu_egl_rn_ctx);
        qemu_egl_rn_ctx = NULL;
    }

    if (qemu_egl_display) {
        eglTerminate(qemu_egl_display);
        qemu_egl_display = NULL;
    }

#ifdef CONFIG_GBM
    g_clear_pointer(&qemu_egl_rn_gbm_dev, gbm_device_destroy);
    g_clear_fd(&qemu_egl_rn_fd, NULL);
#endif
}

#ifdef __APPLE__
/*
 * libepoxy resolves GL through the platform's own GL library, which on macOS is not
 * the one that owns the context: ANGLE's.  Calls would then run against a library with
 * no current context of its own.  Take the entry points the display path uses from the
 * EGL display instead, so they and the context belong to the same implementation.
 */
#define QEMU_ANGLE_GL(name)                                                  \
    do {                                                                     \
        void *proc = (void *)eglGetProcAddress(#name);                        \
        if (proc) {                                                           \
            epoxy_##name = (__typeof__(epoxy_##name))proc;                    \
        }                                                                     \
    } while (0)

void qemu_egl_angle_dispatch(void)
{
    QEMU_ANGLE_GL(glAttachShader);
    QEMU_ANGLE_GL(glBindBuffer);
    QEMU_ANGLE_GL(glBindFramebuffer);
    QEMU_ANGLE_GL(glBindTexture);
    QEMU_ANGLE_GL(glBindVertexArray);
    QEMU_ANGLE_GL(glBlendFunc);
    QEMU_ANGLE_GL(glBlitFramebuffer);
    QEMU_ANGLE_GL(glBufferData);
    QEMU_ANGLE_GL(glCheckFramebufferStatus);
    QEMU_ANGLE_GL(glClear);
    QEMU_ANGLE_GL(glClearColor);
    QEMU_ANGLE_GL(glCompileShader);
    QEMU_ANGLE_GL(glCreateProgram);
    QEMU_ANGLE_GL(glCreateShader);
    QEMU_ANGLE_GL(glDeleteFramebuffers);
    QEMU_ANGLE_GL(glDeleteProgram);
    QEMU_ANGLE_GL(glDeleteShader);
    QEMU_ANGLE_GL(glDeleteTextures);
    QEMU_ANGLE_GL(glDisable);
    QEMU_ANGLE_GL(glDrawArrays);
    QEMU_ANGLE_GL(glEGLImageTargetTexture2DOES);
    QEMU_ANGLE_GL(glEnable);
    QEMU_ANGLE_GL(glEnableVertexAttribArray);
    QEMU_ANGLE_GL(glFinish);
    QEMU_ANGLE_GL(glFlush);
    QEMU_ANGLE_GL(glFramebufferTexture2D);
    QEMU_ANGLE_GL(glFramebufferTexture2DEXT);
    QEMU_ANGLE_GL(glGenBuffers);
    QEMU_ANGLE_GL(glGenFramebuffers);
    QEMU_ANGLE_GL(glGenTextures);
    QEMU_ANGLE_GL(glGenVertexArrays);
    QEMU_ANGLE_GL(glGetAttribLocation);
    QEMU_ANGLE_GL(glGetBooleanv);
    QEMU_ANGLE_GL(glGetError);
    QEMU_ANGLE_GL(glGetFloatv);
    QEMU_ANGLE_GL(glGetIntegerv);
    QEMU_ANGLE_GL(glGetString);
    QEMU_ANGLE_GL(glGetStringi);
    QEMU_ANGLE_GL(glGetTexLevelParameteriv);
    QEMU_ANGLE_GL(glGetProgramInfoLog);
    QEMU_ANGLE_GL(glGetProgramiv);
    QEMU_ANGLE_GL(glGetShaderInfoLog);
    QEMU_ANGLE_GL(glGetShaderiv);
    QEMU_ANGLE_GL(glLinkProgram);
    QEMU_ANGLE_GL(glMapBufferRange);
    QEMU_ANGLE_GL(glPixelStorei);
    QEMU_ANGLE_GL(glReadBuffer);
    QEMU_ANGLE_GL(glReadPixels);
    QEMU_ANGLE_GL(glShaderSource);
    QEMU_ANGLE_GL(glTexImage2D);
    QEMU_ANGLE_GL(glTexParameteri);
    QEMU_ANGLE_GL(glTexSubImage2D);
    QEMU_ANGLE_GL(glUseProgram);
    QEMU_ANGLE_GL(glVertexAttribPointer);
    QEMU_ANGLE_GL(glViewport);
}
#endif
