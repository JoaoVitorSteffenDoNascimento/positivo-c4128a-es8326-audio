/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef POSITIVO_AUDIO_ALSA_H
#define POSITIVO_AUDIO_ALSA_H
#include <alsa/asoundlib.h>
#include <json-c/json.h>
#include "audio_mixer.h"
typedef struct { snd_ctl_t *ctl; json_object *before; const char *history; } AlsaContext;
int alsa_read(void *, const char *, AudioControl *);
int alsa_neutral(void *, const AudioControl *, long *);
int alsa_write(void *, const AudioControl *, const long *);
int alsa_save(void *, const AudioPlan *, bool);
json_object *alsa_snapshot(snd_ctl_t *);
int save_json(const char *, json_object *);
#endif
