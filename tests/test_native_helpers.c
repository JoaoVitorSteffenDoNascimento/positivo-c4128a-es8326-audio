/* SPDX-License-Identifier: BSD-3-Clause */
#define main audio_program_main
#include "../src/audio_interno.c"
#undef main
#include <assert.h>

static void put(const char *path, const char *data)
{
    int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, 0600); assert(fd >= 0);
    assert(write(fd, data, strlen(data)) == (ssize_t)strlen(data)); assert(!close(fd));
}
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--wave")) return write_wave(argv[2]) < 0;
    if (argc == 2 && !strcmp(argv[1], "--child")) { for (int i=0; i<65536; i++) fputs("fixture output\n", stdout); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--sleeper")) { sleep(5); return 0; }
    char *output = NULL;
    const char *echo[] = {"/bin/echo", "one argument with spaces; $(literal)", NULL};
    assert(command(echo, 2, &output) == 0); assert(!strcmp(output, "one argument with spaces; $(literal)\n")); free(output);
    const char *child[] = {argv[0], "--child", NULL};
    assert(command(child, 5, &output) == 0 && strlen(output) == 65536 * 15); free(output);
    const char *sleeper[] = {argv[0], "--sleeper", NULL};
    assert(command(sleeper, 1, &output) == -ETIMEDOUT && !output);
    const char *failure[] = {"/bin/false", NULL}; assert(command(failure, 2, &output) < 0); free(output);
    puts("C commands: argument safety, pipe draining, failures and timeout passed");
    json_object *cards = json_tokener_parse("[{\"name\":\"" CARD "\",\"index\":0}]");
    json_object *card = find_card(cards); assert(card);
    json_object *sinks = json_tokener_parse("[{\"name\":\"alsa_output.test.vision14-speakers\",\"card\":\"0\",\"ports\":{\"" PORT "\":{}}}]");
    assert(find_speaker(sinks, card, true)); assert(exclusive_port(json_object_array_get_idx(sinks,0)));
    json_object_array_add(sinks, json_tokener_parse("{\"name\":\"alsa_output.test.hdmi-stereo\",\"card\":0}"));
    assert(!find_speaker(sinks, card, true)); assert(find_speaker(sinks,card,false));
    json_object_put(cards); json_object_put(sinks);
    puts("C profiles: unique card, exclusive speaker and dictionary ports passed");
    char temp[] = "/tmp/positivo-c-tests-XXXXXX"; assert(mkdtemp(temp));
    char home[PATH_MAX], backups_dir[PATH_MAX], bin[PATH_MAX], executable[PATH_MAX], path[PATH_MAX];
    assert(!path_join(home,temp,"home")); assert(!safe_directory(home,true));
    assert(!path_join(backups_dir,temp,"backups")); assert(!safe_directory(backups_dir,true));
    assert(!path_join(bin,temp,"bin")); assert(!safe_directory(bin,true));
    assert(!path_join(executable,bin,"systemctl")); put(executable,"#!/bin/sh\nexit 0\n"); assert(!chmod(executable,0700));
    assert(!path_join(state_dir,temp,"state")); assert(!safe_directory(state_dir,true));
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    char *old_path = getenv("PATH") ? strdup(getenv("PATH")) : NULL;
    assert(!setenv("HOME",home,1)); assert(!setenv("PATH",bin,1));
    const char *rel[] = {".config/wireplumber/wireplumber.conf.d/51-vision14-es8326.conf", ".config/systemd/user/wireplumber.service.d/51-vision14-speakers.conf", ".config/alsa-card-profile/vision14-interno.conf", ".config/alsa-card-profile/vision14-speaker.conf"};
    char targets[4][PATH_MAX], hash[65]; assert(!sha256("applied fixture",15,hash));
    json_object *manifest = json_object_new_object(), *files = json_object_new_array();
    json_object_object_add(manifest,"files",files);
    for (size_t i=0;i<4;i++) {
        assert(!path_join(targets[i],home,rel[i])); char parent[PATH_MAX]; strcpy(parent,targets[i]); *strrchr(parent,'/')=0;
        assert(!safe_directory(parent,true)); put(targets[i],"applied fixture");
        json_object *row = json_object_new_object();
        json_object_object_add(row,"path",json_object_new_string(targets[i]));
        json_object_object_add(row,"applied_sha256",json_object_new_string(hash));
        json_object_object_add(row,"existed",json_object_new_boolean(i<2));
        if (i<2) { char name[32]; snprintf(name,sizeof(name),"old-%zu.conf",i); assert(!path_join(path,backups_dir,name)); put(path,"original fixture"); assert(!chmod(path,i==0?0644:0600)); json_object_object_add(row,"file",json_object_new_string(name)); }
        json_object_array_add(files,row);
    }
    assert(!path_join(path,backups_dir,"manifest.json")); assert(!save_json(path,manifest));
    assert(!path_join(path,state_dir,"backup-audio-interno.txt")); char pointer[PATH_MAX+2]; snprintf(pointer,sizeof(pointer),"%s\n",backups_dir); put(path,pointer);
    put(targets[3],"later change"); assert(undo()<0);
    char *data; size_t length; assert(!read_file(targets[0],&data,&length)); assert(!strcmp(data,"applied fixture")); free(data);
    put(targets[3],"applied fixture"); assert(!undo());
    for (size_t i=0;i<2;i++) { struct stat st; assert(!read_file(targets[i],&data,&length)); assert(!strcmp(data,"original fixture")); free(data); assert(!stat(targets[i],&st)); assert((st.st_mode&0777)==(i==0?0644:0600)); }
    assert(access(targets[2],F_OK)<0 && access(targets[3],F_OK)<0);
    json_object_put(manifest);
    if(old_home) {setenv("HOME",old_home,1);free(old_home);} else unsetenv("HOME");
    if(old_path) {setenv("PATH",old_path,1);free(old_path);} else unsetenv("PATH");
    /* Cleanup only the known temporary fixture, without shell expansion. */
    const char *cleanup[] = {"/bin/rm","-r","--",temp,NULL}; assert(!run(cleanup,5));
    puts("C undo: conflict blocks all writes; original contents/modes restored; created files removed");
    return 0;
}
