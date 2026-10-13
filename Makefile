# SPDX-License-Identifier: BSD-3-Clause
CC ?= cc
CPPFLAGS += -D_GNU_SOURCE -Isrc $(shell pkg-config --cflags alsa json-c libcrypto)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror -Wformat=2
LDLIBS += $(shell pkg-config --libs alsa json-c libcrypto) -lm
SOURCES = src/audio_interno.c src/audio_mixer.c src/audio_alsa.c
.PHONY: all check sanitize
all: build/audio_interno
build:
	mkdir -p build
build/audio_interno: $(SOURCES) src/audio_mixer.h src/audio_alsa.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDLIBS) -o $@
build/test_mixer: tests/test_mixer.c src/audio_mixer.c src/audio_alsa.c src/audio_mixer.h src/audio_alsa.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_mixer.c src/audio_mixer.c src/audio_alsa.c $(LDLIBS) -Wl,--wrap=snd_ctl_get_dB_range -Wl,--wrap=snd_ctl_convert_from_dB -o $@
build/test_native_helpers: tests/test_native_helpers.c src/audio_interno.c src/audio_mixer.c src/audio_alsa.c src/audio_mixer.h src/audio_alsa.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_native_helpers.c src/audio_mixer.c src/audio_alsa.c $(LDLIBS) -o $@
check: all build/test_mixer build/test_native_helpers
	./build/test_mixer
	./build/test_native_helpers
	PYTHONPATH=src python3 -m unittest discover -s tests -v
sanitize: | build
	$(CC) $(CPPFLAGS) -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_mixer.c src/audio_mixer.c src/audio_alsa.c $(LDLIBS) -Wl,--wrap=snd_ctl_get_dB_range -Wl,--wrap=snd_ctl_convert_from_dB -o build/test_mixer_sanitize
	./build/test_mixer_sanitize
	$(CC) $(CPPFLAGS) -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_native_helpers.c src/audio_mixer.c src/audio_alsa.c $(LDLIBS) -o build/test_helpers_sanitize
	./build/test_helpers_sanitize
