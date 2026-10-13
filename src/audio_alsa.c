/* SPDX-License-Identifier: BSD-3-Clause */
#include "audio_alsa.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int save_json(const char *path, json_object *object)
{
    char temp[PATH_MAX];
    if (snprintf(temp, sizeof(temp), "%s.tmpXXXXXX", path) >= (int)sizeof(temp)) return -ENAMETOOLONG;
    int fd = mkstemp(temp);
    if (fd < 0) return -errno;
    const char *data = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PRETTY);
    size_t length = strlen(data), offset = 0;
    int result = 0;
    while (offset < length) {
        ssize_t done = write(fd, data + offset, length - offset);
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) { result = -EIO; break; }
        offset += (size_t)done;
    }
    if (!result && (write(fd, "\n", 1) != 1 || fsync(fd))) result = -errno;
    if (close(fd) && !result) result = -errno;
    if (!result && rename(temp, path)) result = -errno;
    if (result) unlink(temp);
    return result;
}

static int read_id(snd_ctl_t *ctl, unsigned int numid, AudioControl *control)
{
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *value;
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_alloca(&value);
    snd_ctl_elem_info_set_numid(info, numid);
    int result = snd_ctl_elem_info(ctl, info);
    if (result < 0) return result;
    memset(control, 0, sizeof(*control));
    snprintf(control->name, sizeof(control->name), "%s", snd_ctl_elem_info_get_name(info));
    control->numid = numid;
    control->type = snd_ctl_elem_info_get_type(info);
    control->writable = snd_ctl_elem_info_is_writable(info);
    control->count = snd_ctl_elem_info_get_count(info);
    if (!control->count || control->count > AUDIO_VALUES) return -EOVERFLOW;
    if (control->type != 1 && control->type != 2 && control->type != 3) return -ENOTSUP;
    if (control->type == 2) {
        control->min = snd_ctl_elem_info_get_min(info);
        control->max = snd_ctl_elem_info_get_max(info);
    }
    snd_ctl_elem_value_set_numid(value, numid);
    result = snd_ctl_elem_read(ctl, value);
    if (result < 0) return result;
    for (unsigned int i = 0; i < control->count; i++) {
        if (control->type == 1) control->values[i] = snd_ctl_elem_value_get_boolean(value, i);
        else if (control->type == 2) control->values[i] = snd_ctl_elem_value_get_integer(value, i);
        else control->values[i] = snd_ctl_elem_value_get_enumerated(value, i);
    }
    return 0;
}

static int get_list(snd_ctl_t *ctl, snd_ctl_elem_list_t **list)
{
    int result = snd_ctl_elem_list_malloc(list);
    if (result < 0) return result;
    result = snd_ctl_elem_list(ctl, *list);
    if (result >= 0) result = snd_ctl_elem_list_alloc_space(*list, snd_ctl_elem_list_get_count(*list));
    if (result >= 0) result = snd_ctl_elem_list(ctl, *list);
    if (result < 0) {
        snd_ctl_elem_list_free_space(*list);
        snd_ctl_elem_list_free(*list);
        *list = NULL;
    }
    return result;
}

int alsa_read(void *context, const char *name, AudioControl *control)
{
    AlsaContext *alsa = context;
    snd_ctl_elem_list_t *list = NULL;
    int result = get_list(alsa->ctl, &list);
    if (result < 0) return result;
    result = -ENOENT;
    for (unsigned int i = 0; i < snd_ctl_elem_list_get_used(list); i++) {
        if (!strcmp(snd_ctl_elem_list_get_name(list, i), name)) {
            result = read_id(alsa->ctl, snd_ctl_elem_list_get_numid(list, i), control);
            break;
        }
    }
    snd_ctl_elem_list_free_space(list);
    snd_ctl_elem_list_free(list);
    return result;
}

int alsa_neutral(void *context, const AudioControl *control, long *value)
{
    AlsaContext *alsa = context;
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_id_set_numid(id, control->numid);
    long low, high;
    int result = snd_ctl_get_dB_range(alsa->ctl, id, &low, &high);
    if (result < 0) return result;
    long target = high < 0 ? high : 0;
    if (target < low) return -ERANGE;
    return snd_ctl_convert_from_dB(alsa->ctl, id, target, value, -1);
}

int alsa_write(void *context, const AudioControl *before, const long *values)
{
    AlsaContext *alsa = context;
    AudioControl current;
    int result = read_id(alsa->ctl, before->numid, &current);
    if (result < 0) return result;
    if (!current.writable || (current.type != 1 && current.type != 2) ||
        current.type != before->type || current.count != before->count ||
        strcmp(current.name, before->name)) return -EINVAL;
    long low = current.type == 1 ? 0 : current.min;
    long high = current.type == 1 ? 1 : current.max;
    for (unsigned int i = 0; i < current.count; i++)
        if (values[i] < low || values[i] > high) return -ERANGE;
    snd_ctl_elem_value_t *value;
    snd_ctl_elem_value_alloca(&value);
    snd_ctl_elem_value_set_numid(value, current.numid);
    result = snd_ctl_elem_read(alsa->ctl, value);
    if (result < 0) return result;
    for (unsigned int i = 0; i < current.count; i++) {
        if (current.type == 1) snd_ctl_elem_value_set_boolean(value, i, values[i]);
        else snd_ctl_elem_value_set_integer(value, i, values[i]);
    }
    return snd_ctl_elem_write(alsa->ctl, value);
}

static json_object *control_json(const AudioControl *control)
{
    json_object *row = json_object_new_object(), *values = json_object_new_array();
    json_object_object_add(row, "name", json_object_new_string(control->name));
    json_object_object_add(row, "numid", json_object_new_int64(control->numid));
    json_object_object_add(row, "type", json_object_new_int(control->type));
    json_object_object_add(row, "writable", json_object_new_boolean(control->writable));
    for (unsigned int i = 0; i < control->count; i++)
        json_object_array_add(values, json_object_new_int64(control->values[i]));
    json_object_object_add(row, "values", values);
    if (control->type == 2) {
        json_object_object_add(row, "min", json_object_new_int64(control->min));
        json_object_object_add(row, "max", json_object_new_int64(control->max));
    }
    return row;
}

json_object *alsa_snapshot(snd_ctl_t *ctl)
{
    json_object *array = json_object_new_array();
    snd_ctl_elem_list_t *list = NULL;
    if (get_list(ctl, &list) < 0) { json_object_put(array); return NULL; }
    for (unsigned int i = 0; i < snd_ctl_elem_list_get_used(list); i++) {
        AudioControl control;
        unsigned int id = snd_ctl_elem_list_get_numid(list, i);
        int result = read_id(ctl, id, &control);
        json_object *row;
        if (result < 0) {
            row = json_object_new_object();
            json_object_object_add(row, "name", json_object_new_string(snd_ctl_elem_list_get_name(list, i)));
            json_object_object_add(row, "numid", json_object_new_int64(id));
            json_object_object_add(row, "error", json_object_new_string(snd_strerror(result)));
        } else row = control_json(&control);
        json_object_array_add(array, row);
    }
    snd_ctl_elem_list_free_space(list);
    snd_ctl_elem_list_free(list);
    return array;
}

int alsa_save(void *context, const AudioPlan *plan, bool after)
{
    AlsaContext *alsa = context;
    json_object *document = json_object_new_object(), *changes = json_object_new_array();
    json_object_object_add(document, "before", json_object_get(alsa->before));
    for (size_t i = 0; i < plan->count; i++) {
        json_object *row = json_object_new_object(), *values = json_object_new_array();
        json_object_object_add(row, "name", json_object_new_string(plan->changes[i].before.name));
        for (unsigned int j = 0; j < plan->changes[i].before.count; j++)
            json_object_array_add(values, json_object_new_int64(plan->changes[i].desired[j]));
        json_object_object_add(row, "values", values);
        json_object_array_add(changes, row);
    }
    json_object_object_add(document, "plan", changes);
    if (after) {
        json_object *snapshot = alsa_snapshot(alsa->ctl);
        if (!snapshot) { json_object_put(document); return -EIO; }
        json_object_object_add(document, "after", snapshot);
    }
    int result = save_json(alsa->history, document);
    json_object_put(document);
    return result;
}
