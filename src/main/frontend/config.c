/***************************************************************************
    XML Configuration File Handling.

    Load & Save Hi-Scores.

    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

#include <stdio.h>
#include <compat/msvc.h>
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

#define MUSIC_CSV_FIELD_COUNT 4
#define MUSIC_CSV_FIELD_MAX 256
#define MUSIC_CSV_FILE_MAX (1024 * 1024)

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

static void config_csv_trim(char* value)
{
    char* start = value;
    size_t length;

    while (*start == ' ' || *start == '\t')
        start++;

    if (start != value)
        memmove(value, start, strlen(start) + 1);

    length = strlen(value);
    while (length > 0 &&
           (value[length - 1] == ' ' ||
            value[length - 1] == '\t'))
        value[--length] = '\0';
}

/* Read one RFC 4180-style record. Quoted fields may contain commas,
 * doubled quotes and line breaks. Blank lines are ignored. */
static int config_csv_next_record(
    const char** cursor,
    char fields[MUSIC_CSV_FIELD_COUNT][MUSIC_CSV_FIELD_MAX],
    unsigned* line,
    unsigned* record_line)
{
    const char* p = *cursor;
    int field;

    for (;;)
    {
        const char* blank = p;

        while (*blank == ' ' || *blank == '\t')
            blank++;

        if (!*blank)
        {
            *cursor = blank;
            return 0;
        }

        if (*blank != '\r' && *blank != '\n')
            break;

        p = blank;
        if (*p == '\r')
            p++;
        if (*p == '\n')
            p++;
        (*line)++;
    }

    if (!*p)
    {
        *cursor = p;
        return 0;
    }

    *record_line = *line;

    for (field = 0; field < MUSIC_CSV_FIELD_COUNT; field++)
    {
        size_t length = 0;

        fields[field][0] = '\0';

        if (*p == '"')
        {
            p++;

            for (;;)
            {
                char value;

                if (!*p)
                    return -1;

                if (*p == '"')
                {
                    if (p[1] == '"')
                    {
                        value = '"';
                        p += 2;
                    }
                    else
                    {
                        p++;
                        break;
                    }
                }
                else if (*p == '\r' || *p == '\n')
                {
                    value = '\n';
                    if (*p == '\r')
                        p++;
                    if (*p == '\n')
                        p++;
                    (*line)++;
                }
                else
                    value = *p++;

                if (length + 1 >= MUSIC_CSV_FIELD_MAX)
                    return -1;

                fields[field][length++] = value;
            }

            fields[field][length] = '\0';

            while (*p == ' ' || *p == '\t')
                p++;
        }
        else
        {
            while (*p && *p != ',' &&
                   *p != '\r' && *p != '\n')
            {
                if (*p == '"' ||
                    length + 1 >= MUSIC_CSV_FIELD_MAX)
                    return -1;

                fields[field][length++] = *p++;
            }

            fields[field][length] = '\0';
            config_csv_trim(fields[field]);
        }

        if (field + 1 < MUSIC_CSV_FIELD_COUNT)
        {
            if (*p != ',')
                return -1;
            p++;
        }
        else
        {
            bool has_line_ending = false;

            if (*p == ',')
                return -1;

            if (*p == '\r')
            {
                p++;
                if (*p == '\n')
                    p++;
                has_line_ending = true;
            }
            else if (*p == '\n')
            {
                p++;
                has_line_ending = true;
            }
            else if (*p)
                return -1;

            if (has_line_ending)
                (*line)++;
        }
    }

    *cursor = p;
    return 1;
}

static bool config_csv_parse_long(const char* value, long* result)
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

bool Config_load_custom_music(Config* self, const char* filename)
{
    RFILE* file;
    int64_t file_length;
    char* file_data;
    const char* cursor;
    char fields[MUSIC_CSV_FIELD_COUNT][MUSIC_CSV_FIELD_MAX];
    unsigned line = 1;
    unsigned record_line = 1;
    unsigned row = 0;
    unsigned loaded_tracks = 0;
    int result;

    /* Retain the three original arcade tracks across content reloads. */
    self->sound.music_num = 3;

    file = filestream_open(
        filename,
        RETRO_VFS_FILE_ACCESS_READ,
        RETRO_VFS_FILE_ACCESS_HINT_NONE);

    /* A missing music.csv is valid and means arcade music only. */
    if (!file)
        return false;

    file_length = filestream_get_size(file);
    if (file_length < 0 || file_length > MUSIC_CSV_FILE_MAX)
    {
        if (log_cb)
            log_cb(
                RETRO_LOG_ERROR,
                "[Cannonball]: Invalid custom music CSV size: %s\n",
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
                "[Cannonball]: Not enough memory to read custom music CSV: %s\n",
                filename);
        filestream_close(file);
        return false;
    }

    if (filestream_read(file, file_data, file_length) != file_length)
    {
        if (log_cb)
            log_cb(
                RETRO_LOG_ERROR,
                "[Cannonball]: Could not read custom music CSV: %s\n",
                filename);
        filestream_close(file);
        free(file_data);
        return false;
    }

    filestream_close(file);
    file_data[file_length] = '\0';

    if (memchr(file_data, '\0', (size_t)file_length))
        goto invalid_csv;

    cursor = file_data;

    if (file_length >= 3 &&
        (unsigned char)cursor[0] == 0xef &&
        (unsigned char)cursor[1] == 0xbb &&
        (unsigned char)cursor[2] == 0xbf)
        cursor += 3;

    result = config_csv_next_record(
        &cursor, fields, &line, &record_line);

    if (result != 1 ||
        strcmp(fields[0], "enabled") != 0 ||
        strcmp(fields[1], "volume") != 0 ||
        strcmp(fields[2], "title") != 0 ||
        strcmp(fields[3], "filename") != 0)
        goto invalid_csv;

    while ((result = config_csv_next_record(
                &cursor, fields, &line, &record_line)) == 1)
    {
        long enabled;
        long volume = 100;
        music_t* music;

        row++;

        if (!config_csv_parse_long(fields[0], &enabled) ||
            (enabled != 0 && enabled != 1))
            goto invalid_csv;

        if (fields[1][0] &&
            !config_csv_parse_long(fields[1], &volume))
            goto invalid_csv;

        if (volume < 0)
            volume = 0;
        else if (volume > 300)
            volume = 300;

        if (!enabled)
            continue;

        if (self->sound.music_num >= MUSIC_TRACK_MAX)
        {
            if (log_cb)
                log_cb(
                    RETRO_LOG_WARN,
                    "[Cannonball]: Custom music track limit reached; "
                    "ignoring row %u in %s\n",
                    row,
                    filename);
            continue;
        }

        music = &self->sound.music[self->sound.music_num];
        music->cmd = SOUND_MUSIC_CUSTOM;

        if (fields[2][0])
            snprintf(
                music->title,
                sizeof(music->title),
                "%s",
                fields[2]);
        else
            snprintf(
                music->title,
                sizeof(music->title),
                "TRACK %u",
                row);

        if (fields[3][0])
            snprintf(
                music->filename,
                sizeof(music->filename),
                "%s",
                fields[3]);
        else
            snprintf(
                music->filename,
                sizeof(music->filename),
                "track%u.wav",
                row);

        music->type = config_music_is_wav(music->filename)
            ? IS_WAV
            : IS_YM_EXT;

        music->volume = music->type == IS_WAV
            ? (uint16_t)volume
            : 100;

        self->sound.music_num++;
        loaded_tracks++;
    }

    if (result < 0)
        goto invalid_csv;

    if (log_cb)
        log_cb(
            RETRO_LOG_INFO,
            "[Cannonball]: Loaded %u custom music track(s) from %s\n",
            loaded_tracks,
            filename);

    free(file_data);
    return true;

invalid_csv:
    self->sound.music_num = 3;

    if (log_cb)
        log_cb(
            RETRO_LOG_ERROR,
            "[Cannonball]: Invalid custom music CSV at line %u: %s\n",
            record_line,
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
