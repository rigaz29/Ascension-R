/*
 * Ascension-R -- the title's camera at the screen's width (display.ultrawide)
 *
 * The title keeps a camera's horizontal half-width and divides it by the aspect at +32 of its render settings for the
 * vertical one, so a wider aspect alone would crop the picture top and bottom. Both change here: the aspect, and the
 * half-width by the same factor, which leaves the vertical view as it was and adds to the sides. The title's own frustum
 * comes from the same two numbers, so what it draws follows. The 2D screens are drawn through orthographic cameras made
 * the same way, so they keep their shape in the 16:9 in the middle. scripts/ppu_hooks.json has the lift call these.
 */
#include "ppu_recomp.h"
#include "rsx_d3d12_backend.h"
#include <stdint.h>
#include <string.h>

/* func_003A3DD8: r4 holds 16:9, about to go to +32 of the render settings. */
extern "C" void gowa_wide_aspect(ppu_context* ctx, uint32_t)
{
    const float r = rsx_d3d12_backend_wide_ratio();
    if (r == 1.0f) return;
    const float aspect = 16.0f / 9.0f * r;
    uint32_t bits;
    memcpy(&bits, &aspect, sizeof bits);
    ctx->gpr[4] = bits;
}

/* func_003B0380: f1 the tangent of a perspective camera's field of view, about to make its half-width at +960. func_003AFDC8
 * and func_003B0380 again: f1 an orthographic camera's half-width (func_0031446C), about to be kept in f31. */
extern "C" void gowa_wide_fov(ppu_context* ctx, uint32_t)
{
    const float r = rsx_d3d12_backend_wide_ratio();
    if (r != 1.0f) ctx->fpr[1] *= r;
}

/* func_00314780: f2 the aspect, about to turn the framing limits of the camera controller into angles. That is how the
 * camera follows Kratos, not what it shows, so it stays at the 16:9 the title was tuned for. */
extern "C" void gowa_wide_framing(ppu_context* ctx, uint32_t)
{
    if (rsx_d3d12_backend_wide_ratio() != 1.0f) ctx->fpr[2] = 16.0f / 9.0f;
}
