/*
 * QEMU SDL display driver
 *
 * Copyright (c) 2003 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/* Ported SDL 1.2 code to 2.0 by Dave Airlie. */

#include "qemu/osdep.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/sdl2.h"

static DisplayScaler sdl2_2d_scaler(struct sdl2_console *scon)
{
    if (scon->opts->has_scaler) {
        return scon->opts->scaler;
    }
    return DISPLAY_SCALER_LINEAR;
}

/*
 * Draw the guest texture into the window.
 *
 * SDL_RenderSetLogicalSize() already letterboxes the guest and scales it to
 * the window, so @linear and @nearest only differ in the filter set on the
 * texture. @integer and @integerplus need the destination rectangle worked
 * out by hand, because SDL would otherwise use the largest fractional scale
 * that fits.
 */
static void sdl2_2d_present(struct sdl2_console *scon)
{
    DisplayScaler scaler = sdl2_2d_scaler(scon);
    int fbw = surface_width(scon->surface);
    int fbh = surface_height(scon->surface);
    int ww, wh, mult, tw, th;
    SDL_Rect dst;

    /*
     * How far the window may fall short of a whole multiple and still be
     * treated as one. A fractional-scale compositor rounds to whole physical
     * pixels, which costs at most a pixel here.
     */
    const int SCALER_SNAP = 2;

    SDL_RenderClear(scon->real_renderer);

    if (scaler == DISPLAY_SCALER_LINEAR || scaler == DISPLAY_SCALER_NEAREST) {
        SDL_RenderCopy(scon->real_renderer, scon->texture, NULL, NULL);
        SDL_RenderPresent(scon->real_renderer);
        return;
    }

    /*
     * The logical size makes SDL letterbox for us, which is the opposite of
     * what is wanted here: the destination rectangle has to be computed in
     * real output pixels.
     */
    SDL_RenderSetLogicalSize(scon->real_renderer, 0, 0);
    if (SDL_GetRendererOutputSize(scon->real_renderer, &ww, &wh) != 0) {
        SDL_GetWindowSize(scon->real_window, &ww, &wh);
    }

    /*
     * A compositor running at a fractional scale rounds the window to whole
     * physical pixels, so a window sized to be exactly N times the guest can
     * come back a pixel short. Flooring there would drop a whole step - 2x
     * would become 1x and half the window would turn into border - so let
     * the multiple round up across that last pixel or two.
     */
    mult = MIN((ww + SCALER_SNAP) / fbw, (wh + SCALER_SNAP) / fbh);
    if (mult < 1) {
        /* Window smaller than the guest: fall back to fitting it. */
        mult = 1;
    }
    tw = fbw * mult;
    th = fbh * mult;

    if (scaler == DISPLAY_SCALER_INTEGER) {
        dst.w = tw;
        dst.h = th;
        dst.x = (ww - dst.w) / 2;
        dst.y = (wh - dst.h) / 2;
        SDL_RenderCopy(scon->real_renderer, scon->texture, NULL, &dst);
        SDL_RenderPresent(scon->real_renderer);
        return;
    }

    /*
     * integerplus: blow the guest up by @mult with no interpolation first,
     * then stretch that to the window with a bilinear filter. Scaling up
     * before filtering keeps the pixel edges of the guest mostly intact, so
     * the result stays sharper than a plain bilinear stretch while still
     * filling the window.
     */
    {
        int fit;

        /* Where the guest would land if stretched to fill the window. */
        if ((int64_t)ww * fbh < (int64_t)wh * fbw) {
            fit = ww;
            dst.w = fit;
            dst.h = fbh * fit / fbw;
        } else {
            fit = wh;
            dst.h = fit;
            dst.w = fbw * fit / fbh;
        }

        /*
         * If that is already the whole multiple, give or take the pixel the
         * compositor rounded away, the second pass would only resample by a
         * pixel - all blur, no gain. Draw the integer scale instead.
         */
        if (ABS(dst.w - tw) <= SCALER_SNAP && ABS(dst.h - th) <= SCALER_SNAP) {
            dst.w = tw;
            dst.h = th;
            dst.x = (ww - dst.w) / 2;
            dst.y = (wh - dst.h) / 2;
            SDL_RenderCopy(scon->real_renderer, scon->texture, NULL, &dst);
            SDL_RenderPresent(scon->real_renderer);
            return;
        }

        if (!scon->scaler_target ||
            scon->scaler_target_w != tw || scon->scaler_target_h != th) {
            if (scon->scaler_target) {
                SDL_DestroyTexture(scon->scaler_target);
            }
            scon->scaler_target =
                SDL_CreateTexture(scon->real_renderer,
                                  SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_TARGET, tw, th);
            scon->scaler_target_w = tw;
            scon->scaler_target_h = th;
        }

        if (scon->scaler_target) {
            SDL_SetTextureScaleMode(scon->scaler_target, SDL_ScaleModeLinear);
            SDL_SetRenderTarget(scon->real_renderer, scon->scaler_target);
            SDL_RenderClear(scon->real_renderer);
            SDL_RenderCopy(scon->real_renderer, scon->texture, NULL, NULL);
            SDL_SetRenderTarget(scon->real_renderer, NULL);
            SDL_RenderClear(scon->real_renderer);
        }

        dst.x = (ww - dst.w) / 2;
        dst.y = (wh - dst.h) / 2;

        SDL_RenderCopy(scon->real_renderer,
                       scon->scaler_target ? scon->scaler_target
                                           : scon->texture,
                       NULL, &dst);
    }

    SDL_RenderPresent(scon->real_renderer);
}

void sdl2_2d_update(DisplayChangeListener *dcl,
                    int x, int y, int w, int h)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    DisplaySurface *surf = scon->surface;
    SDL_Rect rect;
    size_t surface_data_offset;
    assert(!scon->opengl);

    if (!scon->texture) {
        return;
    }

    surface_data_offset = surface_bytes_per_pixel(surf) * x +
                          surface_stride(surf) * y;
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;

    SDL_UpdateTexture(scon->texture, &rect,
                      surface_data(surf) + surface_data_offset,
                      surface_stride(surf));
    sdl2_2d_present(scon);
}

void sdl2_2d_switch(DisplayChangeListener *dcl,
                    DisplaySurface *new_surface)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    DisplaySurface *old_surface = scon->surface;
    int format = 0;

    assert(!scon->opengl);

    scon->surface = new_surface;

    if (scon->texture) {
        SDL_DestroyTexture(scon->texture);
        scon->texture = NULL;
    }

    if (surface_is_placeholder(new_surface) && qemu_console_get_index(dcl->con)) {
        sdl2_window_destroy(scon);
        return;
    }

    if (!scon->real_window) {
        sdl2_window_create(scon);
    } else if (old_surface &&
               ((surface_width(old_surface)  != surface_width(new_surface)) ||
                (surface_height(old_surface) != surface_height(new_surface)))) {
        sdl2_window_resize(scon);
    }

    SDL_RenderSetLogicalSize(scon->real_renderer,
                             surface_width(new_surface),
                             surface_height(new_surface));

    switch (surface_format(scon->surface)) {
    case PIXMAN_x1r5g5b5:
        format = SDL_PIXELFORMAT_ARGB1555;
        break;
    case PIXMAN_r5g6b5:
        format = SDL_PIXELFORMAT_RGB565;
        break;
    case PIXMAN_a8r8g8b8:
    case PIXMAN_x8r8g8b8:
        format = SDL_PIXELFORMAT_ARGB8888;
        break;
    case PIXMAN_a8b8g8r8:
    case PIXMAN_x8b8g8r8:
        format = SDL_PIXELFORMAT_ABGR8888;
        break;
    case PIXMAN_r8g8b8a8:
    case PIXMAN_r8g8b8x8:
        format = SDL_PIXELFORMAT_RGBA8888;
        break;
    case PIXMAN_b8g8r8x8:
        format = SDL_PIXELFORMAT_BGRX8888;
        break;
    case PIXMAN_b8g8r8a8:
        format = SDL_PIXELFORMAT_BGRA8888;
        break;
    default:
        g_assert_not_reached();
    }
    scon->texture = SDL_CreateTexture(scon->real_renderer, format,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      surface_width(new_surface),
                                      surface_height(new_surface));
    /*
     * Set the filter on the texture itself rather than relying on
     * SDL_HINT_RENDER_SCALE_QUALITY, which the embedder may have set to
     * something else before SDL was initialised.
     */
    SDL_SetTextureScaleMode(scon->texture,
                            sdl2_2d_scaler(scon) == DISPLAY_SCALER_LINEAR
                            ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);

    /* The guest resolution changed, so any intermediate target is stale. */
    if (scon->scaler_target) {
        SDL_DestroyTexture(scon->scaler_target);
        scon->scaler_target = NULL;
        scon->scaler_target_w = scon->scaler_target_h = 0;
    }
    sdl2_2d_redraw(scon);
}

void sdl2_2d_refresh(DisplayChangeListener *dcl)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    assert(!scon->opengl);
    qemu_console_hw_update(dcl->con);
    sdl2_poll_events(scon);
}

void sdl2_2d_redraw(struct sdl2_console *scon)
{
    assert(!scon->opengl);

    if (!scon->surface) {
        return;
    }
    sdl2_2d_update(&scon->dcl, 0, 0,
                   surface_width(scon->surface),
                   surface_height(scon->surface));
}

bool sdl2_2d_check_format(DisplayChangeListener *dcl,
                          pixman_format_code_t format)
{
    /*
     * We let SDL convert for us a few more formats than,
     * the native ones. These are the ones I have tested.
     */
    return (format == PIXMAN_x8r8g8b8 ||
            format == PIXMAN_a8r8g8b8 ||
            format == PIXMAN_a8b8g8r8 ||
            format == PIXMAN_x8b8g8r8 ||
            format == PIXMAN_b8g8r8x8 ||
            format == PIXMAN_b8g8r8a8 ||
            format == PIXMAN_r8g8b8x8 ||
            format == PIXMAN_r8g8b8a8 ||
            format == PIXMAN_x1r5g5b5 ||
            format == PIXMAN_r5g6b5);
}
