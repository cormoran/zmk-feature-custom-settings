/* SPDX-License-Identifier: MIT */
/* Convenience API boundary. The engine only sees compact views. Legacy
 * storage ownership is implemented separately in compat/value.c. */
#include <errno.h>
#include "../custom_settings_internal.h"

struct value_visitor_context {
    zmk_custom_setting_value_visitor_t visitor;
    void *user_data;
    int result;
};
static void visit_value(const struct zmk_custom_setting_value_view *view, void *context) {
    struct value_visitor_context *ctx = context;
    ctx->result = value_visit(view, ctx->visitor, ctx->user_data);
}
int zmk_custom_setting_with_value(const struct zmk_custom_setting *setting,
                                  zmk_custom_setting_value_visitor_t visitor, void *user_data) {
    if (!visitor)
        return -EINVAL;
    struct value_visitor_context ctx = {.visitor = visitor, .user_data = user_data};
    int ret = zmk_custom_setting_with_view(setting, visit_value, &ctx);
    return ret ? ret : ctx.result;
}

int zmk_custom_setting_read(const struct zmk_custom_setting *setting,
                            struct zmk_custom_setting_value *value) {
    struct zmk_custom_setting_value_view value_view =
        value ? value_output(value) : (struct zmk_custom_setting_value_view){0};
    int ret = zmk_custom_setting_read_view(setting, value ? &value_view : NULL);
    if (!ret && value)
        ret = value_finish(value, &value_view);
    return ret;
}

int zmk_custom_setting_read_default(const struct zmk_custom_setting *setting,
                                    struct zmk_custom_setting_value *value) {
    struct zmk_custom_setting_value_view value_view =
        value ? value_output(value) : (struct zmk_custom_setting_value_view){0};
    int ret = zmk_custom_setting_read_default_view(setting, value ? &value_view : NULL);
    if (!ret && value)
        ret = value_finish(value, &value_view);
    return ret;
}

int zmk_custom_setting_read_by_key(const char *custom_subsystem_id, const char *key,
                                   struct zmk_custom_setting_value *value) {
    struct zmk_custom_setting_value_view value_view =
        value ? value_output(value) : (struct zmk_custom_setting_value_view){0};
    int ret =
        zmk_custom_setting_read_by_key_view(custom_subsystem_id, key, value ? &value_view : NULL);
    if (!ret && value)
        ret = value_finish(value, &value_view);
    return ret;
}

int zmk_custom_setting_read_array_by_key(const char *custom_subsystem_id, const char *key,
                                         uint32_t index, struct zmk_custom_setting_value *value) {
    struct zmk_custom_setting_value_view value_view =
        value ? value_output(value) : (struct zmk_custom_setting_value_view){0};
    int ret = zmk_custom_setting_read_array_by_key_view(custom_subsystem_id, key, index,
                                                        value ? &value_view : NULL);
    if (!ret && value)
        ret = value_finish(value, &value_view);
    return ret;
}

int zmk_custom_setting_serialize_rpc_value(const struct zmk_custom_setting *setting,
                                           const struct zmk_custom_setting_value *internal_value,
                                           struct zmk_custom_setting_value *rpc_value) {
    if (!internal_value)
        return -EINVAL;
    int check = value_check_input(internal_value);
    if (check)
        return check;
    struct zmk_custom_setting_value_view internal_value_view = value_borrow(internal_value);
    struct zmk_custom_setting_value_view rpc_value_view =
        rpc_value ? value_output(rpc_value) : (struct zmk_custom_setting_value_view){0};
    int ret = zmk_custom_setting_serialize_rpc_value_view(
        setting, internal_value ? &internal_value_view : NULL, rpc_value ? &rpc_value_view : NULL);
    if (!ret && rpc_value)
        ret = value_finish(rpc_value, &rpc_value_view);
    return ret;
}

int zmk_custom_setting_deserialize_rpc_value(const struct zmk_custom_setting *setting,
                                             const struct zmk_custom_setting_value *rpc_value,
                                             struct zmk_custom_setting_value *internal_value) {
    if (!rpc_value)
        return -EINVAL;
    int check = value_check_input(rpc_value);
    if (check)
        return check;
    struct zmk_custom_setting_value_view rpc_value_view = value_borrow(rpc_value);
    struct zmk_custom_setting_value_view internal_value_view =
        internal_value ? value_output(internal_value) : (struct zmk_custom_setting_value_view){0};
    int ret = zmk_custom_setting_deserialize_rpc_value_view(
        setting, rpc_value ? &rpc_value_view : NULL, internal_value ? &internal_value_view : NULL);
    if (!ret && internal_value)
        ret = value_finish(internal_value, &internal_value_view);
    return ret;
}

int zmk_custom_setting_write(const struct zmk_custom_setting *setting,
                             const struct zmk_custom_setting_value *value,
                             enum zmk_custom_setting_write_mode mode) {
    if (!value)
        return -EINVAL;
    int check = value_check_input(value);
    if (check)
        return check;
    struct zmk_custom_setting_value_view value_view = value_borrow(value);
    int ret = zmk_custom_setting_write_view(setting, value ? &value_view : NULL, mode);

    return ret;
}

int zmk_custom_setting_write_by_key(const char *custom_subsystem_id, const char *key,
                                    const struct zmk_custom_setting_value *value,
                                    enum zmk_custom_setting_write_mode mode) {
    if (!value)
        return -EINVAL;
    int check = value_check_input(value);
    if (check)
        return check;
    struct zmk_custom_setting_value_view value_view = value_borrow(value);
    int ret = zmk_custom_setting_write_by_key_view(custom_subsystem_id, key,
                                                   value ? &value_view : NULL, mode);

    return ret;
}

int zmk_custom_setting_write_array_by_key(const char *custom_subsystem_id, const char *key,
                                          uint32_t index,
                                          const struct zmk_custom_setting_value *value,
                                          enum zmk_custom_setting_write_mode mode) {
    if (!value)
        return -EINVAL;
    int check = value_check_input(value);
    if (check)
        return check;
    struct zmk_custom_setting_value_view value_view = value_borrow(value);
    int ret = zmk_custom_setting_write_array_by_key_view(custom_subsystem_id, key, index,
                                                         value ? &value_view : NULL, mode);

    return ret;
}

int zmk_custom_setting_write_array_element(const struct zmk_custom_setting *setting,
                                           const struct zmk_custom_setting_value *value,
                                           uint32_t array_size,
                                           enum zmk_custom_setting_write_mode mode) {
    if (!value)
        return -EINVAL;
    int check = value_check_input(value);
    if (check)
        return check;
    struct zmk_custom_setting_value_view value_view = value_borrow(value);
    int ret = zmk_custom_setting_write_array_element_view(setting, value ? &value_view : NULL,
                                                          array_size, mode);

    return ret;
}

int zmk_custom_setting_array_push_back(const struct zmk_custom_setting *setting,
                                       const struct zmk_custom_setting_value *value,
                                       enum zmk_custom_setting_write_mode mode) {
    if (!value)
        return -EINVAL;
    int check = value_check_input(value);
    if (check)
        return check;
    struct zmk_custom_setting_value_view value_view = value_borrow(value);
    int ret = zmk_custom_setting_array_push_back_view(setting, value ? &value_view : NULL, mode);

    return ret;
}

int zmk_custom_setting_array_pop_back(const struct zmk_custom_setting *setting,
                                      struct zmk_custom_setting_value *value,
                                      enum zmk_custom_setting_write_mode mode) {
    struct zmk_custom_setting_value_view value_view =
        value ? value_output(value) : (struct zmk_custom_setting_value_view){0};
    int ret = zmk_custom_setting_array_pop_back_view(setting, value ? &value_view : NULL, mode);
    if (!ret && value)
        ret = value_finish(value, &value_view);
    return ret;
}

int zmk_custom_setting_array_insert_at(const struct zmk_custom_setting *array_setting_or_element,
                                       uint32_t index, const struct zmk_custom_setting_value *value,
                                       enum zmk_custom_setting_write_mode mode) {
    if (!value)
        return -EINVAL;
    int check = value_check_input(value);
    if (check)
        return check;
    struct zmk_custom_setting_value_view value_view = value_borrow(value);
    int ret = zmk_custom_setting_array_insert_at_view(array_setting_or_element, index,
                                                      value ? &value_view : NULL, mode);

    return ret;
}

int zmk_custom_setting_array_remove_at(const struct zmk_custom_setting *array_setting_or_element,
                                       uint32_t index, struct zmk_custom_setting_value *out_value,
                                       enum zmk_custom_setting_write_mode mode) {
    struct zmk_custom_setting_value_view out_value_view =
        out_value ? value_output(out_value) : (struct zmk_custom_setting_value_view){0};
    int ret = zmk_custom_setting_array_remove_at_view(array_setting_or_element, index,
                                                      out_value ? &out_value_view : NULL, mode);
    if (!ret && out_value)
        ret = value_finish(out_value, &out_value_view);
    return ret;
}

int zmk_custom_setting_validate(const struct zmk_custom_setting *setting,
                                const struct zmk_custom_setting_value *value) {
    if (!value)
        return -EINVAL;
    int check = value_check_input(value);
    if (check)
        return check;
    struct zmk_custom_setting_value_view value_view = value_borrow(value);
    int ret = zmk_custom_setting_validate_view(setting, value ? &value_view : NULL);

    return ret;
}

int zmk_custom_setting_keyspace_create(struct zmk_custom_setting_keyspace *keyspace,
                                       const char *key,
                                       const struct zmk_custom_setting_value *value,
                                       enum zmk_custom_setting_write_mode mode,
                                       const struct zmk_custom_setting **out_setting) {
    if (!value)
        return -EINVAL;
    int check = value_check_input(value);
    if (check)
        return check;
    struct zmk_custom_setting_value_view value_view = value_borrow(value);
    int ret = zmk_custom_setting_keyspace_create_view(keyspace, key, value ? &value_view : NULL,
                                                      mode, out_setting);

    return ret;
}
