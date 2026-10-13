/* SPDX-License-Identifier: BSD-3-Clause */
#include "audio_mixer.h"
#include "audio_alsa.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    AudioControl controls[9]; size_t count;
    int writes, saves; bool fail_write, fail_save, fail_db, bad_db, missing_dac, missing_optional, ignore_write;
} Fake;
static const char *names[] = {"Headphone Switch", "Speaker Switch", "DAC Playback Volume",
    "HPL Playback Volume", "HPR Playback Volume", "SPKL Playback Volume", "SPKR Playback Volume",
    "Headphone Jack", "HPL Volume"};
static Fake fixture(void)
{
    Fake fake = {.count = 9};
    for (unsigned int i = 0; i < 9; i++) {
        AudioControl *c = &fake.controls[i];
        snprintf(c->name, sizeof(c->name), "%s", names[i]); c->numid = i + 1;
        c->count = 2; c->type = i < 2 || i == 7 ? 1 : 2;
        c->writable = i != 7; c->max = c->type == 1 ? 1 : 191;
        c->values[0] = c->values[1] = i == 0 || i == 7 ? 1 : i == 8 ? 3 : 0;
    }
    return fake;
}
static int read_control(void *context, const char *name, AudioControl *out)
{
    Fake *fake = context;
    if ((fake->missing_dac && !strcmp(name, names[2])) ||
        (fake->missing_optional && !strcmp(name, names[3]))) return -ENOENT;
    for (size_t i = 0; i < fake->count; i++)
        if (!strcmp(fake->controls[i].name, name)) { *out = fake->controls[i]; return 0; }
    return -ENOENT;
}
static int neutral(void *context, const AudioControl *control, long *value)
{
    Fake *fake = context; (void)control;
    if (fake->fail_db) return -EIO;
    *value = fake->bad_db ? 192 : 191; return 0;
}
static int write_control(void *context, const AudioControl *control, const long *values)
{
    Fake *fake = context; fake->writes++;
    assert(strcmp(control->name, "Headphone Jack")); assert(strcmp(control->name, "HPL Volume"));
    if (fake->fail_write && !strcmp(control->name, names[2]) && values[0] == 191) return -EIO;
    if (fake->ignore_write && values[0] == 191) return 0;
    AudioControl *current = &fake->controls[control->numid - 1];
    memcpy(current->values, values, current->count * sizeof(long)); return 0;
}
static int save(void *context, const AudioPlan *plan, bool after)
{
    Fake *fake = context; (void)plan; fake->saves++;
    if (!after) assert(fake->writes == 0);
    return fake->fail_save ? -EIO : 0;
}
static AudioBackend backend(Fake *fake)
{
    return (AudioBackend){fake, read_control, neutral, write_control, save};
}
static int db_low = -9600, db_high = 1200, db_status = 0, converted;
static long conversion_target;
int __wrap_snd_ctl_get_dB_range(snd_ctl_t *ctl, const snd_ctl_elem_id_t *id, long *low, long *high)
{
    (void)ctl; assert(snd_ctl_elem_id_get_numid(id) == 3); *low = db_low; *high = db_high; return db_status;
}
int __wrap_snd_ctl_convert_from_dB(snd_ctl_t *ctl, const snd_ctl_elem_id_t *id, long target, long *value, int direction)
{
    (void)ctl; (void)id; assert(direction == -1); converted++; conversion_target = target; *value = 191; return 0;
}
int main(void)
{
    char error[256]; AudioPlan plan; Fake fake = fixture(), original = fake;
    AudioBackend b = backend(&fake);
    assert(audio_prepare(&b, &plan, error, sizeof(error)) == 0); assert(plan.count == 7);
    assert(fake.writes == 0 && fake.saves == 0);
    assert(audio_execute(&b, &plan, error, sizeof(error)) == 0);
    assert(fake.controls[0].values[0] == 0 && fake.controls[1].values[0] == 1);
    for (size_t i = 2; i < 7; i++) assert(fake.controls[i].values[0] == 191 && fake.controls[i].values[1] == 191);
    assert(!memcmp(&fake.controls[7], &original.controls[7], 2 * sizeof(AudioControl)));
    int writes = fake.writes; fake.saves = 0;
    assert(audio_prepare(&b, &plan, error, sizeof(error)) == 0);
    /* Reset the save-before-write counter assertion for a second transaction. */
    fake.writes = 0; assert(audio_execute(&b, &plan, error, sizeof(error)) == 0); assert(fake.writes == 0); assert(writes == 7);
    puts("C mixer: stereo routing, untouched sensors and idempotency passed");
    for (int scenario = 0; scenario < 6; scenario++) {
        fake = fixture(); b = backend(&fake);
        if (scenario == 0) fake.fail_db = true;
        if (scenario == 1) fake.bad_db = true;
        if (scenario == 2) fake.missing_dac = true;
        if (scenario == 3) fake.controls[0].writable = false;
        if (scenario == 4) fake.controls[0].count = AUDIO_VALUES + 1;
        if (scenario == 5) fake.controls[2].type = 3;
        assert(audio_prepare(&b, &plan, error, sizeof(error)) < 0);
        assert(fake.writes == 0 && fake.saves == 0);
    }
    fake = fixture(); fake.missing_optional = true; b = backend(&fake);
    assert(audio_prepare(&b, &plan, error, sizeof(error)) == 0 && plan.count == 6);
    for (int scenario = 0; scenario < 3; scenario++) {
        fake = fixture(); original = fake; b = backend(&fake);
        if (scenario == 0) fake.fail_write = true;
        if (scenario == 1) fake.fail_save = true;
        if (scenario == 2) fake.ignore_write = true;
        assert(audio_prepare(&b, &plan, error, sizeof(error)) == 0);
        assert(audio_execute(&b, &plan, error, sizeof(error)) < 0);
        assert(!memcmp(fake.controls, original.controls, sizeof(fake.controls)));
        if (scenario == 1) assert(fake.writes == 0);
    }
    puts("C mixer: preflight failure, absent optional controls, persistence, readback and rollback passed");
    AlsaContext context = {0}; AudioControl control = {.numid=3}; long value;
    assert(alsa_neutral(&context, &control, &value) == 0 && value == 191 && conversion_target == 0);
    db_high = -600;
    assert(alsa_neutral(&context, &control, &value) == 0 && conversion_target == -600);
    int count = converted; db_low = 600; db_high = 1200;
    assert(alsa_neutral(&context, &control, &value) < 0 && count == converted);
    db_status = -EIO; assert(alsa_neutral(&context, &control, &value) < 0);
    puts("C ALSA: same 0 dB cap and conversion direction as Python passed");
    return 0;
}
