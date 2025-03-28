/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2015-2022 The Fluent Bit Authors
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_filter.h>
#include <fluent-bit/flb_filter_plugin.h>
#include <fluent-bit/flb_config.h>
#include <fluent-bit/flb_error.h>
#include <fluent-bit/flb_utils.h>
#include <fluent-bit/flb_pack.h>
#include <fluent-bit/flb_parser.h>
#include <fluent-bit/flb_kv.h>
#include <msgpack.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filter_wasm.h"

/* cb_filter callback */
static int cb_wasm_filter(const void *data, size_t bytes,
                          const char *tag, int tag_len,
                          void **out_buf, size_t *out_bytes,
                          struct flb_filter_instance *f_ins,
                          struct flb_input_instance *i_ins,
                          void *filter_context,
                          struct flb_config *config)
{
    int ret;
    char *ret_val = NULL;
    char *buf = NULL;
    (void) f_ins;
    (void) i_ins;
    (void) config;

    size_t off = 0;
    size_t last_off = 0;
    size_t alloc_size = 0;
    char *json_buf = NULL;
    size_t json_size;
    int root_type;
    struct flb_wasm *wasm = NULL;

    msgpack_object *p;
    msgpack_unpacked result;
    msgpack_object root;
    msgpack_sbuffer tmp_sbuf;
    msgpack_packer tmp_pck;

    struct flb_time t;
    struct flb_filter_wasm *ctx = filter_context;

    wasm = flb_wasm_instantiate(config, ctx->wasm_path, ctx->accessible_dir_list, -1, -1, -1);
    if (wasm == NULL) {
        flb_plg_debug(ctx->ins, "instantiate wasm [%s] failed", ctx->wasm_path);
        goto on_error;
    }
    ctx->wasm = wasm;

    /* Create temporary msgpack buffer */
    msgpack_sbuffer_init(&tmp_sbuf);
    msgpack_packer_init(&tmp_pck, &tmp_sbuf, msgpack_sbuffer_write);

    msgpack_unpacked_init(&result);
    while (msgpack_unpack_next(&result, data, bytes, &off) == MSGPACK_UNPACK_SUCCESS) {
        root = result.data;
        if (root.type != MSGPACK_OBJECT_ARRAY) {
            continue;
        }

        alloc_size = (off - last_off) + 128; /* JSON is larger than msgpack */
        last_off = off;

        /* Get timestamp */
        flb_time_pop_from_msgpack(&t, &result, &p);

        /* Encode as JSON from msgpack */
        buf = flb_msgpack_to_json_str(alloc_size, p);

        if (buf) {
            /* Execute WASM program */
            ret_val = flb_wasm_call_function_format_json(ctx->wasm, ctx->wasm_function_name,
                                                         tag, tag_len,
                                                         t,
                                                         buf, strlen(buf));

            flb_free(buf);
        }
        else {
            flb_plg_error(ctx->ins, "encode as JSON from msgpack is failed");
            msgpack_sbuffer_destroy(&tmp_sbuf);
            msgpack_unpacked_destroy(&result);

            goto on_error;
        }

        if (ret_val == NULL) { /* Skip record */
            flb_plg_debug(ctx->ins, "encode as JSON from msgpack is broken. Skip.");
            continue;
        }

        /* main array */
        msgpack_pack_array(&tmp_pck, 2);

        /* repack original flb_time */
        flb_time_append_to_msgpack(&t, &tmp_pck, 0);

        /* Convert JSON payload to msgpack */
        ret = flb_pack_json(ret_val, strlen(ret_val),
                            &json_buf, &json_size, &root_type);

        if (ret == 0 && root_type == JSMN_OBJECT) {
            /* JSON found, pack it msgpack representation */
            msgpack_sbuffer_write(&tmp_sbuf, json_buf, json_size);
        }
        else {
            flb_plg_error(ctx->ins, "invalid JSON format. ret: %d, buf: %s", ret, ret_val);
        }

        /* release 'ret_val' if it was allocated */
        if (ret_val != NULL) {
            flb_free(ret_val);
        }

        /* release 'json_buf' if it was allocated */
        if (json_buf != NULL) {
            flb_free(json_buf);
        }
    }

    msgpack_unpacked_destroy(&result);

    /* Teardown WASM context */
    if (ctx->wasm != NULL) {
        flb_wasm_destroy(ctx->wasm);
    }

    /* link new buffers */
    *out_buf   = tmp_sbuf.data;
    *out_bytes = tmp_sbuf.size;

    return FLB_FILTER_MODIFIED;

on_error:
    if (ctx->wasm != NULL) {
        flb_wasm_destroy(ctx->wasm);
    }

    return FLB_FILTER_NOTOUCH;
}

/* read config file and*/
static int filter_wasm_config_read(struct flb_filter_wasm *ctx,
                                   struct flb_filter_instance *f_ins,
                                   struct flb_config *config)
{
    int ret;

    ctx->ins = f_ins;

    /* Load the config map */
    ret = flb_filter_config_map_set(f_ins, (void *)ctx);
    if (ret == -1) {
        flb_plg_error(f_ins, "unable to load configuration");
        return -1;
    }

    /* filepath setting */
    if (ctx->wasm_path == NULL) {
        flb_plg_error(f_ins, "no WASM 'program path' was given");
        return -1;
    }

    /* function_name setting */
    if (ctx->wasm_function_name == NULL) {
        flb_plg_error(f_ins, "no WASM 'function name' was given");
        return -1;
    }

    return 0;
}

static void delete_wasm_config(struct flb_filter_wasm *ctx)
{
    if (!ctx) {
        return;
    }

    flb_free(ctx);
}

/* Initialize plugin */
static int cb_wasm_init(struct flb_filter_instance *f_ins,
                        struct flb_config *config, void *data)
{
    struct flb_filter_wasm *ctx = NULL;
    int ret = -1;

    /* Allocate space for the configuration */
    ctx = flb_calloc(1, sizeof(struct flb_filter_wasm));
    if (!ctx) {
        return -1;
    }

    /* Initialize exec config */
    ret = filter_wasm_config_read(ctx, f_ins, config);
    if (ret < 0) {
        goto init_error;
    }

    flb_wasm_init(config);

    /* Set context */
    flb_filter_set_context(f_ins, ctx);
    return 0;

init_error:
    delete_wasm_config(ctx);

    return -1;
}

static int cb_wasm_exit(void *data, struct flb_config *config)
{
    struct flb_filter_wasm *ctx = data;

    flb_wasm_destroy_all(config);
    delete_wasm_config(ctx);
    return 0;
}

static struct flb_config_map config_map[] = {
    {
     FLB_CONFIG_MAP_STR, "wasm_path", NULL,
     0, FLB_TRUE, offsetof(struct flb_filter_wasm, wasm_path),
     "Set the wasm path to execute"
    },
    {
     FLB_CONFIG_MAP_CLIST, "accessible_paths", ".",
     0, FLB_TRUE, offsetof(struct flb_filter_wasm, accessible_dir_list),
     "Specifying paths to be accessible from a WASM program."
     "Default value is current working directory"
    },
    {
     FLB_CONFIG_MAP_STR, "function_name", NULL,
     0, FLB_TRUE, offsetof(struct flb_filter_wasm, wasm_function_name),
     "Set the function name in wasm to execute"
    },
    /* EOF */
    {0}
};

struct flb_filter_plugin filter_wasm_plugin = {
    .name         = "wasm",
    .description  = "WASM program filter",
    .cb_init      = cb_wasm_init,
    .cb_filter    = cb_wasm_filter,
    .cb_exit      = cb_wasm_exit,
    .config_map   = config_map
};
