
/*
 * Copyright (C) 2022 Web Server LLC
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


#define NGX_HTTP_LIMIT_CONN_PASSED            1
#define NGX_HTTP_LIMIT_CONN_REJECTED          2
#define NGX_HTTP_LIMIT_CONN_REJECTED_DRY_RUN  3


typedef struct {
    u_char                        color;
    u_char                        len;
    u_short                       conn;
    u_char                        data[1];
} ngx_http_limit_conn_node_t;


typedef struct {
    ngx_shm_zone_t               *shm_zone;
    ngx_rbtree_node_t            *node;
} ngx_http_limit_conn_cleanup_t;


#if (NGX_API)

typedef struct {
    ngx_atomic_t                  passed;
    ngx_atomic_t                  skipped;
    ngx_atomic_t                  rejected;
    ngx_atomic_t                  exhausted;
} ngx_http_limit_conn_stats_t;

#endif


typedef struct {
    ngx_rbtree_t                  rbtree;
    ngx_rbtree_node_t             sentinel;
#if (NGX_API)
    ngx_http_limit_conn_stats_t   stats;
#endif
} ngx_http_limit_conn_shctx_t;


typedef struct ngx_http_limit_conn_ctx_s  ngx_http_limit_conn_ctx_t;

struct ngx_http_limit_conn_ctx_s {
    ngx_shm_zone_t               *shm_zone;
    ngx_http_limit_conn_shctx_t  *sh;
    ngx_slab_pool_t              *shpool;
    ngx_http_complex_value_t      key;
#if (NGX_API)
    ngx_uint_t                    passed;  /* unsigned  passed:1; */
#endif
    ngx_http_limit_conn_ctx_t    *next;
};


typedef struct {
    ngx_shm_zone_t               *shm_zone;
    ngx_uint_t                    conn;
} ngx_http_limit_conn_limit_t;


typedef struct {
    ngx_http_limit_conn_ctx_t    *limit_conns;
    ngx_http_limit_conn_ctx_t   **limit_conns_next_p;
} ngx_http_limit_conn_main_conf_t;


typedef struct {
    ngx_array_t                   limits;
    ngx_uint_t                    log_level;
    ngx_uint_t                    status_code;
    ngx_flag_t                    dry_run;
} ngx_http_limit_conn_loc_conf_t;


static ngx_rbtree_node_t *ngx_http_limit_conn_lookup(ngx_rbtree_t *rbtree,
    ngx_str_t *key, uint32_t hash);
static void ngx_http_limit_conn_cleanup(void *data);
static ngx_inline void ngx_http_limit_conn_cleanup_all(ngx_pool_t *pool);

static ngx_int_t ngx_http_limit_conn_status_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static void *ngx_http_limit_conn_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_limit_conn_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_limit_conn_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_http_limit_conn_zone(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_limit_conn(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_http_limit_conn_add_variables(ngx_conf_t *cf);
static ngx_int_t ngx_http_limit_conn_init(ngx_conf_t *cf);


static ngx_conf_enum_t  ngx_http_limit_conn_log_levels[] = {
    { ngx_string("info"), NGX_LOG_INFO },
    { ngx_string("notice"), NGX_LOG_NOTICE },
    { ngx_string("warn"), NGX_LOG_WARN },
    { ngx_string("error"), NGX_LOG_ERR },
    { ngx_null_string, 0 }
};


static ngx_conf_num_bounds_t  ngx_http_limit_conn_status_bounds = {
    ngx_conf_check_num_bounds, 400, 599
};


static ngx_command_t  ngx_http_limit_conn_commands[] = {

    { ngx_string("limit_conn_zone"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE2,
      ngx_http_limit_conn_zone,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("limit_conn"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_http_limit_conn,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("limit_conn_log_level"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_conn_loc_conf_t, log_level),
      &ngx_http_limit_conn_log_levels },

    { ngx_string("limit_conn_status"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_conn_loc_conf_t, status_code),
      &ngx_http_limit_conn_status_bounds },

    { ngx_string("limit_conn_dry_run"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_conn_loc_conf_t, dry_run),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_limit_conn_module_ctx = {
    ngx_http_limit_conn_add_variables,     /* preconfiguration */
    ngx_http_limit_conn_init,              /* postconfiguration */

    ngx_http_limit_conn_create_main_conf,  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_limit_conn_create_loc_conf,   /* create location configuration */
    ngx_http_limit_conn_merge_loc_conf     /* merge location configuration */
};


ngx_module_t  ngx_http_limit_conn_module = {
    NGX_MODULE_V1,
    &ngx_http_limit_conn_module_ctx,       /* module context */
    ngx_http_limit_conn_commands,          /* module directives */
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


static ngx_http_variable_t  ngx_http_limit_conn_vars[] = {

    { ngx_string("limit_conn_status"), NULL,
      ngx_http_limit_conn_status_variable, 0, NGX_HTTP_VAR_NOCACHEABLE, 0 },

      ngx_http_null_variable
};


static ngx_str_t  ngx_http_limit_conn_status[] = {
    ngx_string("PASSED"),
    ngx_string("REJECTED"),
    ngx_string("REJECTED_DRY_RUN")
};


#if (NGX_API)

static ngx_int_t ngx_api_http_limit_conns_handler(ngx_api_entry_data_t data,
    ngx_api_ctx_t *actx, void *ctx);
static ngx_int_t ngx_api_http_limit_conns_iter(ngx_api_iter_ctx_t *ictx,
    ngx_api_ctx_t *actx);


static ngx_api_entry_t  ngx_api_http_limit_conn_entries[] = {

    {
        .name      = ngx_string("passed"),
        .handler   = ngx_api_struct_atomic_handler,
        .data.off  = offsetof(ngx_http_limit_conn_stats_t, passed)
    },

    {
        .name      = ngx_string("skipped"),
        .handler   = ngx_api_struct_atomic_handler,
        .data.off  = offsetof(ngx_http_limit_conn_stats_t, skipped)
    },

    {
        .name      = ngx_string("rejected"),
        .handler   = ngx_api_struct_atomic_handler,
        .data.off  = offsetof(ngx_http_limit_conn_stats_t, rejected)
    },

    {
        .name      = ngx_string("exhausted"),
        .handler   = ngx_api_struct_atomic_handler,
        .data.off  = offsetof(ngx_http_limit_conn_stats_t, exhausted)
    },

    ngx_api_null_entry
};


static ngx_api_entry_t  ngx_api_http_limit_conns_entry = {
    .name      = ngx_string("limit_conns"),
    .handler   = ngx_api_http_limit_conns_handler,
};

#endif


static ngx_int_t
ngx_http_limit_conn_handler(ngx_http_request_t *r)
{
    size_t                           n;
    uint32_t                         hash;
    ngx_str_t                        key;
    ngx_uint_t                       i;
    ngx_rbtree_node_t               *node;
    ngx_pool_cleanup_t              *cln;
    ngx_http_limit_conn_ctx_t       *ctx;
    ngx_http_limit_conn_node_t      *lc;
    ngx_http_limit_conn_limit_t     *limits;
    ngx_http_limit_conn_cleanup_t   *lccln;
    ngx_http_limit_conn_loc_conf_t  *lclcf;

    if (r->main->limit_conn_status) {
        return NGX_DECLINED;
    }

    lclcf = ngx_http_get_module_loc_conf(r, ngx_http_limit_conn_module);
    limits = lclcf->limits.elts;

    for (i = 0; i < lclcf->limits.nelts; i++) {
        ctx = limits[i].shm_zone->data;

        if (ngx_http_complex_value(r, &ctx->key, &key) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

#if (NGX_API)
        ctx->passed = 0;
#endif

        if (key.len == 0) {
            continue;
        }

        if (key.len > 255) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "the value of the \"%V\" key "
                          "is more than 255 bytes: \"%V\"",
                          &ctx->key.value, &key);
            continue;
        }

        r->main->limit_conn_status = NGX_HTTP_LIMIT_CONN_PASSED;

        hash = ngx_crc32_short(key.data, key.len);

        ngx_shmtx_lock(&ctx->shpool->mutex);

        node = ngx_http_limit_conn_lookup(&ctx->sh->rbtree, &key, hash);

        if (node == NULL) {

            n = offsetof(ngx_rbtree_node_t, color)
                + offsetof(ngx_http_limit_conn_node_t, data)
                + key.len;

            node = ngx_slab_alloc_locked(ctx->shpool, n);

            if (node == NULL) {
                ngx_shmtx_unlock(&ctx->shpool->mutex);
#if (NGX_API)
                (void) ngx_atomic_fetch_add(&ctx->sh->stats.exhausted, 1);
#endif
                goto reject;
            }

            lc = (ngx_http_limit_conn_node_t *) &node->color;

            node->key = hash;
            lc->len = (u_char) key.len;
            lc->conn = 1;
            ngx_memcpy(lc->data, key.data, key.len);

            ngx_rbtree_insert(&ctx->sh->rbtree, node);

        } else {

            lc = (ngx_http_limit_conn_node_t *) &node->color;

            if ((ngx_uint_t) lc->conn >= limits[i].conn) {

                ngx_shmtx_unlock(&ctx->shpool->mutex);

                ngx_log_error(lclcf->log_level, r->connection->log, 0,
                              "limiting connections%s by zone \"%V\"",
                              lclcf->dry_run ? ", dry run," : "",
                              &limits[i].shm_zone->shm.name);
#if (NGX_API)
                (void) ngx_atomic_fetch_add(&ctx->sh->stats.rejected, 1);
#endif
                goto reject;
            }

            lc->conn++;
        }

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "limit conn: %08Xi %d", node->key, lc->conn);

        ngx_shmtx_unlock(&ctx->shpool->mutex);

        cln = ngx_pool_cleanup_add(r->pool,
                                   sizeof(ngx_http_limit_conn_cleanup_t));
        if (cln == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        cln->handler = ngx_http_limit_conn_cleanup;
        lccln = cln->data;

        lccln->shm_zone = limits[i].shm_zone;
        lccln->node = node;

#if (NGX_API)
        ctx->passed = 1;
#endif
    }

#if (NGX_API)

    for (i = 0; i < lclcf->limits.nelts; i++) {
        ctx = limits[i].shm_zone->data;

        (void) ngx_atomic_fetch_add(ctx->passed ? &ctx->sh->stats.passed
                                                : &ctx->sh->stats.skipped, 1);
    }

#endif

    return NGX_DECLINED;

reject:

    ngx_http_limit_conn_cleanup_all(r->pool);

    if (lclcf->dry_run) {
        r->main->limit_conn_status = NGX_HTTP_LIMIT_CONN_REJECTED_DRY_RUN;

        return NGX_DECLINED;
    }

    r->main->limit_conn_status = NGX_HTTP_LIMIT_CONN_REJECTED;

    return lclcf->status_code;
}


static void
ngx_http_limit_conn_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t           **p;
    ngx_http_limit_conn_node_t   *lcn, *lcnt;

    for ( ;; ) {

        if (node->key < temp->key) {

            p = &temp->left;

        } else if (node->key > temp->key) {

            p = &temp->right;

        } else { /* node->key == temp->key */

            lcn = (ngx_http_limit_conn_node_t *) &node->color;
            lcnt = (ngx_http_limit_conn_node_t *) &temp->color;

            p = (ngx_memn2cmp(lcn->data, lcnt->data, lcn->len, lcnt->len) < 0)
                ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


static ngx_rbtree_node_t *
ngx_http_limit_conn_lookup(ngx_rbtree_t *rbtree, ngx_str_t *key, uint32_t hash)
{
    ngx_int_t                    rc;
    ngx_rbtree_node_t           *node, *sentinel;
    ngx_http_limit_conn_node_t  *lcn;

    node = rbtree->root;
    sentinel = rbtree->sentinel;

    while (node != sentinel) {

        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        /* hash == node->key */

        lcn = (ngx_http_limit_conn_node_t *) &node->color;

        rc = ngx_memn2cmp(key->data, lcn->data, key->len, (size_t) lcn->len);

        if (rc == 0) {
            return node;
        }

        node = (rc < 0) ? node->left : node->right;
    }

    return NULL;
}


static void
ngx_http_limit_conn_cleanup(void *data)
{
    ngx_http_limit_conn_cleanup_t  *lccln = data;

    ngx_rbtree_node_t           *node;
    ngx_http_limit_conn_ctx_t   *ctx;
    ngx_http_limit_conn_node_t  *lc;

    ctx = lccln->shm_zone->data;
    node = lccln->node;
    lc = (ngx_http_limit_conn_node_t *) &node->color;

    ngx_shmtx_lock(&ctx->shpool->mutex);

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, lccln->shm_zone->shm.log, 0,
                   "limit conn cleanup: %08Xi %d", node->key, lc->conn);

    lc->conn--;

    if (lc->conn == 0) {
        ngx_rbtree_delete(&ctx->sh->rbtree, node);
        ngx_slab_free_locked(ctx->shpool, node);
    }

    ngx_shmtx_unlock(&ctx->shpool->mutex);
}


static ngx_inline void
ngx_http_limit_conn_cleanup_all(ngx_pool_t *pool)
{
    ngx_pool_cleanup_t  *cln;

    cln = pool->cleanup;

    while (cln && cln->handler == ngx_http_limit_conn_cleanup) {
        ngx_http_limit_conn_cleanup(cln->data);
        cln = cln->next;
    }

    pool->cleanup = cln;
}


static ngx_int_t
ngx_http_limit_conn_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_limit_conn_ctx_t  *octx = data;

    size_t                      len;
    ngx_http_limit_conn_ctx_t  *ctx;

    ctx = shm_zone->data;

    if (octx) {
        if (ctx->key.value.len != octx->key.value.len
            || ngx_strncmp(ctx->key.value.data, octx->key.value.data,
                           ctx->key.value.len)
               != 0)
        {
            ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                          "limit_conn_zone \"%V\" uses the \"%V\" key "
                          "while previously it used the \"%V\" key",
                          &shm_zone->shm.name, &ctx->key.value,
                          &octx->key.value);
            return NGX_ERROR;
        }

        ctx->sh = octx->sh;
        ctx->shpool = octx->shpool;

        return NGX_OK;
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (shm_zone->shm.exists) {
        ctx->sh = ctx->shpool->data;

        return NGX_OK;
    }

    ctx->sh = ngx_slab_alloc(ctx->shpool, sizeof(ngx_http_limit_conn_shctx_t));
    if (ctx->sh == NULL) {
        return NGX_ERROR;
    }

    ctx->shpool->data = ctx->sh;

    ngx_rbtree_init(&ctx->sh->rbtree, &ctx->sh->sentinel,
                    ngx_http_limit_conn_rbtree_insert_value);

#if (NGX_API)
    ngx_memzero(&ctx->sh->stats, sizeof(ngx_http_limit_conn_stats_t));
#endif

    len = sizeof(" in limit_conn_zone \"\"") + shm_zone->shm.name.len;

    ctx->shpool->log_ctx = ngx_slab_alloc(ctx->shpool, len);
    if (ctx->shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(ctx->shpool->log_ctx, " in limit_conn_zone \"%V\"%Z",
                &shm_zone->shm.name);

    return NGX_OK;
}


static ngx_int_t
ngx_http_limit_conn_status_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    if (r->main->limit_conn_status == 0) {
        v->not_found = 1;
        return NGX_OK;
    }

    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->len = ngx_http_limit_conn_status[r->main->limit_conn_status - 1].len;
    v->data = ngx_http_limit_conn_status[r->main->limit_conn_status - 1].data;

    return NGX_OK;
}


static void *
ngx_http_limit_conn_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_limit_conn_main_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_limit_conn_main_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->limit_conns = NULL;
     */

    conf->limit_conns_next_p = &conf->limit_conns;

    return conf;
}


static void *
ngx_http_limit_conn_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_limit_conn_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_limit_conn_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->limits.elts = NULL;
     */

    conf->log_level = NGX_CONF_UNSET_UINT;
    conf->status_code = NGX_CONF_UNSET_UINT;
    conf->dry_run = NGX_CONF_UNSET;

    return conf;
}


static char *
ngx_http_limit_conn_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_limit_conn_loc_conf_t *prev = parent;
    ngx_http_limit_conn_loc_conf_t *conf = child;

    if (conf->limits.elts == NULL) {
        conf->limits = prev->limits;
    }

    ngx_conf_merge_uint_value(conf->log_level, prev->log_level, NGX_LOG_ERR);
    ngx_conf_merge_uint_value(conf->status_code, prev->status_code,
                              NGX_HTTP_SERVICE_UNAVAILABLE);

    ngx_conf_merge_value(conf->dry_run, prev->dry_run, 0);

    return NGX_CONF_OK;
}


static char *
ngx_http_limit_conn_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_limit_conn_main_conf_t *lcmcf = conf;

    ngx_str_t                         *value;
    ngx_uint_t                         i;
    ngx_shm_zone_t                    *shm_zone;
    ngx_shm_zone_params_t              zp;
    ngx_http_limit_conn_ctx_t         *ctx;
    ngx_http_compile_complex_value_t   ccv;

    value = cf->args->elts;

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_limit_conn_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[1];
    ccv.complex_value = &ctx->key;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(&zp, sizeof(ngx_shm_zone_params_t));

    zp.min_size = 8 * ngx_pagesize;

    for (i = 2; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "zone=", 5) == 0) {

            value[i].data += 5;
            value[i].len -= 5;

            if (ngx_conf_parse_zone_spec(cf, &zp, &value[i]) != NGX_OK) {
                return NGX_CONF_ERROR;
            }

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (zp.name.len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"zone\" parameter",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    shm_zone = ngx_shared_memory_add(cf, &zp.name, zp.size,
                                     &ngx_http_limit_conn_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    if (shm_zone->data) {
        ctx = shm_zone->data;

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "%V \"%V\" is already bound to key \"%V\"",
                           &cmd->name, &zp.name, &ctx->key.value);
        return NGX_CONF_ERROR;
    }

    shm_zone->init = ngx_http_limit_conn_init_zone;
    shm_zone->data = ctx;

    ctx->shm_zone = shm_zone;

    *lcmcf->limit_conns_next_p = ctx;
    lcmcf->limit_conns_next_p = &ctx->next;

    return NGX_CONF_OK;
}


static char *
ngx_http_limit_conn(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_limit_conn_loc_conf_t *lclcf = conf;

    ngx_shm_zone_t               *shm_zone;
    ngx_http_limit_conn_limit_t  *limit, *limits;

    ngx_str_t  *value;
    ngx_int_t   n;
    ngx_uint_t  i;

    value = cf->args->elts;

    shm_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                     &ngx_http_limit_conn_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    limits = lclcf->limits.elts;

    if (limits == NULL) {
        if (ngx_array_init(&lclcf->limits, cf->pool, 1,
                           sizeof(ngx_http_limit_conn_limit_t))
            != NGX_OK)
        {
            return NGX_CONF_ERROR;
        }
    }

    for (i = 0; i < lclcf->limits.nelts; i++) {
        if (shm_zone == limits[i].shm_zone) {
            return "is duplicate";
        }
    }

    n = ngx_atoi(value[2].data, value[2].len);
    if (n <= 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid number of connections \"%V\"", &value[2]);
        return NGX_CONF_ERROR;
    }

    if (n > 65535) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "connection limit must be less 65536");
        return NGX_CONF_ERROR;
    }

    limit = ngx_array_push(&lclcf->limits);
    if (limit == NULL) {
        return NGX_CONF_ERROR;
    }

    limit->conn = n;
    limit->shm_zone = shm_zone;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_limit_conn_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var, *v;

    for (v = ngx_http_limit_conn_vars; v->name.len; v++) {
        var = ngx_http_add_variable(cf, &v->name, v->flags);
        if (var == NULL) {
            return NGX_ERROR;
        }

        var->get_handler = v->get_handler;
        var->data = v->data;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_limit_conn_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_limit_conn_handler;

#if (NGX_API)
    if (ngx_api_add(cf->cycle, "/status/http", &ngx_api_http_limit_conns_entry)
        != NGX_OK)
    {
        return NGX_ERROR;
    }
#endif

    return NGX_OK;
}


#if (NGX_API)

static ngx_int_t
ngx_api_http_limit_conns_handler(ngx_api_entry_data_t data, ngx_api_ctx_t *actx,
    void *ctx)
{
    ngx_api_iter_ctx_t                ictx;
    ngx_http_limit_conn_main_conf_t  *lcmcf;

    lcmcf = ngx_http_cycle_get_module_main_conf(ngx_cycle,
                                                ngx_http_limit_conn_module);

    ictx.entry.handler = ngx_api_object_handler;
    ictx.entry.data.ents = ngx_api_http_limit_conn_entries;
    ictx.elts = lcmcf->limit_conns;

    return ngx_api_object_iterate(ngx_api_http_limit_conns_iter, &ictx, actx);
}


static ngx_int_t
ngx_api_http_limit_conns_iter(ngx_api_iter_ctx_t *ictx, ngx_api_ctx_t *actx)
{
    ngx_http_limit_conn_ctx_t  *ctx;

    ctx = ictx->elts;

    if (ctx == NULL) {
        return NGX_DECLINED;
    }

    ictx->entry.name = ctx->shm_zone->shm.name;
    ictx->ctx = &ctx->sh->stats;
    ictx->elts = ctx->next;

    return NGX_OK;
}

#endif
