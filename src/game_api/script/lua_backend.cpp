#include "lua_backend.hpp"

#include "file_api.hpp"
#include "level_api.hpp"
#include "lua_libs/lua_libs.hpp"
#include "lua_vm.hpp"
#include "overloaded.hpp"
#include "particles.hpp"
#include "render_api.hpp"
#include "rpc.hpp"
#include "script_util.hpp"
#include "sound_manager.hpp"
#include "spawn_api.hpp"
#include "state.hpp"

#include "usertypes/gui_lua.hpp"
#include "usertypes/level_lua.hpp"
#include "usertypes/save_context.hpp"

#include <sol/sol.hpp>

LuaBackend::LuaBackend(SoundManager* sound_mgr)
{
    sound_manager = sound_mgr;

    g_state = get_state_ptr();
    g_items = list_entities();
    g_players = get_players();
    g_save = savedata();

    if (!g_players.empty())
        state.player = g_players.at(0);
    else
        state.player = nullptr;
    state.screen = g_state->screen;
    state.time_level = g_state->time_level;
    state.time_total = g_state->time_total;
    state.time_global = get_frame_count();
    state.frame = get_frame_count();
    state.loading = g_state->loading;
    state.reset = (g_state->quest_flags & 1);
    state.quest_flags = g_state->quest_flags;

    populate_lua_state(this);
    load_libraries(this);
}

void LuaBackend::clear()
{
    std::lock_guard gil_guard{gil};

    clear_all_callbacks();

    const std::string root_path = get_root();
    auto package = lua["package"];
    if (get_unsafe())
    {
        package["path"] = root_path + "/?.lua;" + root_path + "/?/init.lua";
        package["cpath"] = root_path + "/?.dll;" + root_path + "/?/init.dll";
        load_unsafe_libraries(this);
    }
    else
    {
        package["path"] = root_path + "/?.lua;" + root_path + "/?/init.lua";
        package["cpath"] = "";
        package["loadlib"] = sol::lua_nil;
    }
}
void LuaBackend::clear_all_callbacks()
{
    std::lock_guard gil_guard{gil};

    // Clear all callbacks on script reload to avoid running them
    // multiple times.
    level_timers.clear();
    global_timers.clear();
    callbacks.clear();
    for (auto id : vanilla_sound_callbacks)
    {
        sound_manager->clear_callback(id);
    }
    vanilla_sound_callbacks.clear();
    for (auto id : chance_callbacks)
    {
        g_state->level_gen->data->unregister_chance_logic_provider(id);
    }
    chance_callbacks.clear();
    for (auto& [ent_uid, id] : entity_hooks)
    {
        if (Entity* ent = get_entity_ptr(ent_uid))
        {
            ent->unhook(id);
        }
    }
    for (auto& [ent_uid, id] : entity_dtor_hooks)
    {
        if (Entity* ent = get_entity_ptr(ent_uid))
        {
            ent->unhook(id);
        }
    }
    entity_hooks.clear();
    clear_entity_hooks.clear();
    entity_dtor_hooks.clear();
    options.clear();
    required_scripts.clear();
    lua["on_guiframe"] = sol::lua_nil;
    lua["on_frame"] = sol::lua_nil;
    lua["on_camp"] = sol::lua_nil;
    lua["on_start"] = sol::lua_nil;
    lua["on_level"] = sol::lua_nil;
    lua["on_transition"] = sol::lua_nil;
    lua["on_death"] = sol::lua_nil;
    lua["on_win"] = sol::lua_nil;
    lua["on_screen"] = sol::lua_nil;
}

bool LuaBackend::reset()
{
    clear();
    return true;
}

bool LuaBackend::update()
{
    if (!get_enabled())
        return true;

    if (!pre_update())
    {
        return false;
    }

    try
    {
        std::lock_guard gil_guard{gil};

        /// Runs on every game engine frame.
        sol::optional<sol::function> on_frame = lua["on_frame"];
        /// Runs on entering the camp.
        sol::optional<sol::function> on_camp = lua["on_camp"];
        /// Runs on the start of every level.
        sol::optional<sol::function> on_level = lua["on_level"];
        /// Runs on the start of first level.
        sol::optional<sol::function> on_start = lua["on_start"];
        /// Runs on the start of level transition.
        sol::optional<sol::function> on_transition = lua["on_transition"];
        /// Runs on the death screen.
        sol::optional<sol::function> on_death = lua["on_death"];
        /// Runs on any ending cutscene.
        sol::optional<sol::function> on_win = lua["on_win"];
        /// Runs on any [screen change](#on).
        sol::optional<sol::function> on_screen = lua["on_screen"];
        g_players = get_players();
        lua["players"] = std::vector<Player*>(g_players.begin(), g_players.end());
        if (g_state->screen != state.screen && g_state->screen_last != 5)
        {
            level_timers.clear();
            script_input.clear();
            if (on_screen)
                on_screen.value()();
        }
        if (on_frame && g_state->time_level != state.time_level && g_state->screen == 12)
        {
            on_frame.value()();
        }
        if (g_state->screen == 11 && state.screen != 11)
        {
            if (on_camp)
                on_camp.value()();
        }
        if (g_state->screen == 12 && g_state->screen_last != 5 && !g_players.empty() &&
            (state.player != g_players.at(0) || ((g_state->quest_flags & 1) == 0 && state.reset > 0)))
        {
            if (g_state->level_count == 0)
            {
                if (on_start)
                    on_start.value()();
            }
            if (on_level)
                on_level.value()();
        }
        if (g_state->screen == 13 && state.screen != 13)
        {
            if (on_transition)
                on_transition.value()();
        }
        if (g_state->screen == 14 && state.screen != 14)
        {
            if (on_death)
                on_death.value()();
        }
        if ((g_state->screen == 16 && state.screen != 16) || (g_state->screen == 19 && state.screen != 19))
        {
            if (on_win)
                on_win.value()();
        }

        for (auto id : clear_callbacks)
        {
            level_timers.erase(id);
            global_timers.erase(id);
            callbacks.erase(id);
            load_callbacks.erase(id);

            std::erase_if(pre_level_gen_callbacks, [id](auto& cb)
                          { return cb.id == id; });
            std::erase_if(post_level_gen_callbacks, [id](auto& cb)
                          { return cb.id == id; });
            std::erase_if(pre_entity_spawn_callbacks, [id](auto& cb)
                          { return cb.id == id; });
            std::erase_if(post_entity_spawn_callbacks, [id](auto& cb)
                          { return cb.id == id; });
        }
        clear_callbacks.clear();

        for (auto& [ent_uid, id] : clear_entity_hooks)
        {
            auto it = std::find(entity_hooks.begin(), entity_hooks.end(), std::pair{ent_uid, id});
            if (it != entity_hooks.end())
            {
                if (Entity* ent = get_entity_ptr(ent_uid))
                {
                    ent->unhook(id);
                }
                entity_hooks.erase(it);
            }
        }
        clear_entity_hooks.clear();

        for (auto it = global_timers.begin(); it != global_timers.end();)
        {
            auto now = get_frame_count();
            if (auto cb = std::get_if<IntervalCallback>(&it->second))
            {
                if (now >= cb->lastRan + cb->interval)
                {
                    handle_function(cb->func);
                    cb->lastRan = now;
                }
                ++it;
            }
            else if (auto cb = std::get_if<TimeoutCallback>(&it->second))
            {
                if (now >= cb->timeout)
                {
                    handle_function(cb->func);
                    it = global_timers.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            else
            {
                ++it;
            }
        }

        auto now = get_frame_count();
        for (auto& [id, callback] : load_callbacks)
        {
            if (callback.lastRan < 0)
            {
                handle_function(callback.func, LoadContext{get_root(), get_name()});
                callback.lastRan = now;
            }
            break;
        }

        for (auto& [id, callback] : callbacks)
        {
            if ((ON)g_state->screen == callback.screen && g_state->screen != state.screen && g_state->screen_last != 5) // game screens
            {
                handle_function(callback.func);
                callback.lastRan = now;
            }
            else if (callback.screen == ON::LEVEL && g_state->screen == (int)ON::LEVEL && g_state->screen_last != (int)ON::OPTIONS && !g_players.empty() && (state.player != g_players.at(0) || ((g_state->quest_flags & 1) == 0 && state.reset > 0)))
            {
                handle_function(callback.func);
                callback.lastRan = now;
            }
            else
            {
                switch (callback.screen)
                {
                case ON::FRAME:
                {
                    if (g_state->time_level != state.time_level && g_state->screen == (int)ON::LEVEL)
                    {
                        handle_function(callback.func);
                        callback.lastRan = now;
                    }
                    break;
                }
                case ON::GAMEFRAME:
                {
                    if (!g_state->pause && get_frame_count() != state.time_global &&
                        ((g_state->screen >= (int)ON::CAMP && g_state->screen <= (int)ON::DEATH) || g_state->screen == (int)ON::ARENA_MATCH))
                    {
                        handle_function(callback.func);
                        callback.lastRan = now;
                    }
                    break;
                }
                case ON::SCREEN:
                {
                    if (g_state->screen != state.screen)
                    {
                        handle_function(callback.func);
                        callback.lastRan = now;
                    }
                    break;
                }
                case ON::START:
                {
                    if (g_state->screen == (int)ON::LEVEL && g_state->level_count == 0 && !g_players.empty() &&
                        state.player != g_players.at(0))
                    {
                        handle_function(callback.func);
                        callback.lastRan = now;
                    }
                    break;
                }
                case ON::LOADING:
                {
                    if (g_state->loading > 0 && g_state->loading != state.loading)
                    {
                        handle_function(callback.func);
                        callback.lastRan = now;
                    }
                    break;
                }
                case ON::RESET:
                {
                    if ((g_state->quest_flags & 1) > 0 && (g_state->quest_flags & 1) != state.reset)
                    {
                        handle_function(callback.func);
                        callback.lastRan = now;
                    }
                    break;
                }
                case ON::SAVE:
                {
                    if (g_state->screen != state.screen && g_state->screen != (int)ON::OPTIONS && g_state->screen_last != (int)ON::OPTIONS)
                    {
                        handle_function(callback.func, SaveContext{get_root(), get_name()});
                        callback.lastRan = now;
                    }
                    break;
                }
                }
            }
        }

        for (auto it = level_timers.begin(); it != level_timers.end();)
        {
            auto now = g_state->time_level;
            if (auto cb = std::get_if<IntervalCallback>(&it->second))
            {
                if (now >= cb->lastRan + cb->interval)
                {
                    handle_function(cb->func);
                    cb->lastRan = now;
                }
                ++it;
            }
            else if (auto cb = std::get_if<TimeoutCallback>(&it->second))
            {
                if (now >= cb->timeout)
                {
                    handle_function(cb->func);
                    it = level_timers.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            else
            {
                ++it;
            }
        }

        if (!g_players.empty())
            state.player = g_players.at(0);
        else
            state.player = nullptr;
        state.screen = g_state->screen;
        state.time_level = g_state->time_level;
        state.time_total = g_state->time_total;
        state.time_global = get_frame_count();
        state.frame = get_frame_count();
        state.loading = g_state->loading;
        state.reset = (g_state->quest_flags & 1);
        state.quest_flags = g_state->quest_flags;
    }
    catch (const sol::error& e)
    {
        result = e.what();
        return false;
    }
    return true;
}

void LuaBackend::draw(ImDrawList* dl)
{
    if (!get_enabled())
        return;

    draw_list = dl;
    try
    {
        std::lock_guard gil_guard{gil};

        /// Runs on every screen frame. You need this to use draw functions.
        sol::optional<sol::function> on_guiframe = lua["on_guiframe"];

        GuiDrawContext draw_ctx(this, dl);

        if (on_guiframe)
        {
            on_guiframe.value()(draw_ctx);
        }

        for (auto& [id, callback] : callbacks)
        {
            auto now = get_frame_count();
            if (callback.screen == ON::GUIFRAME)
            {
                handle_function(callback.func, draw_ctx);
                callback.lastRan = now;
            }
        }
    }
    catch (const sol::error& e)
    {
        result = e.what();
    }
    draw_list = nullptr;
}

void LuaBackend::render_options()
{
    ImGui::PushID(get_id());
    for (auto& name_option_pair : options)
    {
        std::visit(
            overloaded{
                [&](IntOption& option)
                {
                    if (ImGui::DragInt(name_option_pair.second.desc.c_str(), &option.value, 0.5f, option.min, option.max))
                    {
                        auto& name = name_option_pair.first;
                        lua["options"][name] = option.value;
                    }
                },
                [&](FloatOption& option)
                {
                    if (ImGui::DragFloat(name_option_pair.second.desc.c_str(), &option.value, 0.5f, option.min, option.max))
                    {
                        auto& name = name_option_pair.first;
                        lua["options"][name] = option.value;
                    }
                },
                [&](BoolOption& option)
                {
                    if (ImGui::Checkbox(name_option_pair.second.desc.c_str(), &option.value))
                    {
                        auto& name = name_option_pair.first;
                        lua["options"][name] = option.value;
                    }
                },
                [&](StringOption& option)
                {
                    if (InputString(name_option_pair.second.desc.c_str(), &option.value, 0, nullptr, nullptr))
                    {
                        auto& name = name_option_pair.first;
                        lua["options"][name] = option.value;
                    }
                },
                [&](ComboOption& option)
                {
                    if (ImGui::Combo(name_option_pair.second.desc.c_str(), &option.value, option.options.c_str()))
                    {
                        auto& name = name_option_pair.first;
                        lua["options"][name] = option.value + 1;
                    }
                },
                [&](ButtonOption& option)
                {
                    if (ImGui::Button(name_option_pair.second.desc.c_str()))
                    {
                        uint64_t now =
                            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                        auto& name = name_option_pair.first;
                        lua["options"][name] = now;
                        handle_function(option.on_click);
                    }
                },
            },
            name_option_pair.second.option_impl);
        if (!name_option_pair.second.long_desc.empty())
        {
            ImGui::TextWrapped("%s", name_option_pair.second.long_desc.c_str());
        }
    }
    ImGui::PopID();
}

bool LuaBackend::pre_level_gen_spawn(std::string_view tile_code, float x, float y, int layer)
{
    if (!get_enabled())
        return false;

    for (auto& callback : pre_level_gen_callbacks)
    {
        if (callback.tile_code == tile_code)
        {
            if (handle_function_with_return<bool>(callback.func, x, y, layer).value_or(false))
            {
                return true;
            }
        }
    }
    return false;
}
void LuaBackend::post_level_gen_spawn(std::string_view tile_code, float x, float y, int layer)
{
    if (!get_enabled())
        return;

    for (auto& callback : post_level_gen_callbacks)
    {
        if (callback.tile_code == tile_code)
        {
            handle_function(callback.func, x, y, layer);
        }
    }
}

void LuaBackend::post_room_generation()
{
    if (!get_enabled())
        return;

    auto now = get_frame_count();

    std::lock_guard lock{gil};
    for (auto& [id, callback] : callbacks)
    {
        if (callback.screen == ON::POST_ROOM_GENERATION)
        {
            handle_function(callback.func, PostRoomGenerationContext{});
            callback.lastRan = now;
        }
    }
}

void LuaBackend::post_level_generation()
{
    if (!get_enabled())
        return;

    auto now = get_frame_count();

    std::lock_guard lock{gil};

    g_players = get_players();
    lua["players"] = std::vector<Player*>(g_players.begin(), g_players.end());

    for (auto& [id, callback] : callbacks)
    {
        if (callback.screen == ON::POST_LEVEL_GENERATION)
        {
            handle_function(callback.func);
            callback.lastRan = now;
        }
    }
}

Entity* LuaBackend::pre_entity_spawn(std::uint32_t entity_type, float x, float y, int layer, Entity* overlay, int spawn_type_flags)
{
    if (!get_enabled())
        return nullptr;

    for (auto& callback : pre_entity_spawn_callbacks)
    {
        bool mask_match = callback.entity_mask == 0 || (get_type(entity_type)->search_flags & callback.entity_mask);
        bool flags_match = callback.spawn_type_flags & spawn_type_flags;
        if (mask_match)
        {
            bool type_match = callback.entity_types.empty() || std::count(callback.entity_types.begin(), callback.entity_types.end(), entity_type) > 0;
            if (type_match)
            {
                if (auto spawn_replacement = handle_function_with_return<std::uint32_t>(callback.func, entity_type, x, y, layer, overlay))
                {
                    return get_entity_ptr(spawn_replacement.value());
                }
            }
        }
    }
    return nullptr;
}
void LuaBackend::post_entity_spawn(Entity* entity, int spawn_type_flags)
{
    if (!get_enabled())
        return;

    for (auto& callback : post_entity_spawn_callbacks)
    {
        bool mask_match = callback.entity_mask == 0 || (entity->type->search_flags & callback.entity_mask);
        bool flags_match = callback.spawn_type_flags & spawn_type_flags;
        if (mask_match)
        {
            bool type_match = callback.entity_types.empty() || std::count(callback.entity_types.begin(), callback.entity_types.end(), entity->type->id) > 0;
            if (type_match)
            {
                handle_function(callback.func, entity);
            }
        }
    }
}

void LuaBackend::hook_entity_dtor(Entity* entity)
{
    if (std::count_if(entity_dtor_hooks.begin(), entity_dtor_hooks.end(), [entity](auto& dtor_hook)
                      { return dtor_hook.first == entity->uid; }) == 0)
    {
        std::uint32_t dtor_hook_id = entity->set_on_destroy([this](Entity* entity)
                                                            { pre_entity_destroyed(entity); });
        entity_dtor_hooks.push_back({entity->uid, dtor_hook_id});
    }
}
void LuaBackend::pre_entity_destroyed(Entity* entity)
{
    auto num_erased_hooks = std::erase_if(entity_hooks, [entity](auto& hook)
                                          { return hook.first == entity->uid; });
    assert(num_erased_hooks != 0);
    auto num_erased_dtors = std::erase_if(entity_dtor_hooks, [entity](auto& dtor_hook)
                                          { return dtor_hook.first == entity->uid; });
    assert(num_erased_dtors == 1);
}

std::string LuaBackend::dump_api()
{
    std::set<std::string> excluded_keys{"meta"};

    sol::state dummy_state;
    dummy_state.open_libraries(sol::lib::math, sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::coroutine, sol::lib::package, sol::lib::debug);

    for (auto& [key, value] : lua["_G"].get<sol::table>())
    {
        std::string key_str = key.as<std::string>();
        if (key_str.starts_with("sol."))
        {
            excluded_keys.insert(std::move(key_str));
        }
    }

    for (auto& [key, value] : dummy_state["_G"].get<sol::table>())
    {
        std::string key_str = key.as<std::string>();
        excluded_keys.insert(std::move(key_str));
    }

    require_serpent_lua(dummy_state);
    sol::table opts = dummy_state.create_table();
    opts["comment"] = false;
    sol::function serpent = dummy_state["serpent"]["block"];

    std::map<std::string, std::string> sorted_output;
    for (auto& [key, value] : lua["_G"].get<sol::table>())
    {
        std::string key_str = key.as<std::string>();
        if (!excluded_keys.contains(key_str))
        {
            std::string value_str = serpent(value, opts).get<std::string>();
            if (value_str.starts_with("\"function"))
            {
                value_str = "function(...) end";
            }
            else if (value_str.starts_with("\"userdata"))
            {
                value_str = {};
            }
            sorted_output[std::move(key_str)] = std::move(value_str);
        }
    }

    std::string api;
    for (auto& [key, value] : sorted_output)
        api += fmt::format("{} = {}\n", key, value);

    const static std::regex reg(R"("function:\s[0-9A-F]+")");
    api = std::regex_replace(api, reg, R"("function:")");

    return api;
}