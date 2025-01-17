#pragma once

#include <cstddef> // for size_t
#include <cstdint> // for uint32_t, uint8_t, int8_t

#include "aliases.hpp"    // for MAX_PLAYERS
#include "render_api.hpp" // for TextureRenderingInfo
#include "sound_manager.hpp"
#include "thread_utils.hpp" // for OnHeapPointer

struct SaveData;
class ScreenCamp;
class ScreenIntro;
class ScreenLeaderboards;
class ScreenLevel;
class ScreenLogo;
class ScreenMenu;
class ScreenOnlineLoading;
class ScreenOnlineLobby;
class ScreenOptions;
class ScreenPlayerProfile;
class ScreenPrologue;
class ScreenSeedInput;
class ScreenTitle;
struct JournalUI;
struct PauseUI;

struct JournalPopupUI
{
    TextureRenderingInfo wiggling_page_icon;
    TextureRenderingInfo black_background;
    TextureRenderingInfo button_icon;
    float wiggling_page_angle;
    uint32_t chapter_to_show;
    uint32_t entry_to_show; // use the odd entry of the left hand page
    uint32_t timer;
    float slide_position;
};

struct SaveRelated
{
    OnHeapPointer<SaveData> savedata;
    JournalPopupUI journal_popup_ui;
};

struct BackgroundMusic
{
    BackgroundSound* game_startup;
    BackgroundSound* main_backgroundtrack;
    BackgroundSound* basecamp;
    BackgroundSound* win_scene;
    BackgroundSound* arena;
    BackgroundSound* arena_intro_and_win;
    BackgroundSound* level_gameplay;
    BackgroundSound* dark_level;
    BackgroundSound* level_transition;
    BackgroundSound* backlayer;
    BackgroundSound* shop;
    BackgroundSound* angered_shopkeeper;
    BackgroundSound* inside_sunken_city_pipe;
    BackgroundSound* pause_menu;
    BackgroundSound* unknown15;
    BackgroundSound* death_transition;
    uint8_t unknown17;
    uint8_t unknown18;
    uint8_t unknown19;
    uint8_t unknown20;
    int8_t skip[2400]; // 600 floats, mostly seem to be 1.0
    float idle_counter;
    uint32_t unknown22;
};

struct GameProps
{
    uint32_t buttons;
    uint32_t unknown1;
    uint32_t unknown2;
    uint32_t unknown3;
    uint32_t buttons_dupe;
    uint32_t unknown4;
    uint32_t unknown5;
    uint32_t unknown6;
    uint32_t buttons_dupe_but_different;
    int8_t unknown8;
    bool game_has_focus;
    bool unknown9;
    bool unknown10;
    // there's more stuff here
};

struct GameManager
{
    BackgroundMusic* music;
    SaveRelated* save_related;
    std::array<uint8_t, MAX_PLAYERS> buttons_controls;
    std::array<uint8_t, MAX_PLAYERS> buttons_movement;
    GameProps* game_props;

    // screen pointers below are most likely in an array and indexed through the screen ID, hence the nullptrs for
    // screens that are available in State
    ScreenLogo* screen_logo;
    ScreenIntro* screen_intro;
    ScreenPrologue* screen_prologue;
    ScreenTitle* screen_title;
    ScreenMenu* screen_menu;
    ScreenOptions* screen_options;
    ScreenPlayerProfile* screen_player_profile;
    ScreenLeaderboards* screen_leaderboards;
    ScreenSeedInput* screen_seed_input;
    size_t unknown_screen_character_select; // available in State
    size_t unknown_screen_team_select;      // available in State
    ScreenCamp* screen_camp;
    ScreenLevel* screen_level;
    size_t screen_transition;            // available in State, but it's a different object! this one only has a render_timer
    size_t unknown_screen_death;         // available in State
    size_t unknown_screen_spaceship;     // (also not) available in State
    size_t unknown_screen_win;           // available in State
    size_t unknown_screen_credits;       // available in State
    size_t unknown_screen_scores;        // available in State
    size_t unknown_screen_constellation; // available in State
    size_t unknown_screen_recap;         // available in State
    size_t unknown_screen_arena_menu;    // available in State
    size_t unknown_screen_arena_stages;  // available in State
    size_t unknown_screen_arena_items;   // available in State
    size_t unknown_screen_arena_select;  // available in State
    size_t unknown_screen_arena_intro;   // available in State
    size_t screen_arena_level;           // also available in State, but it's a different object! this one only has a render_timer, no UI parts
    size_t unknown_screen_arena_score;   // available in State
    ScreenOnlineLoading* screen_online_loading;
    ScreenOnlineLobby* screen_online_lobby;
    PauseUI* pause_ui;
    JournalUI* journal_ui;
    BackgroundSound* main_menu_music;
};

GameManager* get_game_manager();
