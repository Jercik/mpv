/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <dlfcn.h>
#include <math.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/data_space.h>
#include <libplacebo/colorspace.h>

#include "video/out/android_common.h"
#include "egl_helpers.h"
#include "common/common.h"
#include "context.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "utils.h"
#include "video/csputils.h"
#include "video/mp_image.h"
#include "video/out/gpu/video.h"

struct color_request {
    bool has_color; // false if nothing asks for a color space
    enum pl_color_primaries primaries;
    enum pl_color_transfer transfer;
    enum pl_color_levels levels;
    // The static metadata of an HDR10 signal; luminances in cd/m².
    struct pl_raw_primaries mastering_primaries;
    float max_luma, min_luma, max_cll, max_fall;
};

struct priv {
    struct GL gl;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;

    void *nativewindow_lib;
    // ANativeWindow_setBuffersDataSpace, which exists from Android 9 on.
    int32_t (*set_buffers_data_space)(ANativeWindow *window, int32_t data_space);

    struct m_config_cache *video_opts_cache; // struct gl_video_opts
    // Whether the renderer's last set_color call named a color space.
    bool hinted;
    bool color_requested;
    struct color_request color_request;
    bool hdr10;
};

static void android_swap_buffers(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    eglSwapBuffers(p->egl_display, p->egl_surface);
}

static struct color_request color_request_from_csp(const struct pl_color_space *csp,
                                                   enum pl_color_levels levels)
{
    struct color_request req = {
        .has_color = true,
        .primaries = csp->primaries,
        .transfer = csp->transfer,
        .levels = levels,
        .mastering_primaries = csp->hdr.prim,
        .max_luma = csp->hdr.max_luma,
        .min_luma = csp->hdr.min_luma,
        .max_cll = csp->hdr.max_cll,
        .max_fall = csp->hdr.max_fall,
    };
    pl_raw_primaries_merge(&req.mastering_primaries,
                           pl_raw_primaries_get(csp->primaries));
    return req;
}

static struct color_request color_request_from_options(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    m_config_cache_update(p->video_opts_cache);
    const struct gl_video_opts *opts = p->video_opts_cache->opts;
    if (opts->target_prim != PL_COLOR_PRIM_BT_2020 ||
        opts->target_trc != PL_COLOR_TRC_PQ)
        return (struct color_request){0};

    struct pl_color_space csp = {
        .primaries = opts->target_prim,
        .transfer = opts->target_trc,
        .hdr = {
            .max_luma = opts->target_peak,
            .max_cll = opts->target_peak,
        },
    };
    // Both renderers write full range RGB.
    return color_request_from_csp(&csp, PL_COLOR_LEVELS_FULL);
}

static bool color_request_equal(const struct color_request *a,
                                const struct color_request *b)
{
    return a->has_color == b->has_color &&
           a->primaries == b->primaries &&
           a->transfer == b->transfer &&
           a->levels == b->levels &&
           pl_raw_primaries_equal(&a->mastering_primaries,
                                  &b->mastering_primaries) &&
           a->max_luma == b->max_luma &&
           a->min_luma == b->min_luma &&
           a->max_cll == b->max_cll &&
           a->max_fall == b->max_fall;
}

static bool set_data_space(struct ra_ctx *ctx, int32_t data_space,
                           const char *name)
{
    struct priv *p = ctx->priv;
    ANativeWindow *window = vo_android_native_window(ctx->vo);
    int32_t ret = p->set_buffers_data_space(window, data_space);
    MP_VERBOSE(ctx, "HDR10: ANativeWindow_setBuffersDataSpace(%s = 0x%x) "
               "returned %d\n", name, (unsigned)data_space, (int)ret);
    return ret == 0;
}

static bool set_hdr10(struct ra_ctx *ctx, const struct color_request *req)
{
    struct priv *p = ctx->priv;

    if (!set_data_space(ctx, ADATASPACE_BT2020_PQ, "ADATASPACE_BT2020_PQ"))
        return false;

#define HDR10_ATTRIB(id, value) {id, # id, value}
    // SMPTE ST 2086, then CTA-861.3.
    const struct pl_raw_primaries *prim = &req->mastering_primaries;
    const struct {
        EGLint attrib;
        const char *name;
        float value;
    } attribs[] = {
        HDR10_ATTRIB(EGL_SMPTE2086_DISPLAY_PRIMARY_RX_EXT, prim->red.x),
        HDR10_ATTRIB(EGL_SMPTE2086_DISPLAY_PRIMARY_RY_EXT, prim->red.y),
        HDR10_ATTRIB(EGL_SMPTE2086_DISPLAY_PRIMARY_GX_EXT, prim->green.x),
        HDR10_ATTRIB(EGL_SMPTE2086_DISPLAY_PRIMARY_GY_EXT, prim->green.y),
        HDR10_ATTRIB(EGL_SMPTE2086_DISPLAY_PRIMARY_BX_EXT, prim->blue.x),
        HDR10_ATTRIB(EGL_SMPTE2086_DISPLAY_PRIMARY_BY_EXT, prim->blue.y),
        HDR10_ATTRIB(EGL_SMPTE2086_WHITE_POINT_X_EXT, prim->white.x),
        HDR10_ATTRIB(EGL_SMPTE2086_WHITE_POINT_Y_EXT, prim->white.y),
        HDR10_ATTRIB(EGL_SMPTE2086_MAX_LUMINANCE_EXT, req->max_luma),
        HDR10_ATTRIB(EGL_SMPTE2086_MIN_LUMINANCE_EXT, req->min_luma),
        HDR10_ATTRIB(EGL_CTA861_3_MAX_CONTENT_LIGHT_LEVEL_EXT, req->max_cll),
        HDR10_ATTRIB(EGL_CTA861_3_MAX_FRAME_AVERAGE_LEVEL_EXT, req->max_fall),
    };
#undef HDR10_ATTRIB

    for (int n = 0; n < MP_ARRAY_SIZE(attribs); n++) {
        EGLint value = lrintf(attribs[n].value * EGL_METADATA_SCALING_EXT);
        if (eglSurfaceAttrib(p->egl_display, p->egl_surface,
                             attribs[n].attrib, value))
        {
            MP_VERBOSE(ctx, "HDR10: eglSurfaceAttrib(%s, %d) succeeded\n",
                       attribs[n].name, (int)value);
        } else {
            MP_VERBOSE(ctx, "HDR10: eglSurfaceAttrib(%s, %d) failed "
                       "(EGL error 0x%x)\n", attribs[n].name, (int)value,
                       (unsigned)eglGetError());
            return false;
        }
    }
    return true;
}

static bool present_as(struct ra_ctx *ctx, const struct color_request *req,
                       const char *source)
{
    struct priv *p = ctx->priv;

    if (p->color_requested && color_request_equal(req, &p->color_request))
        return p->hdr10;
    p->color_requested = true;
    p->color_request = *req;

    if (!req->has_color) {
        MP_VERBOSE(ctx, "HDR10: no output color space requested\n");
    } else {
        MP_VERBOSE(ctx, "HDR10: %s requested output color space %s/%s/%s, "
                   "luminance %g-%g nits, MaxCLL %g, MaxFALL %g\n", source,
                   m_opt_choice_str(pl_csp_prim_names, req->primaries),
                   m_opt_choice_str(pl_csp_trc_names, req->transfer),
                   m_opt_choice_str(pl_csp_levels_names, req->levels),
                   req->min_luma, req->max_luma, req->max_cll, req->max_fall);
    }

    // ADATASPACE_BT2020_PQ is full range.
    bool want_hdr10 = req->has_color &&
                      req->primaries == PL_COLOR_PRIM_BT_2020 &&
                      req->transfer == PL_COLOR_TRC_PQ &&
                      req->levels == PL_COLOR_LEVELS_FULL;
    bool was_hdr10 = p->hdr10;
    p->hdr10 = want_hdr10 && set_hdr10(ctx, req);

    // The data space eglCreateWindowSurface gives a window that asks for no
    // color space.
    if (!p->hdr10 && (was_hdr10 || want_hdr10))
        set_data_space(ctx, ADATASPACE_UNKNOWN, "ADATASPACE_UNKNOWN");

    if (p->hdr10) {
        MP_VERBOSE(ctx, "HDR10: presenting as BT.2020 PQ\n");
    } else if (want_hdr10) {
        MP_WARN(ctx, "HDR10: could not present as BT.2020 PQ, "
                "presenting as SDR\n");
    } else if (was_hdr10) {
        MP_VERBOSE(ctx, "HDR10: presenting as SDR again\n");
    }
    return p->hdr10;
}

// vo_gpu_next calls this before every frame, with no params unless
// --target-colorspace-hint is in effect.
static bool android_set_color(struct ra_ctx *ctx, struct mp_image_params *params)
{
    struct priv *p = ctx->priv;

    p->hinted = params;
    if (!params) {
        struct color_request req = color_request_from_options(ctx);
        return present_as(ctx, &req, "--target-prim and --target-trc");
    }
    struct color_request req =
        color_request_from_csp(&params->color, params->repr.levels);
    return present_as(ctx, &req, "the renderer");
}

static void android_uninit(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    ra_gl_ctx_uninit(ctx);

    if (p->egl_surface) {
        eglMakeCurrent(p->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        eglDestroySurface(p->egl_display, p->egl_surface);
    }
    if (p->egl_context)
        eglDestroyContext(p->egl_display, p->egl_context);

    if (p->nativewindow_lib)
        dlclose(p->nativewindow_lib);

    vo_android_uninit(ctx->vo);
}

static bool android_init(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv = talloc_zero(ctx, struct priv);

    if (!vo_android_init(ctx->vo))
        goto fail;

    p->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(p->egl_display, NULL, NULL)) {
        MP_FATAL(ctx, "EGL failed to initialize.\n");
        goto fail;
    }

    EGLConfig config;
    if (!mpegl_create_context(ctx, p->egl_display, &p->egl_context, &config))
        goto fail;

    ANativeWindow *native_window = vo_android_native_window(ctx->vo);
    EGLint format;
    eglGetConfigAttrib(p->egl_display, config, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(native_window, 0, 0, format);

    p->egl_surface = eglCreateWindowSurface(p->egl_display, config,
                                    (EGLNativeWindowType)native_window, NULL);

    if (p->egl_surface == EGL_NO_SURFACE) {
        MP_FATAL(ctx, "Could not create EGL surface!\n");
        goto fail;
    }

    if (!eglMakeCurrent(p->egl_display, p->egl_surface, p->egl_surface,
                        p->egl_context)) {
        MP_FATAL(ctx, "Failed to set context!\n");
        goto fail;
    }

    mpegl_load_functions(&p->gl, ctx->log);

    p->video_opts_cache = m_config_cache_alloc(ctx, ctx->global, &gl_video_conf);

    // libmpv is built for Android versions that lack the function.
    p->nativewindow_lib = dlopen("libnativewindow.so", RTLD_NOW);
    if (p->nativewindow_lib) {
        p->set_buffers_data_space = dlsym(p->nativewindow_lib,
                                          "ANativeWindow_setBuffersDataSpace");
    }

    EGLint r_size = 0, g_size = 0, b_size = 0, a_size = 0;
    eglGetConfigAttrib(p->egl_display, config, EGL_RED_SIZE, &r_size);
    eglGetConfigAttrib(p->egl_display, config, EGL_GREEN_SIZE, &g_size);
    eglGetConfigAttrib(p->egl_display, config, EGL_BLUE_SIZE, &b_size);
    eglGetConfigAttrib(p->egl_display, config, EGL_ALPHA_SIZE, &a_size);
    const char *exts = eglQueryString(p->egl_display, EGL_EXTENSIONS);
    MP_VERBOSE(ctx, "HDR10: surface is R%dG%dB%dA%d, "
               "ANativeWindow_setBuffersDataSpace %s, "
               "EGL_EXT_gl_colorspace_bt2020_pq %s, "
               "EGL_EXT_surface_SMPTE2086_metadata %s, "
               "EGL_EXT_surface_CTA861_3_metadata %s\n",
               (int)r_size, (int)g_size, (int)b_size, (int)a_size,
               p->set_buffers_data_space ? "found" : "missing",
               gl_check_extension(exts, "EGL_EXT_gl_colorspace_bt2020_pq")
                   ? "advertised" : "not advertised",
               gl_check_extension(exts, "EGL_EXT_surface_SMPTE2086_metadata")
                   ? "advertised" : "not advertised",
               gl_check_extension(exts, "EGL_EXT_surface_CTA861_3_metadata")
                   ? "advertised" : "not advertised");

    struct ra_ctx_params params = {
        .swap_buffers = android_swap_buffers,
        .set_color = p->set_buffers_data_space ? android_set_color : NULL,
    };

    if (!ra_gl_ctx_init(ctx, &p->gl, params))
        goto fail;

    return true;
fail:
    android_uninit(ctx);
    return false;
}

static bool android_reconfig(struct ra_ctx *ctx)
{
    int w, h;
    if (!vo_android_surface_size(ctx->vo, &w, &h))
        return false;

    // Update window geometry to prevent screen tearing
    ANativeWindow *native_window = vo_android_native_window(ctx->vo);
    if (native_window) {
        int32_t current_format = ANativeWindow_getFormat(native_window);
        ANativeWindow_setBuffersGeometry(native_window, w, h, current_format);
    }

    ctx->vo->dwidth = w;
    ctx->vo->dheight = h;
    ra_gl_ctx_resize(ctx->swapchain, w, h, 0);

    // vo_gpu never calls set_color, so the options are all that tells what
    // it writes.
    struct priv *p = ctx->priv;
    if (p->set_buffers_data_space && !p->hinted) {
        struct color_request req = color_request_from_options(ctx);
        present_as(ctx, &req, "--target-prim and --target-trc");
    }
    return true;
}

static int android_control(struct ra_ctx *ctx, int *events, int request, void *arg)
{
    return VO_NOTIMPL;
}

const struct ra_ctx_fns ra_ctx_android = {
    .type           = "opengl",
    .name           = "android",
    .description    = "Android/EGL",
    .reconfig       = android_reconfig,
    .control        = android_control,
    .init           = android_init,
    .uninit         = android_uninit,
};
