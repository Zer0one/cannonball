/***************************************************************************
    Configuration and Hi-Score Handling.

    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

#include <stdio.h>
#include <compat/msvc.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h> /* remove() */

#include <streams/file_stream.h>

#include <libretro.h>

#include "../main.h"
#include "config.h"
#include "../globals.h"
#include "../setup.h"
#include "../utils.h"

#include "../engine/ohiscore.h"

enum { COUNTER_1M_15 = 0x11D0 };
#include "../engine/audio/osoundint.h"

extern retro_log_printf_t                 log_cb;

Config config;

#define MUSIC_LIST_VALUE_MAX 256
#define MUSIC_LIST_FILE_MAX (1024 * 1024)

enum config_music_section
{
    CONFIG_MUSIC_SECTION_NONE,
    CONFIG_MUSIC_SECTION_MUSIC,
    CONFIG_MUSIC_SECTION_TRACK
};

typedef struct
{
    unsigned number;
    unsigned line;
    bool enabled;
    bool enabled_set;
    bool volume_set;
    bool title_set;
    bool filename_set;
    long volume;
    char title[MUSIC_LIST_VALUE_MAX];
    char filename[MUSIC_LIST_VALUE_MAX];
} config_music_track;

void Config_ctor(Config* self)
{
    /* Safe defaults required before Config_set_fps(Config* self) initializes audio. */
    self->sound.rate        = 44100;
    self->sound.music_timer = MUSIC_TIMER;
    self->video.shadow      = 0;
    self->engine.hiscore_delete = true;
    self->engine.hiscore_timer  = HIGHSCORE_TIMER;
    self->engine.grippy_tyres   = false;
    self->engine.offroad        = false;
    self->engine.bumper         = false;
    self->engine.turbo          = false;
    self->engine.car_pal        = 0;

    self->sound.music[0].type = IS_YM_INT;
    self->sound.music[0].cmd  = SOUND_MUSIC_MAGICAL;
    self->sound.music[0].volume = 100;
    strcpy(self->sound.music[0].title, "MAGICAL SOUND SHOWER");
    self->sound.music[0].filename[0] = 0;

    self->sound.music[1].type = IS_YM_INT;
    self->sound.music[1].cmd  = SOUND_MUSIC_BREEZE;
    self->sound.music[1].volume = 100;
    strcpy(self->sound.music[1].title, "PASSING BREEZE");
    self->sound.music[1].filename[0] = 0;

    self->sound.music[2].type = IS_YM_INT;
    self->sound.music[2].cmd  = SOUND_MUSIC_SPLASH;
    self->sound.music[2].volume = 100;
    strcpy(self->sound.music[2].title, "SPLASH WAVE");
    self->sound.music[2].filename[0] = 0;

    self->sound.music_num = 3;
}

static char* config_list_trim(char* value)
{
    char* start = value;
    size_t length;

    while (*start == ' ' || *start == '\t')
        start++;

    length = strlen(start);
    while (length > 0 &&
           (start[length - 1] == ' ' ||
            start[length - 1] == '\t'))
        start[--length] = '\0';

    return start;
}

static bool config_list_parse_long(const char* value, long* result)
{
    char* end;
    long parsed;

    if (!value[0])
        return false;

    parsed = strtol(value, &end, 10);
    if (*end)
        return false;

    *result = parsed;
    return true;
}

static bool config_list_parse_bool(const char* value, bool* result)
{
    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0)
        *result = true;
    else if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0)
        *result = false;
    else
        return false;

    return true;
}

/* Values follow the RetroArch-style key = value convention. Unquoted
 * values end at '#'. Quoted values support escaped quotes and backslashes. */
static bool config_list_parse_value(char* text, char** value)
{
    char* input = config_list_trim(text);

    if (*input == '"')
    {
        char* output = input;

        input++;
        while (*input && *input != '"')
        {
            if (*input == '\\')
            {
                input++;
                if (*input != '\\' && *input != '"')
                    return false;
            }
            *output++ = *input++;
        }

        if (*input != '"')
            return false;

        input++;
        *output = '\0';
        input = config_list_trim(input);

        if (*input && *input != '#')
            return false;

        *value = config_list_trim(text);
        return true;
    }
    else
    {
        char* comment = strchr(input, '#');

        if (comment)
            *comment = '\0';

        if (strchr(input, '"'))
            return false;

        *value = config_list_trim(input);
        return true;
    }
}

static bool config_list_parse_track_section(
    const char* section,
    unsigned* number)
{
    const char* digit;
    unsigned parsed = 0;

    if (strncmp(section, "track", 5) != 0)
        return false;

    digit = section + 5;
    if (!*digit)
        return false;

    while (*digit)
    {
        unsigned value;

        if (*digit < '0' || *digit > '9')
            return false;

        value = (unsigned)(*digit - '0');
        if (parsed > (UINT_MAX - value) / 10)
            return false;

        parsed = parsed * 10 + value;
        digit++;
    }

    if (!parsed)
        return false;

    *number = parsed;
    return true;
}

static void config_music_track_init(
    config_music_track* track,
    unsigned number,
    unsigned line)
{
    memset(track, 0, sizeof(*track));
    track->number = number;
    track->line = line;
    track->volume = 100;
}

static bool config_music_is_wav(const char* filename)
{
    size_t length = strlen(filename);
    const char* extension;

    if (length < 4)
        return false;

    extension = filename + length - 4;
    return
        extension[0] == '.' &&
        (extension[1] == 'w' || extension[1] == 'W') &&
        (extension[2] == 'a' || extension[2] == 'A') &&
        (extension[3] == 'v' || extension[3] == 'V');
}

static bool config_music_add_track(
    Config* self,
    const config_music_track* track,
    const char* filename,
    unsigned* loaded_tracks)
{
    music_t* music;

    if (!track->enabled_set)
        return false;

    if (!track->enabled)
        return true;

    if (self->sound.music_num >= MUSIC_TRACK_MAX)
    {
        if (log_cb)
            log_cb(
                RETRO_LOG_WARN,
                "[Cannonball]: Custom music track limit reached; "
                "ignoring [track%u] in %s\n",
                track->number,
                filename);
        return true;
    }

    music = &self->sound.music[self->sound.music_num];
    music->cmd = SOUND_MUSIC_CUSTOM;

    if (track->title[0])
        snprintf(music->title, sizeof(music->title), "%s", track->title);
    else
        snprintf(
            music->title,
            sizeof(music->title),
            "TRACK %u",
            track->number);

    if (track->filename[0])
        snprintf(
            music->filename,
            sizeof(music->filename),
            "%s",
            track->filename);
    else
        snprintf(
            music->filename,
            sizeof(music->filename),
            "track%u.wav",
            track->number);

    music->type = config_music_is_wav(music->filename)
        ? IS_WAV
        : IS_YM_EXT;

    music->volume = music->type == IS_WAV
        ? (uint16_t)track->volume
        : 100;

    self->sound.music_num++;
    (*loaded_tracks)++;
    return true;
}

bool Config_load_custom_music(Config* self, const char* filename)
{
    RFILE* file;
    int64_t file_length;
    char* file_data;
    char* cursor;
    enum config_music_section current_section = CONFIG_MUSIC_SECTION_NONE;
    config_music_track track;
    unsigned line = 1;
    unsigned error_line = 1;
    unsigned last_track = 0;
    unsigned loaded_tracks = 0;
    bool music_section_set = false;
    bool version_set = false;

    /* Retain the three original arcade tracks across content reloads. */
    self->sound.music_num = 3;

    file = filestream_open(
        filename,
        RETRO_VFS_FILE_ACCESS_READ,
        RETRO_VFS_FILE_ACCESS_HINT_NONE);

    /* A missing music.list is valid and means arcade music only. */
    if (!file)
        return false;

    file_length = filestream_get_size(file);
    if (file_length < 0 || file_length > MUSIC_LIST_FILE_MAX)
    {
        if (log_cb)
            log_cb(
                RETRO_LOG_ERROR,
                "[Cannonball]: Invalid custom music list size: %s\n",
                filename);
        filestream_close(file);
        return false;
    }

    file_data = (char*)malloc((size_t)file_length + 1);
    if (!file_data)
    {
        if (log_cb)
            log_cb(
                RETRO_LOG_ERROR,
                "[Cannonball]: Not enough memory to read custom music list: %s\n",
                filename);
        filestream_close(file);
        return false;
    }

    if (filestream_read(file, file_data, file_length) != file_length)
    {
        if (log_cb)
            log_cb(
                RETRO_LOG_ERROR,
                "[Cannonball]: Could not read custom music list: %s\n",
                filename);
        filestream_close(file);
        free(file_data);
        return false;
    }

    filestream_close(file);
    file_data[file_length] = '\0';

    if (memchr(file_data, '\0', (size_t)file_length))
        goto invalid_list;

    cursor = file_data;

    if (file_length >= 3 &&
        (unsigned char)cursor[0] == 0xef &&
        (unsigned char)cursor[1] == 0xbb &&
        (unsigned char)cursor[2] == 0xbf)
        cursor += 3;

    while (*cursor)
    {
        char* text = cursor;
        char* end = cursor;

        while (*end && *end != '\r' && *end != '\n')
            end++;

        if (*end)
        {
            char ending = *end;

            *end++ = '\0';
            if (ending == '\r' && *end == '\n')
                end++;
        }

        cursor = end;
        text = config_list_trim(text);
        error_line = line;

        if (*text && *text != '#')
        {
            if (*text == '[')
            {
                char* close = strchr(text + 1, ']');
                char* section;
                char* trailing;
                unsigned track_number;

                if (!close)
                    goto invalid_list;

                *close = '\0';
                section = config_list_trim(text + 1);
                trailing = config_list_trim(close + 1);

                if (!*section || (*trailing && *trailing != '#'))
                    goto invalid_list;

                if (current_section == CONFIG_MUSIC_SECTION_TRACK &&
                    !config_music_add_track(
                        self,
                        &track,
                        filename,
                        &loaded_tracks))
                {
                    error_line = track.line;
                    goto invalid_list;
                }

                if (strcmp(section, "music") == 0)
                {
                    if (music_section_set || last_track)
                        goto invalid_list;

                    music_section_set = true;
                    current_section = CONFIG_MUSIC_SECTION_MUSIC;
                }
                else if (config_list_parse_track_section(
                             section,
                             &track_number))
                {
                    if (!music_section_set || !version_set ||
                        track_number <= last_track)
                        goto invalid_list;

                    last_track = track_number;
                    config_music_track_init(&track, track_number, line);
                    current_section = CONFIG_MUSIC_SECTION_TRACK;
                }
                else
                    goto invalid_list;
            }
            else
            {
                char* equals = strchr(text, '=');
                char* key;
                char* value;

                if (!equals)
                    goto invalid_list;

                *equals = '\0';
                key = config_list_trim(text);
                if (!*key || !config_list_parse_value(equals + 1, &value))
                    goto invalid_list;

                if (current_section == CONFIG_MUSIC_SECTION_MUSIC)
                {
                    if (strcmp(key, "version") == 0)
                    {
                        long version;

                        if (version_set ||
                            !config_list_parse_long(value, &version) ||
                            version != 1)
                            goto invalid_list;

                        version_set = true;
                    }
                    else if (log_cb)
                        log_cb(
                            RETRO_LOG_WARN,
                            "[Cannonball]: Ignoring unknown key '%s' "
                            "in [music] at line %u: %s\n",
                            key,
                            line,
                            filename);
                }
                else if (current_section == CONFIG_MUSIC_SECTION_TRACK)
                {
                    if (strcmp(key, "enabled") == 0)
                    {
                        if (track.enabled_set ||
                            !config_list_parse_bool(value, &track.enabled))
                            goto invalid_list;
                        track.enabled_set = true;
                    }
                    else if (strcmp(key, "volume") == 0)
                    {
                        long volume = 100;

                        if (track.volume_set ||
                            (*value &&
                             !config_list_parse_long(value, &volume)))
                            goto invalid_list;

                        if (volume < 0)
                            volume = 0;
                        else if (volume > 300)
                            volume = 300;

                        track.volume = volume;
                        track.volume_set = true;
                    }
                    else if (strcmp(key, "title") == 0)
                    {
                        if (track.title_set ||
                            strlen(value) >= sizeof(track.title))
                            goto invalid_list;

                        snprintf(track.title, sizeof(track.title), "%s", value);
                        track.title_set = true;
                    }
                    else if (strcmp(key, "filename") == 0)
                    {
                        if (track.filename_set ||
                            strlen(value) >= sizeof(track.filename))
                            goto invalid_list;

                        snprintf(
                            track.filename,
                            sizeof(track.filename),
                            "%s",
                            value);
                        track.filename_set = true;
                    }
                    else if (log_cb)
                        log_cb(
                            RETRO_LOG_WARN,
                            "[Cannonball]: Ignoring unknown key '%s' "
                            "in [track%u] at line %u: %s\n",
                            key,
                            track.number,
                            line,
                            filename);
                }
                else
                    goto invalid_list;
            }
        }

        line++;
    }

    if (current_section == CONFIG_MUSIC_SECTION_TRACK &&
        !config_music_add_track(
            self,
            &track,
            filename,
            &loaded_tracks))
    {
        error_line = track.line;
        goto invalid_list;
    }

    if (!music_section_set || !version_set)
        goto invalid_list;

    if (log_cb)
        log_cb(
            RETRO_LOG_INFO,
            "[Cannonball]: Loaded %u custom music track(s) from %s\n",
            loaded_tracks,
            filename);

    free(file_data);
    return true;

invalid_list:
    self->sound.music_num = 3;

    if (log_cb)
        log_cb(
            RETRO_LOG_ERROR,
            "[Cannonball]: Invalid custom music list at line %u: %s\n",
            error_line,
            filename);

    free(file_data);
    return false;
}



static RFILE* config_open_read(const char* filename)
{
    char path[600];
    snprintf(path, sizeof(path), "%s.sav", filename);
    return filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
                           RETRO_VFS_FILE_ACCESS_HINT_NONE);
}

static RFILE* config_open_write(const char* filename)
{
    char path[600];
    snprintf(path, sizeof(path), "%s.sav", filename);
    return filestream_open(path, RETRO_VFS_FILE_ACCESS_WRITE,
                           RETRO_VFS_FILE_ACCESS_HINT_NONE);
}

/* High scores are persisted as a small binary blob in the save */
/* directory. Every field is stored little-endian behind a 4-byte magic */
/* and 1-byte version, so a .sav is portable across platforms regardless */
/* of host endianness or struct padding. Settings themselves come from */
/* the libretro core options, so no XML configuration is loaded/written. */

#define CONFIG_SAV_VERSION 1

static void config_write_u8(RFILE* f, uint8_t v)
{
    filestream_write(f, &v, 1);
}

static void config_write_u16(RFILE* f, uint16_t v)
{
    uint8_t b[2];
    b[0] = (uint8_t)(v & 0xff);
    b[1] = (uint8_t)((v >> 8) & 0xff);
    filestream_write(f, b, 2);
}

static void config_write_u32(RFILE* f, uint32_t v)
{
    uint8_t b[4];
    b[0] = (uint8_t)(v & 0xff);
    b[1] = (uint8_t)((v >> 8) & 0xff);
    b[2] = (uint8_t)((v >> 16) & 0xff);
    b[3] = (uint8_t)((v >> 24) & 0xff);
    filestream_write(f, b, 4);
}

static uint8_t config_read_u8(RFILE* f)
{
    uint8_t v = 0;
    filestream_read(f, &v, 1);
    return v;
}

static uint16_t config_read_u16(RFILE* f)
{
    uint8_t b[2];
    b[0] = 0; b[1] = 0;
    filestream_read(f, b, 2);
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static uint32_t config_read_u32(RFILE* f)
{
    uint8_t b[4];
    b[0] = 0; b[1] = 0; b[2] = 0; b[3] = 0;
    filestream_read(f, b, 4);
    return  (uint32_t)b[0]        | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void config_write_header(RFILE* f, char m0, char m1, char m2, char m3)
{
    config_write_u8(f, (uint8_t)m0);
    config_write_u8(f, (uint8_t)m1);
    config_write_u8(f, (uint8_t)m2);
    config_write_u8(f, (uint8_t)m3);
    config_write_u8(f, CONFIG_SAV_VERSION);
}

static bool config_check_header(RFILE* f, char m0, char m1, char m2, char m3)
{
    uint8_t h[5];
    h[0] = 0; h[1] = 0; h[2] = 0; h[3] = 0; h[4] = 0;
    filestream_read(f, h, 5);
    return h[0] == (uint8_t)m0 && h[1] == (uint8_t)m1 &&
           h[2] == (uint8_t)m2 && h[3] == (uint8_t)m3 &&
           h[4] == CONFIG_SAV_VERSION;
}

void Config_load_scores(Config* self, const char* filename)
{
    int    i;
    RFILE* f = config_open_read(filename);
    if (!f)
        return;
    if (!config_check_header(f, 'C', 'B', 'H', 'S'))
    {
        filestream_close(f);
        return;
    }
    for (i = 0; i < NO_SCORES; i++)
    {
        score_entry* e = &ohiscore.scores[i];
        e->score    = config_read_u32(f);
        e->initial1 = config_read_u8(f);
        e->initial2 = config_read_u8(f);
        e->initial3 = config_read_u8(f);
        e->maptiles = config_read_u32(f);
        e->time     = config_read_u16(f);
    }
    filestream_close(f);
}

void Config_save_scores(Config* self, const char* filename)
{
    int    i;
    RFILE* f = config_open_write(filename);
    if (!f)
        return;
    config_write_header(f, 'C', 'B', 'H', 'S');
    for (i = 0; i < NO_SCORES; i++)
    {
        score_entry* e = &ohiscore.scores[i];
        config_write_u32(f, e->score);
        config_write_u8(f, e->initial1);
        config_write_u8(f, e->initial2);
        config_write_u8(f, e->initial3);
        config_write_u32(f, e->maptiles);
        config_write_u16(f, e->time);
    }
    filestream_close(f);
}

void Config_load_tiletrial_scores(Config* self)
{
    int    i;
    RFILE* f = config_open_read(FILENAME_TTRIAL);
    if (f && config_check_header(f, 'C', 'B', 'T', 'T'))
    {
        for (i = 0; i < 15; i++)
            self->ttrial.best_times[i] = config_read_u16(f);
        filestream_close(f);
        return;
    }
    if (f)
        filestream_close(f);
    for (i = 0; i < 15; i++)
        self->ttrial.best_times[i] = COUNTER_1M_15;
}

void Config_save_tiletrial_scores(Config* self)
{
    int    i;
    RFILE* f = config_open_write(FILENAME_TTRIAL);
    if (!f)
        return;
    config_write_header(f, 'C', 'B', 'T', 'T');
    for (i = 0; i < 15; i++)
        config_write_u16(f, self->ttrial.best_times[i]);
    filestream_close(f);
}

bool Config_clear_scores(Config* self)
{
    char path[600];
    bool files_removed = false;

    OHiScore_init_def_scores(&ohiscore);

    /* remove() returns 0 on success */
    snprintf(path, sizeof(path), "%s.sav", FILENAME_SCORES);
    if (!remove(path))
        files_removed = true;
    snprintf(path, sizeof(path), "%s.sav", FILENAME_TTRIAL);
    if (!remove(path))
        files_removed = true;
    snprintf(path, sizeof(path), "%s.sav", FILENAME_CONT);
    if (!remove(path))
        files_removed = true;

    return files_removed;
}

void Config_set_fps(Config* self, int fps)
{
    self->video.fps = fps;
    /* Set core FPS to 30fps, 60fps or 120fps */
    if (self->video.fps == 0)
        self->fps = 30;
    else if (self->video.fps == 3)
        self->fps = 120;
    else
        self->fps = 60;
    
    /* Original game ticks sprites at 30fps but background scroll at 60fps */
    if (self->video.fps == 3)
        self->tick_fps = 120;
    else if (self->video.fps < 2)
        self->tick_fps = 30;
    else
        self->tick_fps = 60;

    if (config.sound.enabled)
        Audio_stop_audio(&cannonball_audio);
    OSoundInt_init(&osoundint);
    if (config.sound.enabled)
        Audio_start_audio(&cannonball_audio);
}
