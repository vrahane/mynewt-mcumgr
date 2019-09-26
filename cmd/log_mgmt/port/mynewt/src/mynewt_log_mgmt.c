/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "log/log.h"
#include "mgmt/mgmt.h"
#include "log_mgmt/log_mgmt.h"
#include "log_mgmt/log_mgmt_impl.h"
#include "../../../src/log_mgmt_config.h"

struct mynewt_log_mgmt_walk_arg {
    log_mgmt_foreach_entry_fn *cb;
    uint8_t body[LOG_MGMT_BODY_LEN];
    void *arg;
};

static struct log *
mynewt_log_mgmt_find_log(const char *log_name)
{
    struct log *log;

    log = NULL;
    while (1) {
        log = log_list_get_next(log);
        if (log == NULL) {
            return NULL;
        }

        if (strcmp(log->l_name, log_name) == 0) {
            return log;
        }
    }
}

int
log_mgmt_impl_get_log(int idx, struct log_mgmt_log *out_log)
{
    struct log *log;
    int i;

    log = NULL;
    for (i = 0; i <= idx; i++) {
        log = log_list_get_next(log);
        if (log == NULL) {
            return MGMT_ERR_ENOENT;
        }
    }

    out_log->name = log->l_name;
    out_log->type = log->l_log->log_type;
    return 0;
}

int
log_mgmt_impl_get_module(int idx, const char **out_module_name)
{
    const char *name;

    name = LOG_MODULE_STR(idx);
    if (name == NULL) {
        return MGMT_ERR_ENOENT;
    } else {
        *out_module_name = name;
        return 0;
    }
}

int
log_mgmt_impl_get_level(int idx, const char **out_level_name)
{
    const char *name;

    name = LOG_LEVEL_STR(idx);
    if (name == NULL) {
        return MGMT_ERR_ENOENT;
    } else {
        *out_level_name = name;
        return 0;
    }
}

int
log_mgmt_impl_get_next_idx(uint32_t *out_idx)
{
    *out_idx = g_log_info.li_next_index;
    return 0;
}

static int
mynewt_log_mgmt_walk_cb(struct log *log, struct log_offset *log_offset,
                        const struct log_entry_hdr *leh,
                        const void *dptr, uint16_t len)
{
    struct mynewt_log_mgmt_walk_arg *mynewt_log_mgmt_walk_arg;
    struct log_mgmt_entry entry;
    int header_len;
    int read_len;
    int rc;

    mynewt_log_mgmt_walk_arg = log_offset->lo_arg;

    /* If specified timestamp is nonzero, it is the primary criterion, and the
     * specified index is the secondary criterion.  If specified timetsamp is
     * zero, specified index is the only criterion.
     *
     * If specified timestamp == 0: encode entries whose index >=
     *     specified index.
     * Else: encode entries whose timestamp >= specified timestamp and whose
     *      index >= specified index
     */
    if (log_offset->lo_ts == 0) {
        if (log_offset->lo_index > leh->ue_index) {
            return 0;
        }
    } else if (leh->ue_ts < log_offset->lo_ts   ||
               (leh->ue_ts == log_offset->lo_ts &&
                leh->ue_index < log_offset->lo_index)) {
        return 0;
    }

    entry.ts = leh->ue_ts;
    entry.index = leh->ue_index;
    entry.module = leh->ue_module;
    entry.level = leh->ue_level;

#if MYNEWT_VAL(LOG_VERSION) < 3
    entry.type = LOG_ETYPE_STRING;
    entry.flags = 0;
    header_len = sizeof leh;
    read_len = min(len - header_len, LOG_MGMT_BODY_LEN - header_len);
#else
    entry.type = leh->ue_etype;
    entry.flags = leh->ue_flags;
    entry.imghash = (leh->ue_flags & LOG_FLAGS_IMG_HASH) ?
        leh->ue_imghash : NULL;
    header_len = log_hdr_len(leh);
    read_len = LOG_MGMT_BODY_LEN - header_len;
#endif
    rc = log_read(log, dptr, mynewt_log_mgmt_walk_arg->body, header_len,
                  read_len);
    if (rc < 0) {
        return MGMT_ERR_EUNKNOWN;
    }

    entry.len = rc;
    entry.data = mynewt_log_mgmt_walk_arg->body;
    return mynewt_log_mgmt_walk_arg->cb(&entry, mynewt_log_mgmt_walk_arg->arg);
}

int
log_mgmt_impl_foreach_entry(const char *log_name,
                            const struct log_mgmt_filter *filter,
                            log_mgmt_foreach_entry_fn *cb, void *arg)
{
    struct mynewt_log_mgmt_walk_arg walk_arg;
    struct log_offset offset;
    struct log *log;

    walk_arg = (struct mynewt_log_mgmt_walk_arg) {
        .cb = cb,
        .arg = arg,
    };

    log = mynewt_log_mgmt_find_log(log_name);
    if (log == NULL) {
        return MGMT_ERR_ENOENT;
    }

    if (strcmp(log->l_name, log_name) == 0) {
        offset.lo_arg = &walk_arg;
        offset.lo_ts = filter->min_timestamp;
        offset.lo_index = filter->min_index;
        offset.lo_data_len = 0;

        return log_walk_body(log, mynewt_log_mgmt_walk_cb, &offset);
    }

    return MGMT_ERR_ENOENT;
}

int
log_mgmt_impl_clear(const char *log_name)
{
    struct log *log;
    int rc;

    log = mynewt_log_mgmt_find_log(log_name);
    if (log == NULL) {
        return MGMT_ERR_ENOENT;
    }

    rc = log_flush(log);
    if (rc != 0) {
        return MGMT_ERR_EUNKNOWN;
    }

    return 0;
}

void
log_mgmt_module_init(void)
{
    /* Ensure this function only gets called by sysinit. */
    SYSINIT_ASSERT_ACTIVE();

    log_mgmt_register_group();
}
