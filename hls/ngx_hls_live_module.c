
/*
 * Copyright (C) Pingo (cczjp89@gmail.com)
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_rtmp.h>
#include <ngx_rtmp_cmd_module.h>
#include <ngx_rtmp_codec_module.h>
#include "ngx_rbuf.h"
#include "ngx_rtmp_mpegts.h"
#include "ngx_mpegts_gop_module.h"
#include "ngx_hls_live_module.h"

static ngx_rtmp_play_pt                 next_play;
static ngx_rtmp_close_stream_pt         next_close_stream;

#define ngx_hls_live_next(s, pos) ((pos + 1) % s->out_queue)
#define ngx_hls_live_prev(s, pos) (pos == 0 ? s->out_queue - 1 : pos - 1)

static ngx_int_t ngx_hls_live_postconfiguration(ngx_conf_t *cf);
static void * ngx_hls_live_create_app_conf(ngx_conf_t *cf);
static char * ngx_hls_live_merge_app_conf(ngx_conf_t *cf,
       void *parent, void *child);

typedef struct {
    ngx_flag_t                          hls;
    ngx_msec_t                          fraglen;
    ngx_msec_t                          max_fraglen;
    ngx_msec_t                          playlen;
    ngx_uint_t                          winfrags;
    ngx_uint_t                          minfrags;
    ngx_flag_t                          nested;
    ngx_uint_t                          slicing;
    ngx_uint_t                          type;
    ngx_path_t                         *slot;
    size_t                              audio_buffer_size;
    ngx_flag_t                          cleanup;
    ngx_array_t                        *variant;
    ngx_str_t                           base_url;
    ngx_hls_live_frag_t                *free_frag;
    ngx_pool_t                         *pool;
    ngx_msec_t                          timeout;
} ngx_hls_live_app_conf_t;


#define NGX_RTMP_HLS_NAMING_SEQUENTIAL  1
#define NGX_RTMP_HLS_NAMING_TIMESTAMP   2
#define NGX_RTMP_HLS_NAMING_SYSTEM      3


#define NGX_RTMP_HLS_SLICING_PLAIN      1
#define NGX_RTMP_HLS_SLICING_ALIGNED    2


#define NGX_RTMP_HLS_TYPE_LIVE          1
#define NGX_RTMP_HLS_TYPE_EVENT         2


static ngx_conf_enum_t                  ngx_hls_live_slicing_slots[] = {
    { ngx_string("plain"),              NGX_RTMP_HLS_SLICING_PLAIN },
    { ngx_string("aligned"),            NGX_RTMP_HLS_SLICING_ALIGNED  },
    { ngx_null_string,                  0 }
};


static ngx_conf_enum_t                  ngx_hls_live_type_slots[] = {
    { ngx_string("live"),               NGX_RTMP_HLS_TYPE_LIVE  },
    { ngx_string("event"),              NGX_RTMP_HLS_TYPE_EVENT },
    { ngx_null_string,                  0 }
};


static ngx_command_t ngx_hls_live_commands[] = {

    { ngx_string("hls2memory"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_hls_live_app_conf_t, hls),
      NULL },

    { ngx_string("hls2_fragment"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_hls_live_app_conf_t, fraglen),
      NULL },

    { ngx_string("hls2_max_fragment"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_hls_live_app_conf_t, max_fraglen),
      NULL },

    { ngx_string("hls2_playlist_length"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_hls_live_app_conf_t, playlen),
      NULL },

    { ngx_string("hls2_minfrags"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_hls_live_app_conf_t, minfrags),
      NULL },

    { ngx_string("hls2_nested"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_hls_live_app_conf_t, nested),
      NULL },

    { ngx_string("hls2_fragment_slicing"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_hls_live_app_conf_t, slicing),
      &ngx_hls_live_slicing_slots },

    { ngx_string("hls2_type"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_hls_live_app_conf_t, type),
      &ngx_hls_live_type_slots },

    { ngx_string("hls2_audio_buffer_size"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_hls_live_app_conf_t, audio_buffer_size),
      NULL },

    { ngx_string("hls2_cleanup"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_hls_live_app_conf_t, cleanup),
      NULL },

    { ngx_string("hls2_base_url"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_hls_live_app_conf_t, base_url),
      NULL },

    { ngx_string("hls2_timeout"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_hls_live_app_conf_t, timeout),
      NULL },

    ngx_null_command
};


static ngx_rtmp_module_t  ngx_hls_live_module_ctx = {
    NULL,                               /* preconfiguration */
    ngx_hls_live_postconfiguration,     /* postconfiguration */

    NULL,                               /* create main configuration */
    NULL,                               /* init main configuration */

    NULL,                               /* create server configuration */
    NULL,                               /* merge server configuration */

    ngx_hls_live_create_app_conf,       /* create location configuration */
    ngx_hls_live_merge_app_conf,        /* merge location configuration */
};


ngx_module_t  ngx_hls_live_module = {
    NGX_MODULE_V1,
    &ngx_hls_live_module_ctx,           /* module context */
    ngx_hls_live_commands,              /* module directives */
    NGX_RTMP_MODULE,                    /* module type */
    NULL,                               /* init master */
    NULL,                               /* init module */
    NULL,                               /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_hls_live_frag_t *
ngx_hls_live_get_frag(ngx_rtmp_session_t *s, ngx_int_t n)
{
    ngx_hls_live_ctx_t         *ctx;
    ngx_hls_live_app_conf_t    *hacf;

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_hls_live_module);
    ctx = ngx_rtmp_get_module_ctx(s, ngx_hls_live_module);

    return ctx->frags[(ctx->nfrag + n) % (hacf->winfrags * 2 + 1)];
}


static void
ngx_hls_live_next_frag(ngx_rtmp_session_t *s)
{
    ngx_hls_live_ctx_t         *ctx;
    ngx_hls_live_app_conf_t    *hacf;

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_hls_live_module);
    ctx = ngx_rtmp_get_module_ctx(s, ngx_hls_live_module);

    if (ctx->nfrags == hacf->winfrags) {
        ctx->nfrag++;
    } else {
        ctx->nfrags++;
    }
}


ngx_int_t
ngx_hls_live_write_playlist(ngx_rtmp_session_t *s, ngx_buf_t *out)
{
    u_char                         *p, *end;
    ngx_hls_live_ctx_t             *ctx;
    ngx_hls_live_app_conf_t        *hacf;
    ngx_hls_live_frag_t            *frag;
    ngx_uint_t                      i, max_frag;
    ngx_str_t                       name_part;
    ngx_str_t                       m3u8;

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_hls_live_module);
    ctx = ngx_rtmp_get_module_ctx(s, ngx_hls_live_module);

    if (ctx->nfrags < 2) {
        return NGX_AGAIN;
    }

    ctx->last_time = ngx_time();

    max_frag = hacf->fraglen / 1000;

    for (i = 0; i < ctx->nfrags; i++) {
        frag = ngx_hls_live_get_frag(s, i);
        if (frag->duration > max_frag) {
            max_frag = (ngx_uint_t) (frag->duration + .5);
        }
    }

    p = out->pos;
    end = out->end;

    p = ngx_slprintf(p, end,
                     "#EXTM3U\n"
                     "#EXT-X-VERSION:3\n"
                     "#EXT-X-MEDIA-SEQUENCE:%uL\n"
                     "#EXT-X-TARGETDURATION:%ui\n",
                     ctx->nfrag, max_frag);

    if (hacf->type == NGX_RTMP_HLS_TYPE_EVENT) {
        p = ngx_slprintf(p, end, "#EXT-X-PLAYLIST-TYPE: EVENT\n");
    }

    name_part = s->name;

    for (i = 0; i < ctx->nfrags; i++) {
        frag = ngx_hls_live_get_frag(s, i);

        if (frag->discont) {
            p = ngx_slprintf(p, end, "#EXT-X-DISCONTINUITY\n");
        }

        p = ngx_slprintf(p, end,
            "#EXTINF:%.3f,\n"
            "%V%V-%uL.ts?session=%V\n",
            frag->duration, &hacf->base_url,
            &name_part, frag->id, &ctx->sid);

        ngx_log_debug5(NGX_LOG_DEBUG_RTMP, s->log, 0,
            "hls: fragment nfrag=%uL, n=%ui/%ui, duration=%.3f, "
            "discont=%i",
            ctx->nfrag, i + 1, ctx->nfrags, frag->duration, frag->discont);
    }

    out->last = p;
    m3u8.data = out->pos;
    m3u8.len = out->last - out->pos;

    ngx_log_error(NGX_LOG_DEBUG, s->log, 0, "hls-live: playlist| %V", &m3u8);

    return NGX_OK;
}


ngx_hls_live_frag_t*
ngx_hls_live_find_frag(ngx_rtmp_session_t *s, ngx_str_t *name)
{
    ngx_hls_live_ctx_t        *ctx;
    u_char                    *p0, *p1, *e;
    ngx_uint_t                 frag_id;
    ngx_hls_live_frag_t       *frag;
    ngx_hls_live_app_conf_t   *hacf;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_hls_live_module);

    p0 = name->data;
    e = p0 + name->len;

    for (; *e != '.' && e != p0; e--);

    if (e == p0) {
        return NULL;
    }

    p1 = e;

    for (; *e != '-' && e != p0; e--);

    if (e == p0) {
        return NULL;
    }

    p0 = e + 1;

    frag_id = ngx_atoi(p0, p1 - p0);

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_hls_live_module);

    if (frag_id > ctx->nfrag + ctx->nfrags ||
        ctx->nfrag + ctx->nfrags - frag_id > hacf->winfrags * 2 + 1)
    {
        ngx_log_error(NGX_LOG_ERR, s->log, 0,
            "hls-live: find_frag| invalid frag id[%d], curr id [%d]",
            frag_id, ctx->nfrag + ctx->nfrags);
        return NULL;
    }

    frag = ctx->frags[frag_id % (hacf->winfrags * 2 + 1)];

    ngx_log_error(NGX_LOG_DEBUG, s->log, 0,
        "hls-live: find_frag| find frag %p [%d] [frag %d] length %ui",
        frag, frag_id, frag->id, frag->length);

    return frag;
}


ngx_chain_t*
ngx_hls_live_prepare_frag(ngx_rtmp_session_t *s, ngx_hls_live_frag_t *frag)
{
    ngx_hls_live_ctx_t   *ctx;
    ngx_chain_t          *out, *cl, **ll;
    ngx_mpegts_frame_t   *frame;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_hls_live_module);

    if (!ctx->patpmt) {
        ctx->patpmt = ngx_create_temp_buf(s->pool, 376);
        ctx->patpmt->last = ngx_cpymem(ctx->patpmt->pos,
            ngx_rtmp_mpegts_pat, 188);

        ngx_rtmp_mpegts_gen_pmt(s->vcodec, s->acodec, s->log, ctx->patpmt->last);
        ctx->patpmt->last += 188;
    }

    ll = &out;
    *ll = ngx_get_chainbuf(0, 0);
    *((*ll)->buf) = *(ctx->patpmt);
    ll = &((*ll)->next);

    while (frag->content_pos != frag->content_last) {
        frame = frag->content[frag->content_pos];

        cl = frame->chain;
        while (cl) {
            *ll = ngx_get_chainbuf(0, 0);
            *((*ll)->buf) = *(cl->buf);
            (*ll)->buf->flush = 1;

            if (frag->content_pos == frag->content_last && cl->next == NULL) {
                (*ll)->buf->last_in_chain = 1;
            }

            ll = &((*ll)->next);
            cl = cl->next;
        }

        *ll = NULL;
        frag->content_pos = ngx_hls_live_next(s, frag->content_pos);
    }

    frag->ref++;

    frag->out = out;

    return out;
}


ngx_rtmp_session_t*
ngx_hls_live_fetch_session(ngx_str_t *server,
    ngx_str_t *stream, ngx_str_t *session)
{
    ngx_live_stream_t    *live_stream;
    ngx_rtmp_core_ctx_t  *lctx;
    ngx_hls_live_ctx_t   *ctx;

    live_stream = ngx_live_fetch_stream(server, stream);
    if (live_stream) {
        for (lctx = live_stream->hls_play_ctx; lctx; lctx = lctx->next) {
            ctx = ngx_rtmp_get_module_ctx(lctx->session, ngx_hls_live_module);
            if (session->len == ctx->sid.len &&
                !ngx_strncmp(ctx->sid.data, session->data, session->len))
            {
                return lctx->session;
            }
        }
    }

    return NULL;
}


static uint64_t
ngx_hls_live_get_fragment_id(ngx_rtmp_session_t *s, uint64_t ts)
{
    ngx_hls_live_ctx_t         *ctx;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_hls_live_module);

    return ctx->nfrag + ctx->nfrags;
}


static ngx_int_t
ngx_hls_live_close_fragment(ngx_rtmp_session_t *s)
{
    ngx_hls_live_ctx_t        *ctx;
    ngx_hls_live_app_conf_t   *hacf;

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_hls_live_module);
    ctx = ngx_rtmp_get_module_ctx(s, ngx_hls_live_module);

    if (ctx == NULL || !ctx->opened) {
        return NGX_OK;
    }

    ngx_log_error(NGX_LOG_DEBUG, s->log, 0,
                   "hls: close fragment id=%uL", ctx->nfrag);

    ctx->opened = 0;

    ngx_hls_live_next_frag(s);

    if (ctx->nfrags >= hacf->minfrags && !ctx->playing) {
        if (ngx_rtmp_fire_event(s, NGX_MPEGTS_MSG_M3U8, NULL, NULL) == NGX_OK) {
            ctx->playing = 1;
        }
    }

    return NGX_OK;
}


void
ngx_hls_live_free_frag(ngx_rtmp_session_t *s, ngx_hls_live_frag_t *frag)
{
    ngx_mpegts_frame_t        *frame;
    ngx_uint_t                 i;
    ngx_chain_t               *ll, *cl;
    ngx_hls_live_app_conf_t   *hacf;

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_hls_live_module);

    frag->ref--;

    ngx_log_error(NGX_LOG_DEBUG, s->log, 0,
        "hls-live: free_frag| frag[%p] ref %ui", frag, frag->ref);

    if (frag->ref > 0) {
        return;
    }

    for (i = 0; i < frag->content_last; ++i) {
        frame = frag->content[i];
        if (frame) {
            ngx_rtmp_shared_free_mpegts_frame(frame);
        }
    }

    ll = frag->out;
    while (ll) {
        cl = ll->next;
        ngx_put_chainbuf(ll);
        ll = cl;
    }

    frag->next = hacf->free_frag;
    hacf->free_frag = frag;
}


static ngx_hls_live_frag_t*
ngx_hls_live_create_frag(ngx_rtmp_session_t *s) {
    ngx_hls_live_frag_t       *frag;
    ngx_hls_live_app_conf_t   *hacf;

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_hls_live_module);

    if (hacf->free_frag) {
        frag = hacf->free_frag;
        hacf->free_frag = hacf->free_frag->next;
    } else {
        frag = ngx_pcalloc(hacf->pool, sizeof(ngx_hls_live_frag_t) +
            sizeof(ngx_mpegts_frame_t*) * s->out_queue);
    }

    ngx_log_error(NGX_LOG_DEBUG, s->log, 0,
        "hls-live: create_frag| create frag[%p]", frag);

    return frag;
}


static ngx_int_t
ngx_hls_live_open_fragment(ngx_rtmp_session_t *s, uint64_t ts,
    ngx_int_t discont)
{
    uint64_t                  id;
    ngx_hls_live_ctx_t       *ctx;
    ngx_hls_live_frag_t     **ffrag, *frag;
    ngx_hls_live_app_conf_t  *hacf;

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_hls_live_module);

    ctx = ngx_rtmp_get_module_ctx(s, ngx_hls_live_module);

    if (ctx->opened) {
        return NGX_OK;
    }

    id = ngx_hls_live_get_fragment_id(s, ts);

    ngx_log_error(NGX_LOG_DEBUG, s->log, 0,
            "hls: open_fragment| create frag[%uL] timestamp %uL", id, ts);

    ffrag = &(ctx->frags[id % (hacf->winfrags * 2 + 1)]);
    if (*ffrag) {
        ngx_hls_live_free_frag(s, *ffrag);
    }
    *ffrag = ngx_hls_live_create_frag(s);

    frag = *ffrag;

    ngx_memzero(frag, sizeof(*frag));

    frag->ref = 1;
    frag->active = 1;
    frag->discont = discont;
    frag->id = id;

    ctx->opened = 1;
    ctx->frag_ts = ts;

    return NGX_OK;
}


static void
ngx_hls_live_timeout(ngx_event_t *ev)
{
    ngx_rtmp_session_t   *s;
    ngx_hls_live_ctx_t   *ctx;

    s = ev->data;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_hls_live_module);

    if (ngx_time() - ctx->last_time > ctx->timeout/1000) {
        ngx_rtmp_finalize_fake_session(s);
        return;
    }

    ngx_add_timer(ev, ctx->timeout);
}


static ngx_int_t
ngx_hls_live_play(ngx_rtmp_session_t *s, ngx_rtmp_play_t *v)
{
    ngx_hls_live_app_conf_t        *hacf;
    ngx_hls_live_ctx_t             *ctx;

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_hls_live_module);
    if (hacf == NULL || !hacf->hls) {
        goto next;
    }

    if (s->interprocess) {
        goto next;
    }

    ctx = ngx_rtmp_get_module_ctx(s, ngx_hls_live_module);
    if (ctx) {
        goto next;
    }

    ctx = ngx_pcalloc(s->pool, sizeof(ngx_hls_live_ctx_t));
    ngx_rtmp_set_ctx(s, ctx, ngx_hls_live_module);

    ctx->sid.len = ngx_strlen(v->session);
    ctx->sid.data = ngx_pcalloc(s->pool, ctx->sid.len);
    ngx_memcpy(ctx->sid.data, v->session, ctx->sid.len);

    if (ctx->frags == NULL) {
        ctx->frags = ngx_pcalloc(s->pool,
            sizeof(ngx_hls_live_frag_t *) * (hacf->winfrags * 2 + 1));
        if (ctx->frags == NULL) {
            return NGX_ERROR;
        }
    }

    ctx->ev.data = s;
    ctx->ev.handler = ngx_hls_live_timeout;
    ctx->ev.log = s->log;
    ctx->timeout = hacf->timeout;

    ngx_add_timer(&ctx->ev, hacf->timeout);

next:
    return next_play(s, v);
}


static ngx_int_t
ngx_hls_live_close_stream(ngx_rtmp_session_t *s, ngx_rtmp_close_stream_t *v)
{
    ngx_hls_live_app_conf_t   *hacf;
    ngx_hls_live_ctx_t        *ctx;
    ngx_uint_t                 i;
    ngx_hls_live_frag_t       *frag;

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_hls_live_module);

    ctx = ngx_rtmp_get_module_ctx(s, ngx_hls_live_module);

    if (hacf == NULL || !hacf->hls || ctx == NULL) {
        goto next;
    }

    if (ctx->ev.timer_set) {
        ngx_del_timer(&ctx->ev);
    }

    ngx_rtmp_fire_event(s, NGX_MPEGTS_MSG_CLOSE, NULL, NULL);

    ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->log, 0,
                   "hls: close stream");

    ngx_hls_live_close_fragment(s);

    for (i = 0; i < 2 * hacf->winfrags + 1; i++) {
        frag = ctx->frags[i % (hacf->winfrags * 2 + 1)];
        if (frag) {
            ngx_hls_live_free_frag(s, frag);
        }
    }

next:
    return next_close_stream(s, v);
}


static void
ngx_hls_live_update_fragment(ngx_rtmp_session_t *s, uint64_t ts,
    ngx_int_t boundary)
{
    ngx_hls_live_ctx_t         *ctx;
    ngx_hls_live_app_conf_t    *hacf;
    ngx_hls_live_frag_t        *frag;
    ngx_msec_t                  ts_frag_len;
    ngx_int_t                   same_frag, force,discont;
    int64_t                     d;

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_hls_live_module);
    ctx = ngx_rtmp_get_module_ctx(s, ngx_hls_live_module);
    frag = NULL;
    force = 0;
    discont = 1;

    if (ctx->opened) {
        frag = ngx_hls_live_get_frag(s, ctx->nfrags);
        d = (int64_t) (ts - ctx->frag_ts);

        if (d > (int64_t) hacf->max_fraglen || d < -2000) {
            ngx_log_error(NGX_LOG_DEBUG, s->log, 0,
                          "hls: force fragment split: %.3f sec, ", d / 1000.);
            force = 1;

        } else {
            frag->duration = (ts - ctx->frag_ts) / 1000.;
            discont = 0;
        }
    }

    switch (hacf->slicing) {
        case NGX_RTMP_HLS_SLICING_PLAIN:
            if (frag && frag->duration < hacf->fraglen / 1000.) {
                boundary = 0;
            }
            break;

        case NGX_RTMP_HLS_SLICING_ALIGNED:

            ts_frag_len = hacf->fraglen;
            same_frag = ctx->frag_ts / ts_frag_len == ts / ts_frag_len;

            if (frag && same_frag) {
                boundary = 0;
            }

            if (frag == NULL && (ctx->frag_ts == 0 || same_frag)) {
                ctx->frag_ts = ts;
                boundary = 0;
            }

            break;
    }

    if (boundary || force) {
        ngx_hls_live_close_fragment(s);
        ngx_hls_live_open_fragment(s, ts, discont);
    }
}


static ngx_int_t
ngx_hls_live_mpegts_write_frame(ngx_rtmp_session_t *s,
    ngx_hls_live_frag_t *nfrag, ngx_mpegts_frame_t *frame)
{
    ngx_hls_live_frag_t   *frag;
    ngx_hls_live_ctx_t    *ctx;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_hls_live_module);

    frag = ngx_hls_live_get_frag(s, ctx->nfrags);

    frag->length += frame->length;

    frag->content[frag->content_last] = frame;
    frag->content_last = ngx_hls_live_next(s, frag->content_last);

    ngx_rtmp_shared_acquire_frame(frame);

    return NGX_OK;
}


static ngx_int_t
ngx_hls_live_update(ngx_rtmp_session_t *s, ngx_rtmp_codec_ctx_t *codec_ctx)
{
    ngx_hls_live_ctx_t   *ctx;
    ngx_mpegts_frame_t   *frame;
    ngx_int_t             boundary;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_hls_live_module);

    while (s->out_pos != s->out_last) {

        if (s->out_pos == s->out_last) {
            break;
        }

        frame = s->mpegts_out[s->out_pos];
#if 0
        ngx_log_error(NGX_LOG_DEBUG, s->log, 0,
            "hls-live: update| "
            "frame[%p] pos[%O] last[%O] pts[%uL] type [%d], key %d, opened %d",
            frame, s->out_pos, s->out_last,frame->pts,
            frame->type, frame->keyframe, ctx->opened);
#endif
        boundary = 0;

        if (frame->type == NGX_MPEGTS_MSG_AUDIO) {
            boundary = codec_ctx->avc_header == NULL;
        } else if (frame->type == NGX_MPEGTS_MSG_VIDEO) {
            boundary = frame->keyframe &&
                (codec_ctx->aac_header == NULL || !ctx->opened);
        } else {
            return NGX_ERROR;
        }

        s->acodec = codec_ctx->audio_codec_id;
        s->vcodec = codec_ctx->video_codec_id;

        if (frame->type == NGX_MPEGTS_MSG_VIDEO) {
            ngx_hls_live_update_fragment(s, frame->pts, boundary);
        } else {
            ngx_hls_live_update_fragment(s, frame->pts, boundary);
//                frame->pts + frame->duration, boundary);
        }
        if (!ctx->opened) {
            break;
        }

        ngx_hls_live_mpegts_write_frame(s, ctx->frag, frame);

        ngx_rtmp_shared_free_mpegts_frame(frame);

        ++s->out_pos;
        s->out_pos %= s->out_queue;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_hls_live_av(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
    ngx_chain_t *in)
{
    ngx_mpegts_frame_t        *frame;
    ngx_live_stream_t         *live_stream;
    ngx_rtmp_core_ctx_t       *live_ctx;
    ngx_rtmp_session_t        *ss;
    ngx_rtmp_codec_ctx_t      *codec_ctx;
    ngx_hls_live_app_conf_t   *hacf;

    codec_ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);
    hacf = ngx_rtmp_get_module_app_conf(s, ngx_hls_live_module);

    frame = ngx_rtmp_shared_alloc_mpegts_frame(in, 1);
    frame->length = h->mlen;
    frame->pts = h->pts / 90;
    frame->type = h->type;
    frame->keyframe = h->keyframe;
    frame->duration = h->duration / 90;

    ngx_mpegts_gop_cache(s, frame);

    live_stream = s->live_stream;
    for (live_ctx = live_stream->hls_play_ctx; live_ctx;
        live_ctx = live_ctx->next)
    {
        ss = live_ctx->session;

        switch (ngx_mpegts_gop_link(s, ss, hacf->playlen, hacf->playlen)) {
        case NGX_DECLINED:
            continue;
        case NGX_ERROR:
            ngx_rtmp_finalize_session(ss);
            continue;
        default:
            break;
        }

        ngx_hls_live_update(ss, codec_ctx);
    }

    ngx_rtmp_shared_free_mpegts_frame(frame);

    return NGX_OK;
}


static void *
ngx_hls_live_create_app_conf(ngx_conf_t *cf)
{
    ngx_hls_live_app_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_hls_live_app_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->pool = ngx_create_pool(1024, ngx_cycle->log);

    conf->hls = NGX_CONF_UNSET;
    conf->fraglen = NGX_CONF_UNSET_MSEC;
    conf->max_fraglen = NGX_CONF_UNSET_MSEC;
    conf->playlen = NGX_CONF_UNSET_MSEC;
    conf->slicing = NGX_CONF_UNSET_UINT;
    conf->type = NGX_CONF_UNSET_UINT;
    conf->audio_buffer_size = NGX_CONF_UNSET_SIZE;
    conf->cleanup = NGX_CONF_UNSET;
    conf->timeout = NGX_CONF_UNSET_MSEC;
    conf->minfrags = NGX_CONF_UNSET_UINT;

    return conf;
}


static char *
ngx_hls_live_merge_app_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_hls_live_app_conf_t    *prev = parent;
    ngx_hls_live_app_conf_t    *conf = child;

    ngx_conf_merge_value(conf->hls, prev->hls, 0);
    ngx_conf_merge_msec_value(conf->fraglen, prev->fraglen, 5000);
    ngx_conf_merge_msec_value(conf->max_fraglen, prev->max_fraglen,
                              conf->fraglen * 10);
    ngx_conf_merge_msec_value(conf->playlen, prev->playlen, 30000);
    ngx_conf_merge_uint_value(conf->slicing, prev->slicing,
                              NGX_RTMP_HLS_SLICING_PLAIN);
    ngx_conf_merge_uint_value(conf->type, prev->type,
                              NGX_RTMP_HLS_TYPE_LIVE);
    ngx_conf_merge_value(conf->cleanup, prev->cleanup, 1);
    ngx_conf_merge_str_value(conf->base_url, prev->base_url, "");
    ngx_conf_merge_msec_value(conf->timeout, prev->timeout, conf->playlen * 2);
    ngx_conf_merge_uint_value(conf->minfrags, prev->minfrags, 2);

    if (conf->fraglen) {
        conf->winfrags = conf->playlen / conf->fraglen;
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_hls_live_postconfiguration(ngx_conf_t *cf)
{
    ngx_rtmp_core_main_conf_t   *cmcf;
    ngx_rtmp_handler_pt         *h;

    cmcf = ngx_rtmp_conf_get_module_main_conf(cf, ngx_rtmp_core_module);

    h = ngx_array_push(&cmcf->events[NGX_MPEGTS_MSG_VIDEO]);
    *h = ngx_hls_live_av;

    h = ngx_array_push(&cmcf->events[NGX_MPEGTS_MSG_AUDIO]);
    *h = ngx_hls_live_av;

    next_play = ngx_rtmp_play;
    ngx_rtmp_play = ngx_hls_live_play;

    next_close_stream = ngx_rtmp_close_stream;
    ngx_rtmp_close_stream = ngx_hls_live_close_stream;

    return NGX_OK;
}
