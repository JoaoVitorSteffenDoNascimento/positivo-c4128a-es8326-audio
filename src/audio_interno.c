/* SPDX-License-Identifier: BSD-3-Clause */
/* Native C port of audio_interno.py and its activation dependencies. */
#include "audio_alsa.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <openssl/evp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CARD "alsa_card.pci-0000_00_0e.0-platform-sof-essx8336"
#define PROFILE "vision14-interno"
#define PORT "vision14-speaker"
static char state_dir[PATH_MAX];

static double monotonic(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static void pause_quarter(void)
{
    struct timespec t = {.tv_nsec = 250000000};
    while (nanosleep(&t, &t) < 0 && errno == EINTR) {}
}
static int path_join(char *out, const char *left, const char *right)
{
    return snprintf(out, PATH_MAX, "%s/%s", left, right) >= PATH_MAX ? -ENAMETOOLONG : 0;
}
static int safe_directory(const char *path, bool create)
{
    char buffer[PATH_MAX];
    if (path[0] != '/' || strlen(path) >= sizeof(buffer)) return -EINVAL;
    strcpy(buffer, path);
    for (char *p = buffer + 1; ; p++) {
        if (*p && *p != '/') continue;
        char saved = *p; *p = 0;
        struct stat st;
        if (lstat(buffer, &st)) {
            if (!create || errno != ENOENT || mkdir(buffer, 0700)) return -errno;
        } else if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return -EINVAL;
        *p = saved;
        if (!saved) break;
    }
    return 0;
}
static int parent_is_safe(const char *path)
{
    char parent[PATH_MAX];
    if (strlen(path) >= sizeof(parent)) return -ENAMETOOLONG;
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return -EINVAL;
    *slash = 0;
    return safe_directory(parent, false);
}
static int read_file(const char *path, char **out, size_t *length)
{
    *out = NULL;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -errno;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 16 * 1024 * 1024) {
        close(fd); return -EINVAL;
    }
    char *data = calloc((size_t)st.st_size + 1, 1);
    if (!data) { close(fd); return -ENOMEM; }
    size_t done = 0;
    while (done < (size_t)st.st_size) {
        ssize_t n = read(fd, data + done, (size_t)st.st_size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { free(data); close(fd); return -EIO; }
        if (n == 0) break; /* sysfs attributes report a page-sized st_size. */
        done += (size_t)n;
    }
    close(fd); *out = data; *length = done;
    return 0;
}
static void trim(char *text)
{
    size_t n = strlen(text);
    while (n && (text[n-1] == '\n' || text[n-1] == '\r' || text[n-1] == ' ')) text[--n] = 0;
}
static int hardware(void)
{
    char *product = NULL, driver[PATH_MAX]; size_t size;
    if (read_file("/sys/class/dmi/id/product_name", &product, &size) < 0) return -ENODEV;
    trim(product);
    bool match = !strcmp(product, "C4128A-14"); free(product);
    if (!match || !realpath("/sys/bus/i2c/devices/i2c-ESSX8326:00/driver", driver)) return -ENODEV;
    const char *base = strrchr(driver, '/');
    return base && !strcmp(base + 1, "es8326") ? 0 : -ENODEV;
}

/* execvp argument vectors only: no shell, no string interpolation. */
static int command(const char *const argv[], int timeout, char **output)
{
    int pipefd[2]; *output = NULL;
    if (pipe(pipefd)) return -errno;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -errno; }
    if (!pid) {
        setpgid(0, 0);
        int null = open("/dev/null", O_RDWR);
        if (null < 0 || dup2(null, STDIN_FILENO) < 0 || dup2(null, STDERR_FILENO) < 0 ||
            dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(126);
        close(null); close(pipefd[0]); close(pipefd[1]);
        execvp(argv[0], (char *const *)argv); _exit(127);
    }
    setpgid(pid, pid); close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    size_t used = 0, capacity = 4096;
    char *data = malloc(capacity);
    bool eof = false, exited = false;
    int status = 0, result = 0;
    double deadline = monotonic() + timeout;
    if (!data) { result = -ENOMEM; goto stop; }
    while (!eof || !exited) {
        if (monotonic() > deadline) { result = -ETIMEDOUT; goto stop; }
        struct pollfd poller = {.fd = pipefd[0], .events = POLLIN | POLLHUP};
        int ready = poll(&poller, 1, 50);
        if (ready < 0 && errno != EINTR) { result = -errno; goto stop; }
        if (ready > 0 && !eof) {
            for (;;) {
                if (used + 2048 >= capacity) {
                    if (capacity >= 16 * 1024 * 1024) { result = -EOVERFLOW; goto stop; }
                    capacity *= 2;
                    char *next = realloc(data, capacity);
                    if (!next) { result = -ENOMEM; goto stop; }
                    data = next;
                }
                ssize_t n = read(pipefd[0], data + used, capacity - used - 1);
                if (n > 0) used += (size_t)n;
                else if (n == 0) { eof = true; break; }
                else if (errno == EINTR) continue;
                else if (errno == EAGAIN) break;
                else { result = -errno; goto stop; }
            }
        }
        if (!exited) {
            pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) exited = true;
            else if (waited < 0 && errno != EINTR) { result = -errno; goto stop; }
        }
    }
    data[used] = 0;
    if (!WIFEXITED(status) || WEXITSTATUS(status)) result = -EIO;
    close(pipefd[0]); *output = data;
    return result;
stop:
    kill(-pid, SIGKILL);
    if (!exited) while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    close(pipefd[0]); free(data);
    return result;
}
static int run(const char *const argv[], int timeout)
{
    char *output; int result = command(argv, timeout, &output); free(output); return result;
}
static json_object *get(json_object *obj, const char *name)
{
    json_object *value = NULL;
    if (obj) json_object_object_get_ex(obj, name, &value);
    return value;
}
static const char *string(json_object *obj, const char *name)
{
    json_object *value = get(obj, name);
    return value ? json_object_get_string(value) : "";
}
static json_object *pulse_list(const char *kind)
{
    const char *argv[] = {"pactl", "--format=json", "list", kind, NULL};
    char *data; int result = command(argv, 15, &data);
    json_object *obj = result >= 0 ? json_tokener_parse(data) : NULL;
    free(data);
    if (obj && !json_object_is_type(obj, json_type_array)) { json_object_put(obj); obj = NULL; }
    return obj;
}
static json_object *snapshot(void)
{
    json_object *obj = json_object_new_object();
    char date[64]; time_t now = time(NULL); struct tm tm;
    gmtime_r(&now, &tm); strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%SZ", &tm);
    json_object_object_add(obj, "time", json_object_new_string(date));
    snd_ctl_t *ctl = NULL;
    json_object *alsa = NULL;
    if (snd_ctl_open(&ctl, "hw:sofessx8336", 0) >= 0) { alsa = alsa_snapshot(ctl); snd_ctl_close(ctl); }
    if (!alsa) { alsa = json_object_new_object(); json_object_object_add(alsa, "error", json_object_new_string("Mixer indisponível")); }
    json_object_object_add(obj, "alsa", alsa);
    static const char *kinds[] = {"cards", "sinks"};
    for (size_t i = 0; i < 2; i++) {
        json_object *list = pulse_list(kinds[i]);
        if (list) json_object_object_add(obj, kinds[i], list);
        else { char key[32]; snprintf(key, sizeof(key), "%s_error", kinds[i]); json_object_object_add(obj, key, json_object_new_string("Sessão indisponível")); }
    }
    const char *argv[] = {"pactl", "get-default-sink", NULL}; char *out;
    int result = command(argv, 15, &out);
    if (result >= 0) { trim(out); json_object_object_add(obj, "default_sink", json_object_new_string(out)); }
    else json_object_object_add(obj, "default_sink_error", json_object_new_string("Sessão indisponível"));
    free(out); return obj;
}
static int save_snapshot(const char *directory, const char *name)
{
    char path[PATH_MAX]; if (path_join(path, directory, name) < 0) return -ENAMETOOLONG;
    json_object *data = snapshot(); int result = save_json(path, data); json_object_put(data); return result;
}
static int unique_name(const char *prefix, char *out)
{
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    char name[128];
    snprintf(name, sizeof(name), "%s-%lld-%09ld-%ld", prefix, (long long)now.tv_sec, now.tv_nsec, (long)getpid());
    return path_join(out, state_dir, name);
}

static int initialize(bool dry_run)
{
    if (hardware() < 0) { fprintf(stderr, "Ajuste exclusivo do C4128A-14 com driver es8326.\n"); return -ENODEV; }
    AlsaContext context = {0};
    int result = snd_ctl_open(&context.ctl, "hw:sofessx8336", 0);
    if (result < 0) return result;
    AudioBackend backend = {&context, alsa_read, alsa_neutral, alsa_write, alsa_save};
    AudioPlan plan; char error[256] = ""; char history[PATH_MAX];
    result = audio_prepare(&backend, &plan, error, sizeof(error));
    if (result < 0) goto done;
    if (dry_run) {
        for (size_t i = 0; i < plan.count; i++)
            printf("%s: %u canais -> %ld\n", plan.changes[i].before.name,
                   plan.changes[i].before.count, plan.changes[i].desired[0]);
        goto done;
    }
    result = unique_name("mixer", history);
    if (result < 0) goto done;
    if (strlen(history) + 5 >= sizeof(history)) { result = -ENAMETOOLONG; goto done; }
    strcat(history, ".json"); context.history = history;
    context.before = alsa_snapshot(context.ctl);
    if (!context.before) { result = -EIO; goto done; }
    result = audio_execute(&backend, &plan, error, sizeof(error));
    if (!result) puts("Mixer: alto-falantes ligados; volumes digitais ajustados pelo nível em dB do driver.");
done:
    if (result < 0 && *error) fprintf(stderr, "%s\n", error);
    if (context.before) json_object_put(context.before);
    snd_ctl_close(context.ctl); return result;
}

static json_object *find_card(json_object *cards)
{
    json_object *found = NULL;
    if (!cards || !json_object_is_type(cards, json_type_array)) return NULL;
    for (size_t i = 0; i < json_object_array_length(cards); i++) {
        json_object *card = json_object_array_get_idx(cards, i);
        if (!strcmp(string(card, "name"), CARD)) {
            if (found) return NULL;
            found = card;
        }
    }
    return found;
}
static bool same_card(json_object *sink, json_object *card)
{
    return !strcmp(string(sink, "card"), string(card, "index"));
}
static json_object *find_speaker(json_object *sinks, json_object *card, bool exclusive)
{
    if (!sinks || !card || !json_object_is_type(sinks, json_type_array)) return NULL;
    json_object *found = NULL; size_t on_card = 0;
    for (size_t i = 0; i < json_object_array_length(sinks); i++) {
        json_object *sink = json_object_array_get_idx(sinks, i);
        if (!same_card(sink, card)) continue;
        on_card++;
        const char *name = string(sink, "name");
        json_object *props = get(sink, "properties");
        bool match = exclusive ? strstr(name, ".vision14-speakers") != NULL :
            (!strstr(name, ".hdmi-stereo") &&
             (!strcmp(string(props, "alsa.device"), "0") || strstr(name, "analog") || strstr(name, ".vision14-speakers")));
        if (!match) continue;
        if (found) return NULL;
        found = sink;
    }
    return exclusive && on_card != 1 ? NULL : found;
}
static bool exclusive_port(json_object *sink)
{
    json_object *ports = get(sink, "ports");
    if (!ports) return false;
    if (json_object_is_type(ports, json_type_array))
        return json_object_array_length(ports) == 1 &&
            !strcmp(string(json_object_array_get_idx(ports, 0), "name"), PORT);
    if (json_object_is_type(ports, json_type_object))
        return json_object_object_length(ports) == 1 && get(ports, PORT) != NULL;
    return false;
}
static int write_le16(FILE *file, uint16_t value)
{
    unsigned char bytes[] = {(unsigned char)value, (unsigned char)(value >> 8)};
    return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes) ? 0 : -EIO;
}
static int write_le32(FILE *file, uint32_t value)
{
    unsigned char bytes[] = {(unsigned char)value, (unsigned char)(value >> 8),
                            (unsigned char)(value >> 16), (unsigned char)(value >> 24)};
    return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes) ? 0 : -EIO;
}
static int write_wave(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -errno;
    FILE *file = fdopen(fd, "wb");
    if (!file) { close(fd); return -errno; }
    const uint32_t frames = 33600, bytes = frames * 2 * 2 * 2;
    int result = 0;
    if (fwrite("RIFF", 1, 4, file) != 4 || write_le32(file, 36 + bytes) ||
        fwrite("WAVEfmt ", 1, 8, file) != 8 || write_le32(file, 16) ||
        write_le16(file, 1) || write_le16(file, 2) || write_le32(file, 48000) ||
        write_le32(file, 48000 * 4) || write_le16(file, 4) || write_le16(file, 16) ||
        fwrite("data", 1, 4, file) != 4 || write_le32(file, bytes)) result = -EIO;
    for (unsigned int channel = 0; channel < 2 && !result; channel++) {
        for (uint32_t index = 0; index < frames; index++) {
            double fade = fmin(1.0, fmin(index / 1200.0, (frames - index) / 1200.0));
            int16_t value = (int16_t)(1500.0 * fade * sin(2.0 * acos(-1.0) * 440.0 * index / 48000.0));
            if (write_le16(file, channel == 0 ? (uint16_t)value : 0) ||
                write_le16(file, channel == 1 ? (uint16_t)value : 0)) { result = -EIO; break; }
        }
    }
    if (!result && (fflush(file) || fsync(fd))) result = -EIO;
    if (fclose(file) && !result) result = -EIO;
    return result;
}
static int playback_test(json_object *sink)
{
    char path[PATH_MAX], target[1024];
    if (path_join(path, state_dir, "teste-baixo-volume.wav") < 0 ||
        snprintf(target, sizeof(target), "--target=%s", string(sink, "name")) >= (int)sizeof(target)) return -ENAMETOOLONG;
    int result = write_wave(path);
    if (result < 0) return result;
    puts("Reproduzindo dois tons curtos e baixos: esquerdo, depois direito.");
    const char *argv[] = {"pw-play", target, path, NULL};
    return run(argv, 15);
}
static int activate(const char *directory)
{
    const char *check[] = {"systemctl", "--user", "show", "wireplumber.service", "--property=LoadState", "--value", NULL};
    const char *reload[] = {"systemctl", "--user", "daemon-reload", NULL};
    const char *restart[] = {"systemctl", "--user", "restart", "pipewire.service", "pipewire-pulse.service", "wireplumber.service", NULL};
    if (run(check, 15) < 0) return -ENOTCONN; /* No session -> no restart/mixer mutation. */
    if (save_snapshot(directory, "antes.json") < 0 || run(reload, 15) < 0 || run(restart, 30) < 0) return -EIO;
    json_object *cards = NULL, *card = NULL, *sinks = NULL, *sink = NULL;
    int result = -ENODEV;
    for (unsigned int i = 0; i < 24; i++) {
        if (cards) json_object_put(cards);
        cards = pulse_list("cards"); card = find_card(cards);
        if (card) break;
        pause_quarter();
    }
    if (!card) goto done;
    const char *profile[] = {"pactl", "set-card-profile", string(card, "name"), PROFILE, NULL};
    if (run(profile, 15) < 0) goto done;
    for (unsigned int i = 0; i < 24; i++) {
        if (sinks) json_object_put(sinks);
        sinks = pulse_list("sinks"); sink = find_speaker(sinks, card, true);
        if (sink) break;
        pause_quarter();
    }
    if (!sink || !exclusive_port(sink)) goto done;
    const char *name = string(sink, "name");
    const char *port[] = {"pactl", "set-sink-port", name, PORT, NULL};
    const char *volume[] = {"pactl", "set-sink-volume", name, "40%", NULL};
    const char *mute[] = {"pactl", "set-sink-mute", name, "0", NULL};
    const char *defaults[] = {"pactl", "set-default-sink", name, NULL};
    if (run(port, 15) < 0 || run(volume, 15) < 0 || run(mute, 15) < 0 || run(defaults, 15) < 0 || initialize(false) < 0) goto done;
    json_object *streams = pulse_list("sink-inputs");
    if (!streams) goto done;
    for (size_t i = 0; i < json_object_array_length(streams); i++) {
        json_object *stream = json_object_array_get_idx(streams, i);
        const char *move[] = {"pactl", "move-sink-input", string(stream, "index"), name, NULL};
        run(move, 15); /* Same best-effort policy as Python. */
    }
    json_object_put(streams);
    json_object *after = snapshot();
    char path[PATH_MAX];
    json_object *active = find_speaker(get(after, "sinks"), find_card(get(after, "cards")), false);
    if (path_join(path, directory, "depois.json") < 0 || save_json(path, after) < 0 ||
        !active || strcmp(string(active, "active_port"), PORT) ||
        json_object_get_boolean(get(active, "mute")) || strcmp(string(after, "default_sink"), name)) {
        json_object_put(after); goto done;
    }
    json_object_put(after);
    puts("Saída ativa: Alto-falantes do PC.");
    result = playback_test(sink);
    if (!result) result = save_snapshot(directory, "apos-teste.json");
    if (!result) printf("Reprodução enviada à placa. Confirme se o som foi audível. Diagnóstico: %s\n", directory);
done:
    if (cards) json_object_put(cards);
    if (sinks) json_object_put(sinks);
    return result;
}
static int apply(void)
{
    if (hardware() < 0) return -ENODEV;
    char directory[PATH_MAX];
    if (unique_name("ativacao", directory) < 0 || mkdir(directory, 0700)) return -EIO;
    char pointer[PATH_MAX];
    if (path_join(pointer, state_dir, "ultima-ativacao.txt") < 0) return -ENAMETOOLONG;
    int fd = open(pointer, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -errno;
    if (dprintf(fd, "%s\n", directory) < 0 || fsync(fd)) { close(fd); return -EIO; }
    close(fd);
    int result = activate(directory);
    if (result < 0) {
        char path[PATH_MAX]; json_object *error = json_object_new_object();
        json_object_object_add(error, "error", json_object_new_string("Falha na ativação; consulte o estado e o backup."));
        if (!path_join(path, directory, "erro.json")) save_json(path, error);
        json_object_put(error);
        save_snapshot(directory, "estado-na-falha.json");
        fprintf(stderr, "Diagnóstico da tentativa: %s\n", directory);
    }
    return result;
}

static int sha256(const char *data, size_t length, char out[65])
{
    unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned int size = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return -ENOMEM;
    int ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) && EVP_DigestUpdate(ctx, data, length) && EVP_DigestFinal_ex(ctx, bytes, &size);
    EVP_MD_CTX_free(ctx);
    if (!ok || size != 32) return -EIO;
    for (unsigned int i = 0; i < size; i++) snprintf(out + 2 * i, 3, "%02x", bytes[i]);
    return 0;
}
static int atomic_restore(const char *path, const char *data, size_t length, const struct stat *metadata)
{
    char temp[PATH_MAX];
    if (parent_is_safe(path) < 0 || snprintf(temp, sizeof(temp), "%s.tmpXXXXXX", path) >= PATH_MAX) return -EINVAL;
    struct stat target;
    if (!lstat(path, &target) && !S_ISREG(target.st_mode)) return -EINVAL;
    int fd = mkstemp(temp); if (fd < 0) return -errno;
    size_t offset = 0; int result = 0;
    while (offset < length) {
        ssize_t n = write(fd, data + offset, length - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { result = -EIO; break; }
        offset += (size_t)n;
    }
    const struct timespec times[] = {metadata->st_atim, metadata->st_mtim};
    if (!result && (fchmod(fd, metadata->st_mode & 0777) || futimens(fd, times) || fsync(fd))) result = -EIO;
    if (close(fd) && !result) result = -EIO;
    if (!result && rename(temp, path)) result = -errno;
    if (result) unlink(temp);
    return result;
}
static int undo(void)
{
    static const char *relatives[] = {
        ".config/wireplumber/wireplumber.conf.d/51-vision14-es8326.conf",
        ".config/systemd/user/wireplumber.service.d/51-vision14-speakers.conf",
        ".config/alsa-card-profile/vision14-interno.conf",
        ".config/alsa-card-profile/vision14-speaker.conf"};
    char pointer[PATH_MAX], manifest_path[PATH_MAX], *directory = NULL, *document = NULL;
    size_t length;
    if (path_join(pointer, state_dir, "backup-audio-interno.txt") < 0 || read_file(pointer, &directory, &length) < 0) return -ENOENT;
    trim(directory);
    if (safe_directory(directory, false) < 0 || path_join(manifest_path, directory, "manifest.json") < 0 ||
        read_file(manifest_path, &document, &length) < 0) { free(directory); return -EINVAL; }
    json_object *manifest = json_tokener_parse(document); free(document);
    json_object *files = get(manifest, "files");
    const char *home = getenv("HOME");
    char paths[4][PATH_MAX]; char *backups[4] = {0}; size_t lengths[4] = {0};
    struct stat metadata[4]; bool existed[4] = {false}, seen[4] = {false};
    int result = -EINVAL;
    if (!home || safe_directory(home, false) < 0 || !files ||
        !json_object_is_type(files, json_type_array) || json_object_array_length(files) != 4) goto done;
    for (size_t i = 0; i < 4; i++) if (path_join(paths[i], home, relatives[i]) < 0) goto done;
    /* Validate every allowed path, current hash and backup before changing anything. */
    for (size_t i = 0; i < 4; i++) {
        json_object *row = json_object_array_get_idx(files, i);
        int index = -1;
        for (int j = 0; j < 4; j++) if (!strcmp(paths[j], string(row, "path"))) index = j;
        if (index < 0 || seen[index] || parent_is_safe(paths[index]) < 0) goto done;
        seen[index] = true;
        struct stat current;
        if (!lstat(paths[index], &current)) {
            char *data = NULL, hash[65]; size_t size;
            if (!S_ISREG(current.st_mode) || read_file(paths[index], &data, &size) < 0) goto done;
            int status = sha256(data, size, hash); free(data);
            if (status < 0 || strcmp(hash, string(row, "applied_sha256"))) goto done;
        } else if (errno != ENOENT) goto done;
        json_object *exists = get(row, "existed");
        if (!exists || !json_object_is_type(exists, json_type_boolean)) goto done;
        existed[index] = json_object_get_boolean(exists);
        if (existed[index]) {
            const char *name = string(row, "file"); char source[PATH_MAX];
            if (!*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..") ||
                path_join(source, directory, name) < 0 || lstat(source, &metadata[index]) ||
                !S_ISREG(metadata[index].st_mode) || read_file(source, &backups[index], &lengths[index]) < 0) goto done;
        }
    }
    for (size_t i = 0; i < 4; i++) {
        if (existed[i]) {
            if (atomic_restore(paths[i], backups[i], lengths[i], &metadata[i]) < 0) goto done;
        } else if (unlink(paths[i]) && errno != ENOENT) goto done;
    }
    const char *reload[] = {"systemctl", "--user", "daemon-reload", NULL};
    const char *restart[] = {"systemctl", "--user", "restart", "pipewire.service", "pipewire-pulse.service", "wireplumber.service", NULL};
    result = run(reload, 15);
    if (!result) result = run(restart, 30);
    if (!result) puts("Perfil exclusivo retirado; configuração anterior restaurada.");
done:
    for (size_t i = 0; i < 4; i++) free(backups[i]);
    if (manifest) json_object_put(manifest);
    free(directory);
    return result;
}
static void usage(void)
{
    puts("Uso: audio_interno {inicializar|aplicar|desfazer} [--state-dir DIRETORIO]");
    puts("     audio_interno inicializar --dry-run [--state-dir DIRETORIO]");
    puts("Somente C4128A-14/ES8326. Execute como usuário normal, sem sudo.");
    puts("C porta o mixer, ativação e desfazer. Não instala os templates de configuração.");
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--help")) { usage(); return 0; }
    if (argc < 2) { usage(); return 2; }
    const char *action = argv[1]; const char *chosen_state = NULL; bool dry = false;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--dry-run")) dry = true;
        else if (!strcmp(argv[i], "--state-dir") && i + 1 < argc) chosen_state = argv[++i];
        else { usage(); return 2; }
    }
    if (strcmp(action, "inicializar") && strcmp(action, "aplicar") && strcmp(action, "desfazer")) { usage(); return 2; }
    if (dry && strcmp(action, "inicializar")) { usage(); return 2; }
    if (geteuid() == 0) { fprintf(stderr, "Execute como usuário normal, sem sudo.\n"); return 2; }
    umask(0077);
    if (chosen_state) {
        if (strlen(chosen_state) >= sizeof(state_dir)) return 2;
        strcpy(state_dir, chosen_state);
    } else {
        const char *xdg = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
        if (xdg && *xdg) { if (path_join(state_dir, xdg, "positivo-es8326") < 0) return 2; }
        else if (!home || snprintf(state_dir, sizeof(state_dir), "%s/.local/state/positivo-es8326", home) >= (int)sizeof(state_dir)) return 2;
    }
    if (!dry && safe_directory(state_dir, true) < 0) { fprintf(stderr, "Diretório de estado inseguro.\n"); return 1; }
    int result = !strcmp(action, "inicializar") ? initialize(dry) : !strcmp(action, "aplicar") ? apply() : undo();
    if (result < 0) { fprintf(stderr, "Não foi possível concluir: %s\n", strerror(-result)); return 1; }
    return 0;
}
