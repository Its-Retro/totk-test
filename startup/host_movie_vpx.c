/* VP9 decode shim for host_movie.h: the weak tm_vp9_* API over libvpx v1.15.2 (BSD, startup/third_party/libvpx).
 * Built by startup/third_party/build_vpx.py into vpx-build/libtotk_movie_vpx.a (linked whole-archive; the
 * runtime references these functions weakly, so without the archive every movie uses the stub). Plain C:
 * no libvpx type crosses into the runtime. */
#define TOTK_HOST_MOVIE_SHIM 1
#include "host_movie.h"
#include <stdlib.h>
#include <string.h>
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"

typedef struct TmVp9 { vpx_codec_ctx_t ctx; vpx_codec_iter_t iter; } TmVp9;

void *tm_vp9_open(int32_t threads, int32_t *error) {
    TmVp9 *d = (TmVp9 *)calloc(1, sizeof *d);
    if (!d) { if (error) *error = -1; return NULL; }
    vpx_codec_dec_cfg_t cfg; memset(&cfg, 0, sizeof cfg);
    cfg.threads = threads > 0 ? (unsigned)threads : 1;
    vpx_codec_err_t e = vpx_codec_dec_init(&d->ctx, vpx_codec_vp9_dx(), &cfg, 0);
    if (e != VPX_CODEC_OK) { if (error) *error = (int32_t)e; free(d); return NULL; }
    if (cfg.threads > 1) vpx_codec_control(&d->ctx, VP9D_SET_ROW_MT, 1);   /* row-based MT: helps single-tile streams */
    if (error) *error = 0;
    return d;
}
int32_t tm_vp9_decode(void *p, const uint8_t *data, uint32_t size) {
    TmVp9 *d = (TmVp9 *)p;
    d->iter = NULL;
    return (int32_t)vpx_codec_decode(&d->ctx, size ? data : NULL, size, NULL, 0);
}
int32_t tm_vp9_frame(void *p, TmImage *img) {
    TmVp9 *d = (TmVp9 *)p;
    vpx_image_t *im = vpx_codec_get_frame(&d->ctx, &d->iter);
    if (!im) return 0;
    for (int i = 0; i < 3; ++i) { img->plane[i] = im->planes[i]; img->stride[i] = im->stride[i]; }
    img->width = (int32_t)im->d_w; img->height = (int32_t)im->d_h;
    img->fmt = im->fmt == VPX_IMG_FMT_I420 ? 0 : (int32_t)im->fmt;   /* the runtime converts I420 only */
    return 1;
}
void tm_vp9_close(void *p) {
    TmVp9 *d = (TmVp9 *)p;
    if (!d) return;
    vpx_codec_destroy(&d->ctx);
    free(d);
}
const char *tm_vp9_version(void) { return vpx_codec_version_str(); }
