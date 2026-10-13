/* SPDX-License-Identifier: BSD-3-Clause */
#include "audio_mixer.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int fail(char *error, size_t size, const char *action, const char *name)
{
    snprintf(error, size, "%s: %s", action, name);
    return -EINVAL;
}

static int append(AudioPlan *plan, const AudioControl *control, long value,
                  char *error, size_t size)
{
    if (!control->count || control->count > AUDIO_VALUES || plan->count >= AUDIO_CHANGES)
        return fail(error, size, "Quantidade de canais inválida", control->name);
    long low = control->type == 1 ? 0 : control->min;
    long high = control->type == 1 ? 1 : control->max;
    if (value < low || value > high)
        return fail(error, size, "Valor fora do intervalo", control->name);
    AudioChange *change = &plan->changes[plan->count++];
    change->before = *control;
    for (unsigned int i = 0; i < control->count; i++) change->desired[i] = value;
    return 0;
}

int audio_prepare(AudioBackend *backend, AudioPlan *plan, char *error, size_t size)
{
    static const char *switches[] = {"Headphone Switch", "Speaker Switch"};
    static const long states[] = {0, 1};
    static const char *volumes[] = {"DAC Playback Volume", "HPL Playback Volume",
        "HPR Playback Volume", "SPKL Playback Volume", "SPKR Playback Volume"};
    memset(plan, 0, sizeof(*plan));
    for (size_t i = 0; i < 2; i++) {
        AudioControl control;
        if (backend->read(backend->context, switches[i], &control) < 0 ||
            control.type != 1 || !control.writable)
            return fail(error, size, "Controle de rota indisponível", switches[i]);
        if (append(plan, &control, states[i], error, size) < 0) return -EINVAL;
    }
    for (size_t i = 0; i < 5; i++) {
        AudioControl control;
        int status = backend->read(backend->context, volumes[i], &control);
        if (status == -ENOENT && i != 0) continue;
        if (status < 0 || control.type != 2 || !control.writable)
            return fail(error, size, "Controle digital indisponível", volumes[i]);
        long neutral;
        if (backend->neutral(backend->context, &control, &neutral) < 0)
            return fail(error, size, "Nível em dB indisponível", volumes[i]);
        if (append(plan, &control, neutral, error, size) < 0) return -EINVAL;
    }
    return 0;
}

int audio_execute(AudioBackend *backend, const AudioPlan *plan, char *error, size_t size)
{
    size_t written[AUDIO_CHANGES], count = 0;
    if (backend->save(backend->context, plan, false) < 0)
        return fail(error, size, "Backup não gravado", "nenhum controle alterado");
    for (size_t i = 0; i < plan->count; i++) {
        const AudioChange *change = &plan->changes[i];
        if (!memcmp(change->before.values, change->desired,
                    change->before.count * sizeof(long))) continue;
        written[count++] = i; /* Includes a possibly partial failed write. */
        AudioControl after;
        if (backend->write(backend->context, &change->before, change->desired) < 0 ||
            backend->read(backend->context, change->before.name, &after) < 0 ||
            after.numid != change->before.numid || after.type != change->before.type ||
            after.count != change->before.count ||
            memcmp(after.values, change->desired, after.count * sizeof(long))) {
            fail(error, size, "Driver não manteve o ajuste", change->before.name);
            goto rollback;
        }
    }
    if (backend->save(backend->context, plan, true) < 0) {
        fail(error, size, "Falha ao gravar estado final", "restaurando controles");
        goto rollback;
    }
    return 0;
rollback:
    while (count) {
        const AudioControl *before = &plan->changes[written[--count]].before;
        if (backend->write(backend->context, before, before->values) < 0)
            fprintf(stderr, "Falha ao restaurar controle %s; consulte o backup.\n", before->name);
    }
    return -EIO;
}
