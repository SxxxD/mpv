/*
 * This file is part of mpv.
 *
 * Filter graph creation code taken from Libav avplay.c (LGPL 2.1 or later)
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <stdarg.h>
#include <assert.h>

#include <libavutil/avstring.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include "config.h"
#include "common/av_common.h"
#include "common/msg.h"
#include "options/m_option.h"
#include "common/tags.h"

#include "video/hwdec.h"
#include "video/img_format.h"
#include "video/mp_image.h"
#include "video/sws_utils.h"
#include "video/fmt-conversion.h"
#include "vf.h"
#include "vf_lavfi.h"

// FFmpeg and Libav have slightly different APIs, just enough to cause us
// unnecessary pain. <Expletive deleted.>
#if LIBAVFILTER_VERSION_MICRO < 100
#define graph_parse(graph, filters, inputs, outputs, log_ctx) \
    avfilter_graph_parse(graph, filters, inputs, outputs, log_ctx)
#define avfilter_graph_send_command(a, b, c, d, e, f, g) -1
#else
#define graph_parse(graph, filters, inputs, outputs, log_ctx) \
    avfilter_graph_parse_ptr(graph, filters, &(inputs), &(outputs), log_ctx)
#endif

struct vf_priv_s {
    AVFilterGraph *graph;
    AVFilterContext *in;
    AVFilterContext *out;
    bool eof;

    AVRational timebase_in;
    AVRational timebase_out;
    AVRational par_in;

    struct mp_tags *metadata;

    // for the lw wrapper
    void *old_priv;
    int (*lw_reconfig_cb)(struct vf_instance *vf,
                          struct mp_image_params *in,
                          struct mp_image_params *out);

    // options
    char *cfg_graph;
    int64_t cfg_sws_flags;
    char **cfg_avopts;
};

static const struct vf_priv_s vf_priv_dflt = {
    .cfg_sws_flags = SWS_BICUBIC,
};

static void destroy_graph(struct vf_instance *vf)
{
    struct vf_priv_s *p = vf->priv;
    avfilter_graph_free(&p->graph);
    p->in = p->out = NULL;

    if (p->metadata) {
        talloc_free(p->metadata);
        p->metadata = NULL;
    }

    p->eof = false;
}

static bool recreate_graph(struct vf_instance *vf, struct mp_image_params *fmt)
{
    void *tmp = talloc_new(NULL);
    struct vf_priv_s *p = vf->priv;
    AVFilterContext *in = NULL, *out = NULL;
    int ret;

    if (bstr0(p->cfg_graph).len == 0) {
        MP_FATAL(vf, "lavfi: no filter graph set\n");
        return false;
    }

    destroy_graph(vf);
    MP_VERBOSE(vf, "lavfi: create graph: '%s'\n", p->cfg_graph);

    AVFilterGraph *graph = avfilter_graph_alloc();
    if (!graph)
        goto error;

    if (mp_set_avopts(vf->log, graph, p->cfg_avopts) < 0)
        goto error;

    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    if (!outputs || !inputs)
        goto error;

    char *sws_flags = talloc_asprintf(tmp, "flags=%"PRId64, p->cfg_sws_flags);
    graph->scale_sws_opts = av_strdup(sws_flags);

    in = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"), "src");
    if (!in)
        goto error;

    AVBufferSrcParameters *in_params = av_buffersrc_parameters_alloc();
    if (!in_params)
        goto error;

    in_params->format = imgfmt2pixfmt(fmt->imgfmt);
    in_params->time_base = AV_TIME_BASE_Q;
    in_params->width = fmt->w;
    in_params->height = fmt->h;
    in_params->sample_aspect_ratio.num = fmt->p_w;
    in_params->sample_aspect_ratio.den = fmt->p_h;
    // Assume it's ignored for non-hwaccel formats.
    in_params->hw_frames_ctx = vf->in_hwframes_ref;

    ret = av_buffersrc_parameters_set(in, in_params);
    av_free(in_params);
    if (ret < 0)
        goto error;

    if (avfilter_init_str(in, NULL) < 0)
        goto error;

    if (avfilter_graph_create_filter(&out, avfilter_get_by_name("buffersink"),
                                     "out", NULL, NULL, graph) < 0)
        goto error;

    outputs->name    = av_strdup("in");
    outputs->filter_ctx = in;

    inputs->name    = av_strdup("out");
    inputs->filter_ctx = out;

    if (graph_parse(graph, p->cfg_graph, inputs, outputs, NULL) < 0)
        goto error;

    if (vf->hwdec_devs) {
        struct mp_hwdec_ctx *hwdec = hwdec_devices_get_first(vf->hwdec_devs);
        for (int n = 0; n < graph->nb_filters; n++) {
            AVFilterContext *filter = graph->filters[n];
            if (hwdec && hwdec->av_device_ref)
                filter->hw_device_ctx = av_buffer_ref(hwdec->av_device_ref);
        }
    }

    if (avfilter_graph_config(graph, NULL) < 0)
        goto error;

    p->in = in;
    p->out = out;
    p->graph = graph;

    assert(out->nb_inputs == 1);
    assert(in->nb_outputs == 1);

    talloc_free(tmp);
    return true;

error:
    MP_FATAL(vf, "Can't configure libavfilter graph.\n");
    avfilter_graph_free(&graph);
    talloc_free(tmp);
    return false;
}

static void reset(vf_instance_t *vf)
{
    struct vf_priv_s *p = vf->priv;
    struct mp_image_params *f = &vf->fmt_in;
    if (p->graph && f->imgfmt)
        recreate_graph(vf, f);
}

static int reconfig(struct vf_instance *vf, struct mp_image_params *in,
                    struct mp_image_params *out)
{
    struct vf_priv_s *p = vf->priv;

    *out = *in; // pass-through untouched flags

    if (vf->priv->lw_reconfig_cb) {
        if (vf->priv->lw_reconfig_cb(vf, in, out) < 0)
            return -1;
    }

    if (!recreate_graph(vf, in))
        return -1;

    AVFilterLink *l_out = p->out->inputs[0];
    AVFilterLink *l_in = p->in->outputs[0];

    p->timebase_in = l_in->time_base;
    p->timebase_out = l_out->time_base;

    p->par_in = l_in->sample_aspect_ratio;

    out->w = l_out->w;
    out->h = l_out->h;
    out->p_w = l_out->sample_aspect_ratio.num;
    out->p_h = l_out->sample_aspect_ratio.den;
    out->imgfmt = pixfmt2imgfmt(l_out->format);
    av_buffer_unref(&vf->out_hwframes_ref);
#if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(6, 69, 100) && \
    LIBAVFILTER_VERSION_MICRO >= 100
    AVBufferRef *hw_frames_ctx = av_buffersink_get_hw_frames_ctx(p->out);
#else
    AVBufferRef *hw_frames_ctx = l_out->hw_frames_ctx;
#endif
    if (hw_frames_ctx) {
        AVHWFramesContext *fctx = (void *)hw_frames_ctx->data;
        out->hw_subfmt = pixfmt2imgfmt(fctx->sw_format);
        vf->out_hwframes_ref = av_buffer_ref(hw_frames_ctx);
    }
    return 0;
}

static int query_format(struct vf_instance *vf, unsigned int fmt)
{
    // Format negotiation is not possible with libavfilter.
    return 1;
}

static AVFrame *mp_to_av(struct vf_instance *vf, struct mp_image *img)
{
    struct vf_priv_s *p = vf->priv;
    if (!img)
        return NULL;
    uint64_t pts = img->pts == MP_NOPTS_VALUE ?
                   AV_NOPTS_VALUE : img->pts * av_q2d(av_inv_q(p->timebase_in));
    AVFrame *frame = mp_image_to_av_frame_and_unref(img);
    if (!frame)
        return NULL; // OOM is (coincidentally) handled as EOF
    frame->pts = pts;
    frame->sample_aspect_ratio = p->par_in;
    return frame;
}

static struct mp_image *av_to_mp(struct vf_instance *vf, AVFrame *av_frame)
{
    struct vf_priv_s *p = vf->priv;
    struct mp_image *img = mp_image_from_av_frame(av_frame);
    if (!img)
        return NULL; // OOM
    img->pts = av_frame->pts == AV_NOPTS_VALUE ?
               MP_NOPTS_VALUE : av_frame->pts * av_q2d(p->timebase_out);
    av_frame_free(&av_frame);
    return img;
}

static void get_metadata_from_av_frame(struct vf_instance *vf, AVFrame *frame)
{
#if LIBAVUTIL_VERSION_MICRO >= 100
    struct vf_priv_s *p = vf->priv;
    if (!p->metadata)
        p->metadata = talloc_zero(p, struct mp_tags);

    mp_tags_copy_from_av_dictionary(p->metadata, av_frame_get_metadata(frame));
#endif
}

static int filter_ext(struct vf_instance *vf, struct mp_image *mpi)
{
    struct vf_priv_s *p = vf->priv;

    if (p->eof && mpi) {
        // Once EOF is reached, libavfilter is "stuck" in the EOF state, and
        // won't accept new input. Forcefully override it. This helps e.g.
        // with cover art, where we always want to generate new output.
        reset(vf);
    }

    if (!p->graph)
        return -1;

    if (!mpi) {
        if (p->eof)
            return 0;
        p->eof = true;
    }

    AVFrame *frame = mp_to_av(vf, mpi);
    int r = av_buffersrc_add_frame(p->in, frame) < 0 ? -1 : 0;
    av_frame_free(&frame);

    return r;
}

static int filter_out(struct vf_instance *vf)
{
    struct vf_priv_s *p = vf->priv;

    AVFrame *frame = av_frame_alloc();
    int err = av_buffersink_get_frame(p->out, frame);
    if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
        // Not an error situation - no more output buffers in queue.
        // AVERROR_EOF means we shouldn't even give the filter more
        // input, but we don't handle that completely correctly.
        av_frame_free(&frame);
        p->eof |= err == AVERROR_EOF;
        return 0;
    }
    if (err < 0) {
        av_frame_free(&frame);
        MP_ERR(vf, "libavfilter error: %d\n", err);
        return -1;
    }

    get_metadata_from_av_frame(vf, frame);
    vf_add_output_frame(vf, av_to_mp(vf, frame));
    return 0;
}

static int control(vf_instance_t *vf, int request, void *data)
{
    switch (request) {
    case VFCTRL_SEEK_RESET:
        reset(vf);
        return CONTROL_OK;
    case VFCTRL_COMMAND: {
        if (!vf->priv->graph)
            break;
        char **args = data;
        return avfilter_graph_send_command(vf->priv->graph, "all",
                                           args[0], args[1], &(char){0}, 0, 0)
                >= 0 ? CONTROL_OK : CONTROL_ERROR;
    }
    case VFCTRL_GET_METADATA:
        if (vf->priv && vf->priv->metadata) {
            *(struct mp_tags *)data = *vf->priv->metadata;
            return CONTROL_OK;
        } else {
            return CONTROL_NA;
        }
    }
    return CONTROL_UNKNOWN;
}

static void uninit(struct vf_instance *vf)
{
    if (!vf->priv)
        return;
    destroy_graph(vf);
}

static int vf_open(vf_instance_t *vf)
{
    vf->reconfig = reconfig;
    vf->filter_ext = filter_ext;
    vf->filter_out = filter_out;
    vf->filter = NULL;
    vf->query_format = query_format;
    vf->control = control;
    vf->uninit = uninit;
    return 1;
}

static bool is_single_video_only(const AVFilterPad *pads)
{
    int count = avfilter_pad_count(pads);
    if (count != 1)
        return false;
    return avfilter_pad_get_type(pads, 0) == AVMEDIA_TYPE_VIDEO;
}

// Does it have exactly one video input and one video output?
static bool is_usable(const AVFilter *filter)
{
    return is_single_video_only(filter->inputs) &&
           is_single_video_only(filter->outputs);
}

static void print_help(struct mp_log *log)
{
    mp_info(log, "List of libavfilter filters:\n");
    for (const AVFilter *filter = avfilter_next(NULL); filter;
         filter = avfilter_next(filter))
    {
        if (is_usable(filter))
            mp_info(log, " %-16s %s\n", filter->name, filter->description);
    }
    mp_info(log, "\n"
        "This lists video->video filters only. Refer to\n"
        "\n"
        " https://ffmpeg.org/ffmpeg-filters.html\n"
        "\n"
        "to see how to use each filter and what arguments each filter takes.\n"
        "Also, be sure to quote the FFmpeg filter string properly, e.g.:\n"
        "\n"
        " \"--vf=lavfi=[gradfun=20:30]\"\n"
        "\n"
        "Otherwise, mpv and libavfilter syntax will conflict.\n"
        "\n");
}

#define OPT_BASE_STRUCT struct vf_priv_s
static const m_option_t vf_opts_fields[] = {
    OPT_STRING("graph", cfg_graph, M_OPT_MIN, .min = 1),
    OPT_INT64("sws-flags", cfg_sws_flags, 0),
    OPT_KEYVALUELIST("o", cfg_avopts, 0),
    {0}
};

const vf_info_t vf_info_lavfi = {
    .description = "libavfilter bridge",
    .name = "lavfi",
    .open = vf_open,
    .priv_size = sizeof(struct vf_priv_s),
    .priv_defaults = &vf_priv_dflt,
    .options = vf_opts_fields,
    .print_help = print_help,
};

// The following code is for the old filters wrapper code.

struct vf_lw_opts {
    int64_t sws_flags;
    char **avopts;
};

#undef OPT_BASE_STRUCT
#define OPT_BASE_STRUCT struct vf_lw_opts
const struct m_sub_options vf_lw_conf = {
    .opts = (const m_option_t[]) {
        OPT_INT64("lavfi-sws-flags", sws_flags, 0),
        OPT_KEYVALUELIST("lavfi-o", avopts, 0),
        {0}
    },
    .defaults = &(const struct vf_lw_opts){
        .sws_flags = SWS_BICUBIC,
    },
    .size = sizeof(struct vf_lw_opts),
};

static bool have_filter(const char *name)
{
    for (const AVFilter *filter = avfilter_next(NULL); filter;
         filter = avfilter_next(filter))
    {
        if (strcmp(filter->name, name) == 0)
            return true;
    }
    return false;
}

// This is used by "old" filters for wrapping lavfi if possible.
// On success, this overwrites all vf callbacks and literally takes over the
// old filter and replaces it with vf_lavfi.
// On error (<0), nothing is changed.
int vf_lw_set_graph(struct vf_instance *vf, struct vf_lw_opts *lavfi_opts,
                    char *filter, char *opts, ...)
{
    if (!lavfi_opts)
        lavfi_opts = (struct vf_lw_opts *)vf_lw_conf.defaults;
    if (filter && !have_filter(filter))
        return -1;
    MP_VERBOSE(vf, "Using libavfilter for '%s'\n", vf->info->name);
    void *old_priv = vf->priv;
    struct vf_priv_s *p = talloc(vf, struct vf_priv_s);
    vf->priv = p;
    *p = vf_priv_dflt;
    p->cfg_sws_flags = lavfi_opts->sws_flags;
    p->cfg_avopts = lavfi_opts->avopts;
    va_list ap;
    va_start(ap, opts);
    char *s = talloc_vasprintf(vf, opts, ap);
    p->cfg_graph = filter ? talloc_asprintf(vf, "%s=%s", filter, s)
                          : talloc_strdup(vf, s);
    talloc_free(s);
    va_end(ap);
    p->old_priv = old_priv;
    // Note: we should be sure vf_open really overwrites _all_ vf callbacks.
    if (vf_open(vf) < 1)
        abort();
    return 1;
}

void *vf_lw_old_priv(struct vf_instance *vf)
{
    struct vf_priv_s *p = vf->priv;
    return p->old_priv;
}

void vf_lw_update_graph(struct vf_instance *vf, char *filter, char *opts, ...)
{
    struct vf_priv_s *p = vf->priv;
    va_list ap;
    va_start(ap, opts);
    char *s = talloc_vasprintf(vf, opts, ap);
    talloc_free(p->cfg_graph);
    p->cfg_graph = filter ? talloc_asprintf(vf, "%s=%s", filter, s)
                          : talloc_strdup(vf, s);
    talloc_free(s);
    va_end(ap);
}

void vf_lw_set_reconfig_cb(struct vf_instance *vf,
                                int (*reconfig_)(struct vf_instance *vf,
                                                 struct mp_image_params *in,
                                                 struct mp_image_params *out))
{
    vf->priv->lw_reconfig_cb = reconfig_;
}
