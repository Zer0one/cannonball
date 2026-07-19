/***************************************************************************
    XML Configuration File Handling.

    Load & Save Hi-Scores.

    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

// see: http://www.boost.org/doc/libs/1_52_0/doc/html/boost_propertytree/tutorial.html
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-builtins"
#endif
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <pugixml/pugixml.hpp>

#include <libretro.h>

#include "main.hpp"
#include "config.hpp"
#include "globals.hpp"
#ifdef __LIBRETRO__
#include "lr_setup.hpp"
#else
#include "setup.hpp"
#endif
#include "../utils.hpp"

#include "engine/ohiscore.hpp"
#include "engine/audio/osoundint.hpp"

extern retro_log_printf_t                 log_cb;

#ifdef __PS3__
#define remove std::remove
#endif


// api change in boost 1.56
#if (BOOST_VERSION >= 105600)
typedef boost::property_tree::xml_writer_settings<std::string> xml_writer_settings;
#else
typedef boost::property_tree::xml_writer_settings<char> xml_writer_settings;
#endif

Config config;

Config::Config(void)
{
    // Safe defaults required before Config::set_fps() initializes audio.
    sound.rate        = 44100;
    sound.music_timer = MUSIC_TIMER;
    video.shadow      = 0;
    engine.hiscore_delete = true;
    engine.hiscore_timer  = HIGHSCORE_TIMER;
    engine.grippy_tyres   = false;
    engine.offroad        = false;
    engine.bumper         = false;
    engine.turbo          = false;
    engine.car_pal        = 0;

    music_t magical;
    magical.type  = music_t::IS_YM_INT;
    magical.cmd   = sound::MUSIC_MAGICAL;
    magical.title = "MAGICAL SOUND SHOWER";

    music_t breeze;
    breeze.type  = music_t::IS_YM_INT;
    breeze.cmd   = sound::MUSIC_BREEZE;
    breeze.title = "PASSING BREEZE";

    music_t splash;
    splash.type  = music_t::IS_YM_INT;
    splash.cmd   = sound::MUSIC_SPLASH;
    splash.title = "SPLASH WAVE";

    sound.music.push_back(magical);
    sound.music.push_back(breeze);
    sound.music.push_back(splash);
}


Config::~Config(void)
{
}

void Config::init()
{

}

void Config::load_custom_music(const std::string& filename)
{
    // Remove custom tracks added by any previous content load,
    // while retaining the three original arcade tracks.
    const size_t original_track_count = 3;

    if (sound.music.size() > original_track_count)
        sound.music.resize(original_track_count);

    pugi::xml_document document;

    const pugi::xml_parse_result result =
        document.load_file(
            filename.c_str(),
            pugi::parse_default |
                pugi::parse_trim_pcdata);

    if (!result)
    {
        if (log_cb)
            log_cb(
                RETRO_LOG_WARN,
                "[Cannonball]: Could not read custom music configuration "
                "%s: %s\n",
                filename.c_str(),
                result.description());

        return;
    }

    const pugi::xml_node custom_music =
        document
            .child("sound")
            .child("custom_music");

    unsigned loaded_tracks = 0;

    // Scan track1, track2, track3... until the first entry
    // without an enabled attribute.
    for (int i = 0; ; i++)
    {
        const std::string track_number =
            Utils::to_string(i + 1);

        const std::string track_name =
            "track" + track_number;

        const pugi::xml_node track =
            custom_music.child(track_name.c_str());

        const pugi::xml_attribute enabled =
            track.attribute("enabled");

        if (!enabled)
            break;

        if (enabled.as_int() != 1)
            continue;

        music_t music;

        const std::string default_title =
            "TRACK " + track_number;

        const std::string default_filename =
            "track" + track_number + ".wav";

        music.title =
            track
                .child("title")
                .text()
                .as_string(default_title.c_str());

        music.filename =
            track
                .child("filename")
                .text()
                .as_string(default_filename.c_str());

        const size_t filename_length =
            music.filename.length();

        const bool is_wav =
            filename_length >= 4 &&
            (
                music.filename.compare(
                    filename_length - 4,
                    4,
                    ".wav") == 0 ||
                music.filename.compare(
                    filename_length - 4,
                    4,
                    ".WAV") == 0
            );

        music.type = is_wav
            ? music_t::IS_WAV
            : music_t::IS_YM_EXT;

        music.cmd = sound::MUSIC_CUSTOM;

        sound.music.push_back(music);
        loaded_tracks++;
    }

    if (log_cb)
        log_cb(
            RETRO_LOG_INFO,
            "[Cannonball]: Loaded %u custom music track(s) "
            "from %s\n",
            loaded_tracks,
            filename.c_str());
}

using boost::property_tree::ptree;
ptree pt_config;

void Config::load_scores(const std::string &filename)
{
    // Create empty property tree object
    ptree pt;

    try
    {
        read_xml(engine.jap ? filename + "_jap.xml" : filename + ".xml" , pt, boost::property_tree::xml_parser::trim_whitespace);
    }
    catch (std::exception &e)
    {
        e.what();
        return;
    }
    
    // Game Scores
    for (int i = 0; i < ohiscore.NO_SCORES; i++)
    {
        score_entry* e = &ohiscore.scores[i];
        
        std::string xmltag = "score";
        xmltag += Utils::to_string(i);  
    
        e->score    = Utils::from_hex_string(pt.get<std::string>(xmltag + ".score",    "0"));
        e->initial1 = pt.get(xmltag + ".initial1", ".")[0];
        e->initial2 = pt.get(xmltag + ".initial2", ".")[0];
        e->initial3 = pt.get(xmltag + ".initial3", ".")[0];
        e->maptiles = Utils::from_hex_string(pt.get<std::string>(xmltag + ".maptiles", "20202020"));
        e->time     = Utils::from_hex_string(pt.get<std::string>(xmltag + ".time"    , "0")); 

        if (e->initial1 == '.') e->initial1 = 0x20;
        if (e->initial2 == '.') e->initial2 = 0x20;
        if (e->initial3 == '.') e->initial3 = 0x20;
    }
}

void Config::save_scores(const std::string &filename)
{
    // Create empty property tree object
    ptree pt;
        
    for (int i = 0; i < ohiscore.NO_SCORES; i++)
    {
        score_entry* e = &ohiscore.scores[i];
    
        std::string xmltag = "score";
        xmltag += Utils::to_string(i);    
        
        pt.put(xmltag + ".score",    Utils::to_hex_string(e->score));
        pt.put(xmltag + ".initial1", e->initial1 == 0x20 ? "." : Utils::to_string((char) e->initial1)); // use . to represent space
        pt.put(xmltag + ".initial2", e->initial2 == 0x20 ? "." : Utils::to_string((char) e->initial2));
        pt.put(xmltag + ".initial3", e->initial3 == 0x20 ? "." : Utils::to_string((char) e->initial3));
        pt.put(xmltag + ".maptiles", Utils::to_hex_string(e->maptiles));
        pt.put(xmltag + ".time",     Utils::to_hex_string(e->time));
    }
    
    try
    {
        write_xml(engine.jap ? filename + "_jap.xml" : filename + ".xml", pt, std::locale(), xml_writer_settings('\t', 1)); // Tab space 1
    }
    catch (std::exception &e)
    {
        log_cb(RETRO_LOG_ERROR, "Error saving hiscores: %s\n", e.what());
    }
}

void Config::load_tiletrial_scores()
{
    const std::string filename = FILENAME_TTRIAL;

    // Counter value that represents 1m 15s 0ms
    static const uint16_t COUNTER_1M_15 = 0x11D0;

    // Create empty property tree object
    ptree pt;

    try
    {
        read_xml(engine.jap ? filename + "_jap.xml" : filename + ".xml" , pt, boost::property_tree::xml_parser::trim_whitespace);
    }
    catch (std::exception &e)
    {
        for (int i = 0; i < 15; i++)
            ttrial.best_times[i] = COUNTER_1M_15;

        e.what();
        return;
    }

    // Time Trial Scores
    for (int i = 0; i < 15; i++)
    {
        ttrial.best_times[i] = pt.get("time_trial.score" + Utils::to_string(i), COUNTER_1M_15);
    }
}

void Config::save_tiletrial_scores()
{
    const std::string filename = FILENAME_TTRIAL;

    // Create empty property tree object
    ptree pt;

    // Time Trial Scores
    for (int i = 0; i < 15; i++)
    {
        pt.put("time_trial.score" + Utils::to_string(i), ttrial.best_times[i]);
    }

    try
    {
        write_xml(engine.jap ? filename + "_jap.xml" : filename + ".xml", pt, std::locale(), xml_writer_settings('\t', 1)); // Tab space 1
    }
    catch (std::exception &e)
    {
        log_cb(RETRO_LOG_ERROR, "Error saving hiscores: %s\n", e.what());
    }
}

bool Config::clear_scores()
{
    // Init Default Hiscores
    ohiscore.init_def_scores();

    bool files_removed = false;

    // Remove XML files if they exist
    // remove() returns 0 on success
    if (!remove(std::string(FILENAME_SCORES).append(".xml").c_str()))
        files_removed = true;
    if (!remove(std::string(FILENAME_SCORES).append("_jap.xml").c_str()))
        files_removed = true;
    if (!remove(std::string(FILENAME_TTRIAL).append(".xml").c_str()))
        files_removed = true;
    if (!remove(std::string(FILENAME_TTRIAL).append("_jap.xml").c_str()))
        files_removed = true;
    if (!remove(std::string(FILENAME_CONT).append(".xml").c_str()))
        files_removed = true;
    if (!remove(std::string(FILENAME_CONT).append("_jap.xml").c_str()))
        files_removed = true;

    return files_removed;
}

void Config::set_fps(int fps)
{
    video.fps = fps;
    // Set core FPS to 30fps, 60fps or 120fps
    if (video.fps == 0)
        this->fps = 30;
    else if (video.fps == 3)
        this->fps = 120;
    else
        this->fps = 60;
    
    // Original game ticks sprites at 30fps but background scroll at 60fps
    if (video.fps == 3)
        tick_fps = 120;
    else if (video.fps < 2)
        tick_fps = 30;
    else
        tick_fps = 60;

    #ifdef COMPILE_SOUND_CODE
    if (config.sound.enabled)
        cannonball::audio.stop_audio();
    osoundint.init();
    if (config.sound.enabled)
        cannonball::audio.start_audio();
    #endif
}
