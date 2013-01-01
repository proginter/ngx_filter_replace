
/*
 * Copyright (C) Yichun Zhang (agentzh)
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <sregex/sregex.h>


typedef struct {
    ngx_str_t                  match;
    ngx_str_t                  value;

    ngx_hash_t                 types;

    ngx_flag_t                 once;
    unsigned                   ncaps;
    unsigned                   ovecsize;
    sre_program_t             *program;

    ngx_array_t               *types_keys;
} ngx_http_replace_loc_conf_t;


typedef struct {
    int                        stream_pos;
    int                       *ovector;
    sre_pool_t                *vm_pool;
    sre_vm_pike_ctx_t         *vm_ctx;

    ngx_chain_t               *pending;
    ngx_chain_t              **last_pending;

    ngx_buf_t                 *buf;

    u_char                    *pos;
    u_char                    *copy_start;
    u_char                    *copy_end;

    ngx_chain_t               *in;
    ngx_chain_t               *out;
    ngx_chain_t              **last_out;
    ngx_chain_t               *busy;
    ngx_chain_t               *free;
    ngx_chain_t               *special;
    ngx_chain_t              **last_special;

    unsigned                   once:1;
    unsigned                   vm_done:1;
    unsigned                   special_buf:1;
    unsigned                   last_buf:1;
} ngx_http_replace_ctx_t;


static ngx_int_t ngx_http_replace_output(ngx_http_request_t *r,
    ngx_http_replace_ctx_t *ctx);
static ngx_int_t ngx_http_replace_parse(ngx_http_request_t *r,
    ngx_http_replace_ctx_t *ctx);

static char * ngx_http_replace_filter(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static void *ngx_http_replace_create_conf(ngx_conf_t *cf);
static char *ngx_http_replace_merge_conf(ngx_conf_t *cf,
    void *parent, void *child);
static ngx_int_t ngx_http_replace_filter_init(ngx_conf_t *cf);
void ngx_http_replace_cleanup_pool(void *data);
static ngx_chain_t * ngx_http_replace_get_free_buf(ngx_pool_t *p,
    ngx_chain_t **free);


static ngx_command_t  ngx_http_replace_filter_commands[] = {

    { ngx_string("replace_filter"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE23,
      ngx_http_replace_filter,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("replace_filter_types"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_types_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_replace_loc_conf_t, types_keys),
      &ngx_http_html_default_types[0] },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_replace_filter_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_replace_filter_init,              /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_replace_create_conf,          /* create location configuration */
    ngx_http_replace_merge_conf            /* merge location configuration */
};


ngx_module_t  ngx_http_replace_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_replace_filter_module_ctx,       /* module context */
    ngx_http_replace_filter_commands,          /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;


static ngx_int_t
ngx_http_replace_header_filter(ngx_http_request_t *r)
{
    ngx_pool_cleanup_t            *cln;
    ngx_http_replace_ctx_t        *ctx;
    ngx_http_replace_loc_conf_t  *slcf;

    slcf = ngx_http_get_module_loc_conf(r, ngx_http_replace_filter_module);

    if (slcf->match.len == 0
        || r->headers_out.content_length_n == 0
        || ngx_http_test_content_type(r, &slcf->types) == NULL)
    {
        return ngx_http_next_header_filter(r);
    }

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_replace_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ctx->last_special = &ctx->special;
    ctx->last_pending = &ctx->pending;

    ctx->ovector = ngx_palloc(r->pool, slcf->ovecsize);
    if (ctx->ovector == NULL) {
        return NGX_ERROR;
    }

    ctx->vm_pool = sre_create_pool(1024);
    if (ctx->vm_pool == NULL) {
        return NGX_ERROR;
    }

    dd("created vm pool %p", ctx->vm_pool);

    cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        sre_destroy_pool(ctx->vm_pool);
        return NGX_ERROR;
    }

    cln->data = ctx->vm_pool;
    cln->handler = ngx_http_replace_cleanup_pool;

    ctx->vm_ctx = sre_vm_pike_create_ctx(ctx->vm_pool, slcf->program,
                                         ctx->ovector, slcf->ovecsize);
    if (ctx->vm_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_http_set_ctx(r, ctx, ngx_http_replace_filter_module);

    /* TODO register a pool cleanup handler to destroy ctx->vm_pool */

    ctx->last_out = &ctx->out;

    r->filter_need_in_memory = 1;

    if (r == r->main) {
        ngx_http_clear_content_length(r);
        ngx_http_clear_last_modified(r);
    }

    return ngx_http_next_header_filter(r);
}


void
ngx_http_replace_cleanup_pool(void *data)
{
    sre_pool_t          *pool = data;

    if (pool) {
        dd("destroy sre pool %p", pool);
        sre_destroy_pool(pool);
    }
}


static ngx_int_t
ngx_http_replace_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_int_t                  rc;
    ngx_buf_t                 *b;
    ngx_chain_t               *cl;

    ngx_http_replace_ctx_t        *ctx;
    ngx_http_replace_loc_conf_t   *slcf;

    ctx = ngx_http_get_module_ctx(r, ngx_http_replace_filter_module);

    if (ctx == NULL) {
        return ngx_http_next_body_filter(r, in);
    }

    if ((in == NULL
         && ctx->buf == NULL
         && ctx->in == NULL
         && ctx->busy == NULL))
    {
        return ngx_http_next_body_filter(r, in);
    }

    if ((ctx->once || ctx->vm_done) && (ctx->buf == NULL || ctx->in == NULL)) {

        if (ctx->busy) {
            if (ngx_http_replace_output(r, ctx) == NGX_ERROR) {
                return NGX_ERROR;
            }
        }

        return ngx_http_next_body_filter(r, in);
    }

    /* add the incoming chain to the chain ctx->in */

    if (in) {
        if (ngx_chain_add_copy(r->pool, &ctx->in, in) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http sub filter \"%V\"", &r->uri);

    while (ctx->in || ctx->buf) {

        if (ctx->buf == NULL) {
            ctx->buf = ctx->in->buf;
            ctx->in = ctx->in->next;

            ctx->pos = ctx->buf->pos;
            ctx->special_buf = ngx_buf_special(ctx->buf);
            ctx->last_buf = (ctx->buf->last_buf || ctx->buf->last_in_chain);

            dd("=== new incoming buf: size=%d, special=%u, last=%u",
               (int) ngx_buf_size(ctx->buf), ctx->special_buf,
               ctx->last_buf);
        }

        b = NULL;

        while (ctx->pos < ctx->buf->last
               || (ctx->special_buf && ctx->last_buf))
        {
            rc = ngx_http_replace_parse(r, ctx);

            ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "replace filter parse: %d, %p-%p",
                           rc, ctx->copy_start, ctx->copy_end);

            if (rc == NGX_ERROR) {
                return rc;
            }

            if (rc == NGX_DECLINED) {

                if (ctx->pending) {
                    *ctx->last_out = ctx->pending;
                    ctx->last_out = ctx->last_pending;

                    ctx->pending = NULL;
                    ctx->last_pending = &ctx->pending;
                }

                if (!ctx->special_buf) {
                    ctx->copy_start = ctx->pos;
                    ctx->copy_end = ctx->buf->last;
                    ctx->pos = ctx->buf->last;

                } else {
                    ctx->copy_start = NULL;
                    ctx->copy_end = NULL;
                }

                sre_reset_pool(ctx->vm_pool);
                ctx->vm_done = 1;
            }

            dd("copy_end - copy_start: %d, special: %u",
               (int) (ctx->copy_end - ctx->copy_start), ctx->special_buf);

            if (ctx->copy_start != ctx->copy_end && !ctx->special_buf) {

                dd("copy: %.*s", (int) (ctx->copy_end - ctx->copy_start),
                   ctx->copy_start);

                cl = ngx_http_replace_get_free_buf(r->pool, &ctx->free);
                if (cl == NULL) {
                    return NGX_ERROR;
                }

                b = cl->buf;
                ngx_memcpy(b, ctx->buf, sizeof(ngx_buf_t));

                b->pos = ctx->copy_start;
                b->last = ctx->copy_end;

                if (b->in_file) {
                    b->file_last = b->file_pos + (b->last - ctx->buf->pos);
                    b->file_pos += b->pos - ctx->buf->pos;
                }

                *ctx->last_out = cl;
                ctx->last_out = &cl->next;
            }

            if (rc == NGX_AGAIN) {
                if (ctx->special_buf && ctx->last_buf) {
                    return NGX_ERROR;
                }

                continue;
            }

            if (rc == NGX_DECLINED) {
                break;
            }

            /* rc == NGX_OK */

            cl = ngx_http_replace_get_free_buf(r->pool, &ctx->free);
            if (cl == NULL) {
                return NGX_ERROR;
            }

            b = cl->buf;

            b->flush = 0;
            b->shadow = NULL;

            slcf = ngx_http_get_module_loc_conf(r,
                                                ngx_http_replace_filter_module);

            dd("emit replaced value: \"%.*s\"", (int) slcf->value.len,
               slcf->value.data);

            if (slcf->value.len) {
                b->memory = 1;
                b->pos = slcf->value.data;
                b->last = slcf->value.data + slcf->value.len;

            } else {
                b->sync = 1;
            }

            cl->buf = b;
            cl->next = NULL;

            *ctx->last_out = cl;
            ctx->last_out = &cl->next;

            ctx->once = slcf->once;

            if (ctx->special_buf) {
                break;
            }

            continue;
        }

        if (ctx->buf->flush || ctx->last_buf || ngx_buf_in_memory(ctx->buf)) {
            if (b == NULL) {
                cl = ngx_http_replace_get_free_buf(r->pool, &ctx->free);
                if (cl == NULL) {
                    return NGX_ERROR;
                }

                b = cl->buf;
                b->sync = 1;

                *ctx->last_out = cl;
                ctx->last_out = &cl->next;
            }

            dd("setting shadow and last buf: %d", (int) ctx->buf->last_buf);
            b->last_buf = ctx->buf->last_buf;
            b->last_in_chain = ctx->buf->last_in_chain;
            b->flush = ctx->buf->flush;
            b->shadow = ctx->buf;
            b->recycled = ctx->buf->recycled;
        }

        if (!ctx->special_buf) {
            ctx->stream_pos += ctx->buf->last - ctx->buf->pos;
        }

        ctx->buf = NULL;

#if (DDEBUG)
        if (ctx->pending == NULL) {
            dd("empty ctx->pending buf");
            if (ctx->last_pending != &ctx->pending) {
                dd("BAD ctx->last_pending");
                return NGX_ERROR;
            }
        }

        for (cl = ctx->pending; cl; cl = cl->next) {
            dd("ctx->pending buf: \"%.*s\"", (int) ngx_buf_size(cl->buf),
               cl->buf->pos);

            if (cl->next == NULL) {
                if (ctx->last_pending != &cl->next) {
                    dd("BAD ctx->last_pending");
                    return NGX_ERROR;
                }
            }
        }
#endif
    }

    if (ctx->out == NULL && ctx->busy == NULL) {
        return NGX_OK;
    }

    return ngx_http_replace_output(r, ctx);
}


static ngx_int_t
ngx_http_replace_output(ngx_http_request_t *r, ngx_http_replace_ctx_t *ctx)
{
    ngx_int_t     rc;
    ngx_buf_t    *b;
    ngx_chain_t  *cl;

#if (DDEBUG)
    b = NULL;
    for (cl = ctx->out; cl; cl = cl->next) {
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "replace out: %p %p", cl->buf, cl->buf->pos);
        if (cl->buf == b) {
            ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                          "the same buf was used in sub");
            ngx_debug_point();
            return NGX_ERROR;
        }
        b = cl->buf;
    }

    if (ctx->out == NULL) {
        dd("empty ctx->out");
        if (ctx->last_out != &ctx->out) {
            dd("BAD ctx->last_out");
            return NGX_ERROR;
        }
    }

    for (cl = ctx->out; cl; cl = cl->next) {
        dd("ctx->out buf: \"%.*s\" last_buf=%d, sync=%d, flush=%d",
           (int) ngx_buf_size(cl->buf), cl->buf->pos, cl->buf->last_buf,
           cl->buf->sync, cl->buf->flush);

        if (cl->next == NULL && ctx->last_out != &cl->next) {
            dd("BAD ctx->last_out");
            return NGX_ERROR;
        }
    }
#endif

    rc = ngx_http_next_body_filter(r, ctx->out);

    if (ctx->busy == NULL) {
        ctx->busy = ctx->out;

    } else {
        for (cl = ctx->busy; cl->next; cl = cl->next) { /* void */ }
        cl->next = ctx->out;
    }

    ctx->out = NULL;
    ctx->last_out = &ctx->out;

    while (ctx->busy) {

        cl = ctx->busy;
        b = cl->buf;

        if (ngx_buf_size(b) != 0) {
            break;
        }

        if (b->shadow) {
            b->shadow->pos = b->shadow->last;
        }

        ctx->busy = cl->next;

        if (ngx_buf_special(b)) {

            /* collect special bufs to ctx->special, which may still be busy */

            cl->next = NULL;
            *ctx->last_special = cl;
            ctx->last_special = &cl->next;

        } else {

            /* add ctx->special to ctx->free because they cannot be busy at
             * this point */

            *ctx->last_special = ctx->free;
            ctx->free = ctx->special;
            ctx->special = NULL;
            ctx->last_special = &ctx->special;

            /* add the data buf itself to the free buf chain */

            cl->next = ctx->free;
            ctx->free = cl;
        }
    }

    if (ctx->in || ctx->buf) {
        r->buffered |= NGX_HTTP_SUB_BUFFERED;

    } else {
        r->buffered &= ~NGX_HTTP_SUB_BUFFERED;
    }

    return rc;
}


static ngx_int_t
ngx_http_replace_parse(ngx_http_request_t *r, ngx_http_replace_ctx_t *ctx)
{
    int                    rc, from, to;
    size_t                 len;
    ngx_buf_t             *b;
    ngx_chain_t           *cl, *newcl, *pending, **ll;

    if (ctx->once || ctx->vm_done) {
        ctx->copy_start = ctx->pos;
        ctx->copy_end = ctx->buf->last;
        ctx->pos = ctx->buf->last;

        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "once");

        return NGX_AGAIN;
    }

    len = ctx->buf->last - ctx->pos;

    dd("=== process data chunk pos=%u, special=%u, last=%u, \"%.*s\"",
       (int) (ctx->pos - ctx->buf->pos + ctx->stream_pos),
       ctx->special_buf, ctx->last_buf,
       (int) len, ctx->pos);

    rc = sre_vm_pike_exec(ctx->vm_ctx, ctx->pos, len, ctx->last_buf);

    dd("vm pike exec: %d", rc);

    switch (rc) {
    case SRE_OK:
        from = ctx->ovector[0];
        to = ctx->ovector[1];

        dd("pike vm ok: (%d, %d)", from, to);

        if (from == to) {
            dd("empty $& capture");

            if (ctx->pending) {
                dd("output pending");
                *ctx->last_out = ctx->pending;
                ctx->last_out = ctx->last_pending;

                ctx->pending = NULL;
                ctx->last_pending = &ctx->pending;
            }

            ctx->copy_start = ctx->pos;
            ctx->copy_end = ctx->buf->pos + (from - ctx->stream_pos);

            ctx->pos = ctx->buf->pos + (to - ctx->stream_pos);

            return NGX_OK;
        }

        dd("non-empty $& capture");

        if (to < ctx->stream_pos) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "general look-ahead not supported: %d < %d",
                          to, ctx->stream_pos);
            return NGX_ERROR;
        }

        if (from >= ctx->stream_pos) {

            if (ctx->pending) {
                *ctx->last_out = ctx->pending;
                ctx->last_out = ctx->last_pending;

                ctx->pending = NULL;
                ctx->last_pending = &ctx->pending;
            }


            ctx->copy_start = ctx->pos;
            ctx->copy_end = ctx->buf->pos + (from - ctx->stream_pos);

            dd("copy len: %d", (int) (ctx->copy_end - ctx->copy_start));
            goto done;
        }

        ll = &ctx->pending;
        for (cl = ctx->pending; cl; ll = &cl->next, cl = cl->next) {
            dd("checking buf %d-%d against capture (%d, %d)",
               (int) cl->buf->file_pos, (int) cl->buf->file_last,
               from, to);

            if (cl->buf->file_pos < to && cl->buf->file_last > from) {
                dd("buf \"%.*s\" overlapped with the capture",
                   (int) ngx_buf_size(cl->buf), cl->buf->pos);

                if (cl->buf->file_pos < from) {
                    dd("discard all the pending data after \"from\" "
                       "because %d < %d", (int) cl->buf->file_pos, from);

                    cl->buf->last -= cl->buf->file_last - from;
                    cl->buf->file_last = from;

                    if (cl->next) {
                        cl->next->next = ctx->free;
                        ctx->free = cl->next;
                        cl->next = NULL;
                    }

                    ctx->last_pending = &cl->next;
                    break;
                }

                dd("discard cl and its following chains, to=%d, stream_pos=%d",
                   to, ctx->stream_pos);

                *ll = NULL;
                cl->next = ctx->free;
                ctx->free = cl;
                ctx->last_pending = ll;
                break;
            }

            dd("skipped pending buf \"%.*s\"",
               (int) ngx_buf_size(cl->buf), cl->buf->pos);
        }

        if (ctx->pending) {
            *ctx->last_out = ctx->pending;
            ctx->last_out = ctx->last_pending;
        }

        ctx->pending = NULL;
        ctx->last_pending = &ctx->pending;

        ctx->copy_start = NULL;
        ctx->copy_end = NULL;

done:
        ctx->pos = ctx->buf->pos + (to - ctx->stream_pos);

        return NGX_OK;

    case SRE_AGAIN:
        from = ctx->ovector[0];
        to = ctx->ovector[1];

        dd("pike vm again: (%d, %d)", from, to);

        if (from == -1) {
            return NGX_ERROR;
        }

        if (to == -1) {
            to = ctx->stream_pos + (ctx->buf->last - ctx->buf->pos);
        }

        dd("pike vm again (adjusted): stream pos:%d, (%d, %d)", ctx->stream_pos,
           from, to);

        if (to < from) {
            return NGX_ERROR;
        }

        if (from == to) {
            dd("even partial match does not exist");
            ctx->copy_start = ctx->pos;
            ctx->copy_end = ctx->buf->pos + (from - ctx->stream_pos);
            ctx->pos = ctx->copy_end;
            return NGX_AGAIN;
        }

        /*
         * append the existing ctx->pending data right before
         * the $& capture to ctx->out.
         */

        if (from >= ctx->stream_pos) {
            ctx->copy_start = ctx->pos;
            ctx->copy_end = ctx->buf->pos + (from - ctx->stream_pos);

            if (ctx->pending) {
                *ctx->last_out = ctx->pending;
                ctx->last_out = ctx->last_pending;

                ctx->pending = NULL;
                ctx->last_pending = &ctx->pending;
            }

            ctx->pending = ngx_http_replace_get_free_buf(r->pool, &ctx->free);
            if (ctx->pending == NULL) {
                return NGX_ERROR;
            }

            ctx->last_pending = &ctx->pending->next;

            b = ctx->pending->buf;

            b->temporary = 1;

            /* abuse the file_pos and file_last fields here */
            b->file_pos = from;
            b->file_last = to;

            b->pos = ngx_palloc(r->pool, to - from);
            if (b->pos == NULL) {
                return NGX_ERROR;
            }

            b->last = ngx_copy(b->pos, ctx->copy_end, to - from);

            dd("saved new pending data: \"%.*s\"", to - from, b->pos);

            ctx->pos = ctx->buf->last;
            return NGX_AGAIN;
        }

        dd("from < ctx->stream_pos");

        pending = NULL;
        ll = &ctx->pending;
        for (cl = ctx->pending; cl; ll = &cl->next, cl = cl->next) {
            if (cl->buf->file_pos < to && cl->buf->file_last > from) {
                dd("overlapped with the capture");

                if (cl->buf->file_pos < from) {
                    dd("split the buf, and the new \"pending\" chain starts "
                       "from the last half");

                    newcl = ngx_http_replace_get_free_buf(r->pool, &ctx->free);
                    if (newcl == NULL) {
                        return NGX_ERROR;
                    }

                    b = newcl->buf;

                    b->memory = 1;
                    b->pos = cl->buf->pos + (from - cl->buf->file_pos);
                    b->last = cl->buf->last;
                    b->file_pos = from;
                    b->file_last = cl->buf->file_last;

                    cl->buf->last = cl->buf->pos + (from - cl->buf->file_pos);
                    cl->buf->file_last = from;

                    pending = newcl;

                    if (cl->next) {
                        pending->next = cl->next;
                        cl->next = NULL;

                    } else {
                        ctx->last_pending = &pending->next;
                    }

                    ll = &cl->next;
                    break;
                }

                /* cl and its following chains form the beginning of
                 * the new "pending" chain */

                *ll = NULL;
                pending = cl;
                break;
            }
        }

        dd("pending: %p", pending);

        if (ctx->pending) {
            *ctx->last_out = ctx->pending;
            ctx->last_out = ll;
        }

        ctx->pending = pending;

        newcl = ngx_http_replace_get_free_buf(r->pool, &ctx->free);
        if (newcl == NULL) {
            return NGX_ERROR;
        }

        *ctx->last_pending = newcl;
        ctx->last_pending = &newcl->next;

        b = newcl->buf;

        b->temporary = 1;

        /* abuse the file_pos and file_last fields here */
        b->file_pos = ctx->pos - ctx->buf->pos + ctx->stream_pos;
        b->file_last = to;

        b->pos = ngx_palloc(r->pool, len);
        if (b->pos == NULL) {
            return NGX_ERROR;
        }

        b->last = ngx_copy(b->pos, ctx->pos, len);

        ctx->copy_start = NULL;
        ctx->copy_end = NULL;

        ctx->pos = ctx->buf->last;
        return NGX_AGAIN;

    case SRE_DECLINED:
        return NGX_DECLINED;

    default:
        /* SRE_ERROR */
        return NGX_ERROR;
    }

    /* cannot reach here */
}


static char *
ngx_http_replace_filter(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_replace_loc_conf_t *slcf = conf;

    int              flags = 0;
    u_char          *p;
    ngx_str_t       *value;
    ngx_uint_t       i;
    sre_pool_t      *ppool; /* parser pool */
    sre_pool_t      *cpool; /* compiler pool */
    sre_regex_t     *re;
    sre_program_t   *prog;

    ngx_pool_cleanup_t            *cln;

    if (slcf->match.len) {
        return "is duplicate";
    }

    value = cf->args->elts;

    slcf->match = value[1];

    /* XXX check variable usage in the value */
    slcf->value = value[2];

    slcf->once = 1;  /* default to once */

    if (cf->args->nelts == 4) {
        /* 3 user args */

        p = value[3].data;

        for (i = 0; i < value[3].len; i++) {
            switch (p[i]) {
            case 'i':
                flags |= SRE_REGEX_CASELESS;
                break;

            case 'g':
                slcf->once = 0;
                break;

            default:
                return "specifies an unrecognized regex flag";
            }
        }
    }

    ppool = sre_create_pool(1024);
    if (ppool == NULL) {
        return NGX_CONF_ERROR;
    }

    dd("regex: %s", value[1].data);

    re = sre_regex_parse(ppool, value[1].data, &slcf->ncaps, flags);
    if (re == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "failed to parse regex \"%V\"", &slcf->match);

        sre_destroy_pool(ppool);
        return NGX_CONF_ERROR;
    }

    cpool = sre_create_pool(1024);
    if (cpool == NULL) {
        sre_destroy_pool(ppool);
        return NGX_CONF_ERROR;
    }

    cln = ngx_pool_cleanup_add(cf->pool, 0);
    if (cln == NULL) {
        sre_destroy_pool(ppool);
        sre_destroy_pool(cpool);
        return NGX_CONF_ERROR;
    }

    cln->data = cpool;
    cln->handler = ngx_http_replace_cleanup_pool;

    prog = sre_regex_compile(cpool, re);

    sre_destroy_pool(ppool);

    if (prog == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "failed to compile regex \"%V\"", &slcf->match);
        return NGX_CONF_ERROR;
    }

    slcf->program = prog;
    slcf->ovecsize = 2 * (slcf->ncaps + 1) * sizeof(int);

    /* TODO register a pool cleanup handler to destroy ppool */

    return NGX_CONF_OK;
}


static void *
ngx_http_replace_create_conf(ngx_conf_t *cf)
{
    ngx_http_replace_loc_conf_t  *slcf;

    slcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_replace_loc_conf_t));
    if (slcf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->match = { 0, NULL };
     *     conf->value = { 0, NULL };
     *     conf->types = { NULL };
     *     conf->types_keys = NULL;
     */

    slcf->once = NGX_CONF_UNSET;

    return slcf;
}


static char *
ngx_http_replace_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_replace_loc_conf_t *prev = parent;
    ngx_http_replace_loc_conf_t *conf = child;

    ngx_conf_merge_value(conf->once, prev->once, 1);
    ngx_conf_merge_str_value(conf->match, prev->match, "");
    ngx_conf_merge_str_value(conf->value, prev->value, "");

    if (ngx_http_merge_types(cf, &conf->types_keys, &conf->types,
                             &prev->types_keys, &prev->types,
                             ngx_http_html_default_types)
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_replace_filter_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_replace_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_replace_body_filter;

    return NGX_OK;
}


static ngx_chain_t *
ngx_http_replace_get_free_buf(ngx_pool_t *p, ngx_chain_t **free)
{
    ngx_chain_t     *cl;

    cl = ngx_chain_get_free_buf(p, free);
    if (cl == NULL) {
        return cl;
    }

    ngx_memzero(cl->buf, sizeof(ngx_buf_t));
    return cl;
}