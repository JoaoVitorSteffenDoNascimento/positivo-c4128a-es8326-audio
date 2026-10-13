/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef POSITIVO_AUDIO_MIXER_H
#define POSITIVO_AUDIO_MIXER_H
#include <stdbool.h>
#include <stddef.h>
#define AUDIO_VALUES 128
#define AUDIO_CHANGES 7
/* ALSA types: BOOLEAN=1, INTEGER=2. Sensors/enums are never written. */
typedef struct {
    char name[128]; unsigned int numid, count;
    int type; bool writable; long min, max, values[AUDIO_VALUES];
} AudioControl;
typedef struct { AudioControl before; long desired[AUDIO_VALUES]; } AudioChange;
typedef struct { AudioChange changes[AUDIO_CHANGES]; size_t count; } AudioPlan;
typedef struct {
    void *context;
    int (*read)(void *, const char *, AudioControl *);
    int (*neutral)(void *, const AudioControl *, long *);
    int (*write)(void *, const AudioControl *, const long *);
    int (*save)(void *, const AudioPlan *, bool);
} AudioBackend;
int audio_prepare(AudioBackend *, AudioPlan *, char *, size_t);
int audio_execute(AudioBackend *, const AudioPlan *, char *, size_t);
#endif
