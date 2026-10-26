/* User settings (settings.json in the data folder).
 * SPDX-License-Identifier: GPL-3.0-only */
#include "app.h"
#include "parson.h"
#include <stdio.h>
#include <string.h>

Settings S;
/* logical keys A B X Y L1 R1 L2 R2 START SELECT -> physical id
 * (joystick button number; 100/101 = L2/R2 analog triggers) */
static const int DEFAULT_KEYS[KM_COUNT] = {1, 0, 3, 2, 4, 5, 100, 101, 7, 6};

void Settings_defaultKeys(void) { memcpy(S.keymap, DEFAULT_KEYS, sizeof(S.keymap)); }
static void path(char *out, size_t n) { snprintf(out, n, "%s/settings.json", paths.data); }

void Settings_load(void) {
    memset(&S, 0, sizeof(S));
    S.autoplay = true; S.prefetch = true; S.telex = true; S.audio_out = 0; S.av_offset_ms = 0; S.dac_volume = 12;
    S.sub_mode = 0; S.sub_size = 1; S.ota_auto = true;   /* subtitles: auto, medium, in the app's language */
    Settings_defaultKeys();
    char p[1200]; path(p, sizeof(p));
    JSON_Value *root = json_parse_file(p);
    JSON_Object *o = json_value_get_object(root);
    if (o) {
        if (json_object_has_value(o, "lang")) S.lang = (int)json_object_get_number(o, "lang");
        if (json_object_has_value(o, "av_offset_ms")) S.av_offset_ms = (int)json_object_get_number(o, "av_offset_ms");
        if (json_object_has_value(o, "audio_out")) S.audio_out = (int)json_object_get_number(o, "audio_out");
        if (json_object_has_value(o, "dac_volume")) S.dac_volume = (int)json_object_get_number(o, "dac_volume");
        if (json_object_has_value(o, "home_mode")) S.home_mode = (int)json_object_get_number(o, "home_mode");
        if (json_object_has_value(o, "autoplay")) S.autoplay = json_object_get_boolean(o, "autoplay") == 1;
        if (json_object_has_value(o, "prefetch")) S.prefetch = json_object_get_boolean(o, "prefetch") == 1;
        if (json_object_has_value(o, "telex")) S.telex = json_object_get_boolean(o, "telex") == 1;
        if (json_object_has_value(o, "bottom_dac")) S.bottom_dac = json_object_get_boolean(o, "bottom_dac") == 1;
        if (json_object_has_value(o, "ota_auto")) S.ota_auto = json_object_get_boolean(o, "ota_auto") == 1;
        if (json_object_has_value(o, "sub_mode")) S.sub_mode = (int)json_object_get_number(o, "sub_mode");
        if (json_object_has_value(o, "sub_size")) S.sub_size = (int)json_object_get_number(o, "sub_size");
        const char *sl = json_object_get_string(o, "sub_lang");
        if (sl) snprintf(S.sub_lang, sizeof(S.sub_lang), "%s", sl);
        JSON_Array *k = json_object_get_array(o, "keymap");
        if (k && json_array_get_count(k) == KM_COUNT) for (int i = 0; i < KM_COUNT; i++) S.keymap[i] = (int)json_array_get_number(k, i);
    }
    json_value_free(root);
    if (S.lang < 0 || S.lang > 1) S.lang = 0;
    if (S.dac_volume < 0 || S.dac_volume > 20) S.dac_volume = 12;
    if (S.audio_out < 0 || S.audio_out > 2) S.audio_out = 0;
    if (S.sub_mode < 0 || S.sub_mode > 2) S.sub_mode = 0;
    if (S.sub_size < 0 || S.sub_size > 2) S.sub_size = 1;
    if (S.av_offset_ms < -400 || S.av_offset_ms > 400) S.av_offset_ms = 0;
}
void Settings_save(void) {
    JSON_Value *root = json_value_init_object(); JSON_Object *o = json_value_get_object(root);
    json_object_set_number(o, "lang", S.lang);
    json_object_set_number(o, "av_offset_ms", S.av_offset_ms);
    json_object_set_number(o, "audio_out", S.audio_out);
    json_object_set_number(o, "home_mode", S.home_mode);
    json_object_set_number(o, "dac_volume", S.dac_volume);
    json_object_set_boolean(o, "autoplay", S.autoplay);
    json_object_set_boolean(o, "prefetch", S.prefetch);
    json_object_set_boolean(o, "telex", S.telex);
    json_object_set_boolean(o, "bottom_dac", S.bottom_dac);
    json_object_set_boolean(o, "ota_auto", S.ota_auto);
    json_object_set_number(o, "sub_mode", S.sub_mode);
    json_object_set_number(o, "sub_size", S.sub_size);
    json_object_set_string(o, "sub_lang", S.sub_lang);
    JSON_Value *k = json_value_init_array();
    for (int i = 0; i < KM_COUNT; i++) json_array_append_number(json_value_get_array(k), S.keymap[i]);
    json_object_set_value(o, "keymap", k);
    char p[1200], tmp[1300]; path(p, sizeof(p)); snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    if (json_serialize_to_file_pretty(root, tmp) == JSONSuccess) rename(tmp, p);
    json_value_free(root);
}
