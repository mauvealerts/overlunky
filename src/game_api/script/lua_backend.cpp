#include "lua_backend.hpp"

#include <assert.h>     // for assert
#include <cstddef>      // for size_t
#include <exception>    // for exception
#include <fmt/format.h> // for format_error
#include <list>         // for _List_iterator, _List_co...
#include <sol/sol.hpp>  // for table_proxy, optional
#include <stack>        // for stack
#include <tuple>        // for get
#include <vector>       // for vector

#include "aliases.hpp"                      // for IMAGE, JournalPageType
#include "constants.hpp"                    // for no_return_str
#include "entities_chars.hpp"               // for Player
#include "entity.hpp"                       // for Entity, get_entity_ptr
#include "handle_lua_function.hpp"          // for handle_function
#include "items.hpp"                        // for Inventory
#include "level_api.hpp"                    // for LevelGenData, LevelGenSy...
#include "level_api_types.hpp"              // for LevelGenRoomData
#include "lua_console.hpp"                  // for LuaConsole
#include "lua_vm.hpp"                       // for acquire_lua_vm, get_lua_vm
#include "math.hpp"                         // for AABB
#include "movable_behavior.hpp"             // for CustomMovableBehavior
#include "overloaded.hpp"                   // for overloaded
#include "rpc.hpp"                          // for get_frame_count, get_pla...
#include "screen.hpp"                       // for get_screen_ptr, Screen
#include "script_util.hpp"                  // for InputString
#include "sound_manager.hpp"                // for SoundManager
#include "state.hpp"                        // for StateMemory, State, get_...
#include "strings.hpp"                      // for clear_custom_shopitem_names
#include "usertypes/gui_lua.hpp"            // for GuiDrawContext
#include "usertypes/level_lua.hpp"          // for PreHandleRoomTilesContext
#include "usertypes/save_context.hpp"       // for LoadContext, SaveContext
#include "usertypes/vanilla_render_lua.hpp" // for VanillaRenderContext

std::recursive_mutex g_all_backends_mutex;
std::vector<std::unique_ptr<LuaBackend::ProtectedBackend>> g_all_backends;

LuaBackend::LuaBackend(SoundManager* sound_mgr, LuaConsole* con)
    : lua{get_lua_vm(sound_mgr), sol::create}, vm{acquire_lua_vm(sound_mgr)}, sound_manager{sound_mgr}, console{con}
{
    g_state = State::get().ptr_main();
    state.screen = g_state->screen;
    state.time_level = g_state->time_level;
    state.time_total = g_state->time_total;
    state.time_global = get_frame_count_main();
    state.frame = state.frame;
    state.loading = g_state->loading;
    state.reset = (g_state->quest_flags & 1);
    state.quest_flags = g_state->quest_flags;

    populate_lua_env(lua);

    std::lock_guard lock{g_all_backends_mutex};
    g_all_backends.emplace_back(new ProtectedBackend{this});
    self = g_all_backends.back().get();
}
LuaBackend::~LuaBackend()
{
    {
        auto self_lock = self->Lock();

        auto& global_vm = *vm;
        for (const std::string& module : loaded_modules)
        {
            global_vm["package"]["loaded"][module] = sol::nil;
            global_vm["_G"][module] = sol::nil;
        }

        clear_all_callbacks();
    }

    {
        std::lock_guard lock{g_all_backends_mutex};
        std::erase_if(g_all_backends, [=](const std::unique_ptr<ProtectedBackend>& protected_backend)
                      { return protected_backend.get() == self; });
    }
}

void LuaBackend::clear()
{
    clear_all_callbacks();

    (get_unsafe()
         ? expose_unsafe_libraries
         : hide_unsafe_libraries)(lua);
}
void LuaBackend::clear_all_callbacks()
{
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
    pre_tile_code_callbacks.clear();
    post_tile_code_callbacks.clear();
    pre_entity_spawn_callbacks.clear();
    post_entity_spawn_callbacks.clear();
    pre_entity_instagib_callbacks.clear();
    for (auto id : chance_callbacks)
    {
        g_state->level_gen->data->unregister_chance_logic_provider(id);
    }
    chance_callbacks.clear();
    for (auto id : extra_spawn_callbacks)
    {
        g_state->level_gen->data->undefine_extra_spawn(id);
    }
    extra_spawn_callbacks.clear();

    HookHandler<Entity, CallbackType::Entity>::clear_all_hooks();
    HookHandler<RenderInfo, CallbackType::Entity>::clear_all_hooks();

    for (auto& [screen_id, id] : screen_hooks)
    {
        if (Screen* screen = get_screen_ptr(screen_id))
        {
            screen->unhook(id);
        }
    }
    for (auto& console_command : console_commands)
    {
        console->unregister_command(this, console_command);
    }
    screen_hooks.clear();
    clear_screen_hooks.clear();
    options.clear();
    for (auto& [uid, user_data] : user_datas)
    {
        if (Entity* entity = get_entity_ptr(uid))
        {
            entity->clean_on_dtor(user_data.dtor_hook_id);
        }
    }
    required_scripts.clear();
    console_commands.clear();
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

CustomMovableBehavior* LuaBackend::get_custom_movable_behavior(std::string_view name)
{
    auto it = std::find_if(custom_movable_behaviors.begin(), custom_movable_behaviors.end(), [name](const CustomMovableBehaviorStorage& beh)
                           { return beh.name == name; });
    if (it != custom_movable_behaviors.end())
    {
        return static_cast<CustomMovableBehavior*>(it->behavior.get());
    }
    return nullptr;
}
CustomMovableBehavior* LuaBackend::make_custom_movable_behavior(std::string_view name, uint8_t state_id, VanillaMovableBehavior* base_behavior)
{
    assert(get_custom_movable_behavior(name) == nullptr);
    auto custom_behavior = std::make_shared<CustomMovableBehavior>();
    custom_behavior->state_id = state_id;
    custom_behavior->base_behavior = base_behavior;
    custom_movable_behaviors.push_back(CustomMovableBehaviorStorage{std::string{name}, custom_behavior});
    return custom_behavior.get();
}

sol::object LuaBackend::get_user_data(Entity& entity)
{
    if (user_datas.contains(entity.uid))
    {
        return user_datas[entity.uid].data;
    }
    return sol::nil;
}
sol::object LuaBackend::get_user_data(uint32_t uid)
{
    if (user_datas.contains(uid))
    {
        return user_datas[uid].data;
    }
    return sol::nil;
}
void LuaBackend::set_user_data(Entity& entity, sol::object user_data)
{
    if (!user_datas.contains(entity.uid))
    {
        uint32_t dtor_hook_id = entity.set_on_dtor(
            [this, uid = entity.uid](Entity*)
            {
                user_datas.erase(uid);
            });
        user_datas[entity.uid].dtor_hook_id = dtor_hook_id;
    }
    user_datas[entity.uid].data = user_data;
}
void LuaBackend::set_user_data(uint32_t uid, sol::object user_data)
{
    auto ent = get_entity_ptr(uid);
    set_user_data(*ent, user_data);
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
        // Deprecated =======

        /// Use `set_callback(function, ON.FRAME)` instead
        sol::optional<sol::function> on_frame = lua["on_frame"];
        /// Use `set_callback(function, ON.CAMP)` instead
        sol::optional<sol::function> on_camp = lua["on_camp"];
        /// Use `set_callback(function, ON.LEVEL)` instead
        sol::optional<sol::function> on_level = lua["on_level"];
        /// Use `set_callback(function, ON.START)` instead
        sol::optional<sol::function> on_start = lua["on_start"];
        /// Use `set_callback(function, ON.TRANSITION)` instead
        sol::optional<sol::function> on_transition = lua["on_transition"];
        /// Use `set_callback(function, ON.DEATH)` instead
        sol::optional<sol::function> on_death = lua["on_death"];
        /// Use `set_callback(function, ON.WIN)` instead
        sol::optional<sol::function> on_win = lua["on_win"];
        /// Use `set_callback(function, ON.SCREEN)` instead
        sol::optional<sol::function> on_screen = lua["on_screen"];

        // ==========

        lua["players"] = get_players(g_state);

        /*moved to pre_load_screen
        if (g_state->loading == 1 && g_state->loading != state.loading && g_state->screen_next != (int)ON::OPTIONS && g_state->screen != (int)ON::OPTIONS && g_state->screen_last != (int)ON::OPTIONS)
        {
            level_timers.clear();
            script_input.clear();
            clear_custom_shopitem_names();
        }*/
        if (g_state->screen != state.screen)
        {
            if (on_screen)
                on_screen.value()();
        }
        if (on_frame && g_state->time_level != state.time_level && g_state->screen == (int)ON::LEVEL)
        {
            on_frame.value()();
        }
        if (g_state->screen == (int)ON::CAMP && g_state->screen_last != (int)ON::OPTIONS && g_state->loading != state.loading && g_state->loading == 3 && g_state->time_level == 1)
        {
            if (on_camp)
                on_camp.value()();
        }
        if (g_state->screen == (int)ON::LEVEL && g_state->screen_last != (int)ON::OPTIONS && g_state->loading != state.loading && g_state->loading == 3 && g_state->time_level == 1)
        {
            if (g_state->level_count == 0)
            {
                if (on_start)
                    on_start.value()();
            }
            if (on_level)
                on_level.value()();
        }
        if (g_state->screen == (int)ON::TRANSITION && state.screen != (int)ON::TRANSITION)
        {
            if (on_transition)
                on_transition.value()();
        }
        if (g_state->screen == (int)ON::DEATH && state.screen != (int)ON::DEATH)
        {
            if (on_death)
                on_death.value()();
        }
        if ((g_state->screen == (int)ON::WIN && state.screen != (int)ON::WIN) || (g_state->screen == (int)ON::CONSTELLATION && state.screen != (int)ON::CONSTELLATION))
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

            std::erase_if(pre_tile_code_callbacks, [id](auto& cb)
                          { return cb.id == id; });
            std::erase_if(post_tile_code_callbacks, [id](auto& cb)
                          { return cb.id == id; });
            std::erase_if(pre_entity_spawn_callbacks, [id](auto& cb)
                          { return cb.id == id; });
            std::erase_if(post_entity_spawn_callbacks, [id](auto& cb)
                          { return cb.id == id; });
            std::erase_if(pre_entity_instagib_callbacks, [id](auto& cb)
                          { return cb.id == id; });
        }
        clear_callbacks.clear();

        HookHandler<Entity, CallbackType::Entity>::clear_pending();
        HookHandler<RenderInfo, CallbackType::Entity>::clear_pending();

        for (auto& [screen_id, id] : clear_screen_hooks)
        {
            auto it = std::find(screen_hooks.begin(), screen_hooks.end(), std::pair{screen_id, id});
            if (it != screen_hooks.end())
            {
                auto screen = get_screen_ptr(screen_id);
                if (screen != nullptr)
                {
                    screen->unhook(id);
                }
                screen_hooks.erase(it);
            }
        }
        clear_screen_hooks.clear();

        for (auto it = global_timers.begin(); it != global_timers.end();)
        {
            int now = get_frame_count();
            if (auto cb = std::get_if<IntervalCallback>(&it->second))
            {
                if (now >= cb->lastRan + cb->interval && !is_callback_cleared(it->first))
                {
                    set_current_callback(-1, it->first, CallbackType::Normal);
                    std::optional<bool> keep_going = handle_function<bool>(this, cb->func);
                    clear_current_callback();
                    cb->lastRan = now;
                    if (!keep_going.value_or(true))
                    {
                        it = global_timers.erase(it);
                        continue;
                    }
                }
                ++it;
            }
            else if (auto cbt = std::get_if<TimeoutCallback>(&it->second))
            {
                if (now >= cbt->timeout && !is_callback_cleared(it->first))
                {
                    set_current_callback(-1, it->first, CallbackType::Normal);
                    handle_function<void>(this, cbt->func);
                    clear_current_callback();
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
                set_current_callback(-1, id, CallbackType::Normal);
                handle_function<void>(this, callback.func, LoadContext{get_root(), get_name()});
                clear_current_callback();
                callback.lastRan = now;
            }
        }

        for (auto& [id, callback] : callbacks)
        {
            if (is_callback_cleared(id))
                continue;

            set_current_callback(-1, id, CallbackType::Normal);
            if ((ON)g_state->screen == callback.screen && g_state->screen != state.screen && g_state->screen_last != (int)ON::OPTIONS) // game screens
            {
                handle_function<void>(this, callback.func);
                callback.lastRan = now;
            }
            else if (callback.screen == ON::LEVEL && g_state->screen == (int)ON::LEVEL && g_state->screen_last != (int)ON::OPTIONS && state.loading != g_state->loading && g_state->loading == 3 && g_state->time_level <= 1)
            {
                handle_function<void>(this, callback.func);
                callback.lastRan = now;
            }
            else if (callback.screen == ON::CAMP && g_state->screen == (int)ON::CAMP && g_state->screen_last != (int)ON::OPTIONS && state.loading != g_state->loading && g_state->loading == 3 && g_state->time_level == 1)
            {
                handle_function<void>(this, callback.func);
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
                        handle_function<void>(this, callback.func);
                        callback.lastRan = now;
                    }
                    break;
                }
                case ON::GAMEFRAME:
                {
                    if (!g_state->pause && get_frame_count() != state.time_global &&
                        ((g_state->screen >= (int)ON::CAMP && g_state->screen <= (int)ON::DEATH) || g_state->screen == (int)ON::ARENA_MATCH))
                    {
                        handle_function<void>(this, callback.func);
                        callback.lastRan = now;
                    }
                    break;
                }
                case ON::SCREEN:
                {
                    if (g_state->screen != state.screen)
                    {
                        handle_function<void>(this, callback.func);
                        callback.lastRan = now;
                    }
                    break;
                }
                case ON::START:
                {
                    if (g_state->screen == (int)ON::LEVEL && g_state->screen_last != (int)ON::OPTIONS && g_state->level_count == 0 && g_state->loading != state.loading && g_state->loading == 3 && g_state->time_level <= 1)
                    {
                        handle_function<void>(this, callback.func);
                        callback.lastRan = now;
                    }
                    break;
                }
                case ON::LOADING:
                {
                    if (g_state->loading > 0 && g_state->loading != state.loading)
                    {
                        handle_function<void>(this, callback.func);
                        callback.lastRan = now;
                    }
                    break;
                }
                case ON::RESET:
                {
                    if ((g_state->quest_flags & 1) > 0 && (g_state->quest_flags & 1) != state.reset)
                    {
                        handle_function<void>(this, callback.func);
                        callback.lastRan = now;
                    }
                    break;
                }
                case ON::SAVE:
                {
                    if ((g_state->loading != state.loading && g_state->loading == 1) || manual_save)
                    {
                        handle_function<void>(this, callback.func, SaveContext{get_root(), get_name()});
                        callback.lastRan = now;
                    }
                    break;
                }
                default:
                    break;
                }
            }
            clear_current_callback();
        }
        const int now_l = g_state->time_level;
        for (auto it = level_timers.begin(); it != level_timers.end();)
        {
            if (auto cb = std::get_if<IntervalCallback>(&it->second))
            {
                if (now_l >= cb->lastRan + cb->interval && !is_callback_cleared(it->first))
                {
                    set_current_callback(-1, it->first, CallbackType::Normal);
                    std::optional<bool> keep_going = handle_function<bool>(this, cb->func);
                    clear_current_callback();
                    cb->lastRan = now_l;
                    if (!keep_going.value_or(true))
                    {
                        it = level_timers.erase(it);
                        continue;
                    }
                }
                ++it;
            }
            else if (auto cbt = std::get_if<TimeoutCallback>(&it->second))
            {
                if (now_l >= cbt->timeout && !is_callback_cleared(it->first))
                {
                    set_current_callback(-1, it->first, CallbackType::Normal);
                    handle_function<void>(this, cbt->func);
                    clear_current_callback();
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

        state.screen = g_state->screen;
        state.time_level = g_state->time_level;
        state.time_total = g_state->time_total;
        state.time_global = get_frame_count();
        state.frame = get_frame_count();
        state.loading = g_state->loading;
        state.reset = (g_state->quest_flags & 1);
        state.quest_flags = g_state->quest_flags;

        if (manual_save)
        {
            manual_save = false;
            last_save = g_state->time_startup;
        }
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
        if (!pre_draw())
        {
            return;
        }

        // Deprecated
        /// Use `set_callback(function, ON.GUIFRAME)` instead
        sol::optional<sol::function> on_guiframe = lua["on_guiframe"];

        GuiDrawContext draw_ctx(this);

        if (on_guiframe)
        {
            on_guiframe.value()(draw_ctx);
        }

        for (auto& [id, callback] : callbacks)
        {
            if (is_callback_cleared(id))
                continue;

            auto now = get_frame_count();
            if (callback.screen == ON::GUIFRAME)
            {
                set_current_callback(-1, id, CallbackType::Normal);
                handle_function<void>(this, callback.func, draw_ctx);
                clear_current_callback();
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
                    auto& name = name_option_pair.first;
                    option.value = lua["options"][name].get_or(option.value);
                    ImGui::DragInt(name_option_pair.second.desc.c_str(), &option.value, 0.5f, option.min, option.max);
                    lua[sol::create_if_nil]["options"][name] = option.value;
                },
                [&](FloatOption& option)
                {
                    auto& name = name_option_pair.first;
                    option.value = lua["options"][name].get_or(option.value);
                    ImGui::DragFloat(name_option_pair.second.desc.c_str(), &option.value, 0.5f, option.min, option.max);
                    lua[sol::create_if_nil]["options"][name] = option.value;
                },
                [&](BoolOption& option)
                {
                    auto& name = name_option_pair.first;
                    option.value = lua["options"][name].get_or(option.value);
                    ImGui::Checkbox(name_option_pair.second.desc.c_str(), &option.value);
                    lua[sol::create_if_nil]["options"][name] = option.value;
                },
                [&](StringOption& option)
                {
                    auto& name = name_option_pair.first;
                    option.value = lua["options"][name].get_or(option.value);
                    InputString(name_option_pair.second.desc.c_str(), &option.value, 0, nullptr, nullptr);
                    lua[sol::create_if_nil]["options"][name] = option.value;
                },
                [&](ComboOption& option)
                {
                    auto& name = name_option_pair.first;
                    option.value = lua["options"][name].get_or(option.value + 1) - 1;
                    ImGui::Combo(name_option_pair.second.desc.c_str(), &option.value, option.options.c_str());
                    lua[sol::create_if_nil]["options"][name] = option.value + 1;
                },
                [&](ButtonOption& option)
                {
                    if (ImGui::Button(name_option_pair.second.desc.c_str()))
                    {
                        uint64_t now =
                            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                        auto& name = name_option_pair.first;
                        lua[sol::create_if_nil]["options"][name] = now;
                        handle_function<void>(this, option.on_click);
                    }
                },
                [&](CustomOption& option)
                {
                    auto& name = name_option_pair.first;
                    GuiDrawContext draw_ctx(this);
                    auto return_value = handle_function<sol::object>(this, option.func, draw_ctx);
                    if (return_value.has_value())
                    {
                        if (name != "")
                            lua[sol::create_if_nil]["options"][name] = return_value.value();
                        else
                            lua[sol::create_if_nil]["options"] = return_value.value();
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

bool LuaBackend::is_callback_cleared(int32_t callback_id) const
{
    return std::find(clear_callbacks.begin(), clear_callbacks.end(), callback_id) != clear_callbacks.end();
}
bool LuaBackend::is_screen_callback_cleared(std::pair<int32_t, uint32_t> callback_id) const
{
    return std::find(clear_screen_hooks.begin(), clear_screen_hooks.end(), callback_id) != clear_screen_hooks.end();
}

bool LuaBackend::pre_tile_code(std::string_view tile_code, float x, float y, int layer, uint16_t room_template)
{
    if (!get_enabled())
        return false;

    for (auto& callback : pre_tile_code_callbacks)
    {
        if (is_callback_cleared(callback.id))
            continue;

        if (callback.tile_code == tile_code)
        {
            if (handle_function<bool>(this, callback.func, x, y, layer, room_template).value_or(false))
            {
                return true;
            }
        }
    }
    return false;
}
void LuaBackend::post_tile_code(std::string_view tile_code, float x, float y, int layer, uint16_t room_template)
{
    if (!get_enabled())
        return;

    for (auto& callback : post_tile_code_callbacks)
    {
        if (is_callback_cleared(callback.id))
            continue;

        if (callback.tile_code == tile_code)
        {
            set_current_callback(-1, callback.id, CallbackType::Normal);
            handle_function<void>(this, callback.func, x, y, layer, room_template);
            clear_current_callback();
        }
    }
}

void LuaBackend::pre_load_level_files()
{
    if (!get_enabled())
        return;

    auto now = get_frame_count();

    for (auto& [id, callback] : callbacks)
    {
        if (is_callback_cleared(id))
            continue;

        if (callback.screen == ON::PRE_LOAD_LEVEL_FILES)
        {
            set_current_callback(-1, id, CallbackType::Normal);
            handle_function<void>(this, callback.func, PreLoadLevelFilesContext{});
            clear_current_callback();
            callback.lastRan = now;
        }
    }
}
void LuaBackend::pre_level_generation()
{
    if (!get_enabled())
        return;

    auto now = get_frame_count();

    lua["players"] = get_players(g_state);

    for (auto& [id, callback] : callbacks)
    {
        if (is_callback_cleared(id))
            continue;

        if (callback.screen == ON::PRE_LEVEL_GENERATION)
        {
            set_current_callback(-1, id, CallbackType::Normal);
            handle_function<void>(this, callback.func);
            clear_current_callback();
            callback.lastRan = now;
        }
    }
}
bool LuaBackend::pre_load_screen()
{
    if (!get_enabled())
        return false;

    auto now = get_frame_count();

    auto state_ptr = State::get().ptr();
    if ((ON)state_ptr->screen_next <= ON::LEVEL && (ON)state_ptr->screen_next != ON::OPTIONS && (ON)state_ptr->screen != ON::OPTIONS)
    {
        using namespace std::string_view_literals;
        set_level_string(u"%d-%d"sv);
    }

    for (auto& [id, callback] : callbacks)
    {
        if (is_callback_cleared(id))
            continue;

        if (callback.screen == ON::PRE_LOAD_SCREEN)
        {
            set_current_callback(-1, id, CallbackType::Normal);
            auto return_value = handle_function<bool>(this, callback.func).value_or(false);
            clear_current_callback();
            callback.lastRan = now;
            if (return_value)
                return return_value;
        }
    }

    if ((ON)state_ptr->screen == ON::LEVEL && (ON)state_ptr->screen_next != ON::DEATH && (state_ptr->quest_flags & 1) == 0)
    {
        for (auto uid : get_entities_by_mask(1))
        {
            auto ent = get_entity_ptr(uid)->as<Player>();
            int slot = ent->inventory_ptr->player_slot;
            if (slot == -1 && ent->linked_companion_parent == -1)
                continue;
            if (slot == -1 && ent->linked_companion_parent != -1)
            {
                Player* parent = ent;
                while (true)
                {
                    parent = get_entity_ptr(parent->linked_companion_parent)->as<Player>();
                    slot++;
                    if (parent->linked_companion_parent == -1)
                    {
                        slot += (parent->inventory_ptr->player_slot + 1) * 100;
                        break;
                    }
                }
            }
            if (slot < 0)
                continue;
            bool should_save = false;
            SavedUserData saved;
            if (user_datas.contains(ent->uid))
            {
                saved.self = get_user_data(ent->uid);
                should_save = true;
            }
            if (ent->holding_uid != -1 and user_datas.contains(ent->holding_uid))
            {
                saved.held = get_user_data(ent->holding_uid);
                should_save = true;
            }
            if (ent->overlay && (ent->overlay->type->search_flags & 2) > 0 && user_datas.contains(ent->overlay->uid))
            {
                saved.mount = get_user_data(ent->overlay->uid);
                should_save = true;
            }
            for (auto [type, powerup] : ent->powerups)
            {
                if (user_datas.contains(powerup->uid))
                {
                    saved.powerups[type] = get_user_data(powerup->uid);
                    should_save = true;
                }
            }
            if (should_save)
                saved_user_datas[slot] = saved;
        }
    }

    level_timers.clear();
    script_input.clear();
    clear_custom_shopitem_names();

    return false;
}
void LuaBackend::post_room_generation()
{
    if (!get_enabled())
        return;

    auto now = get_frame_count();

    for (auto& [id, callback] : callbacks)
    {
        if (is_callback_cleared(id))
            continue;

        if (callback.screen == ON::POST_ROOM_GENERATION)
        {
            set_current_callback(-1, id, CallbackType::Normal);
            handle_function<void>(this, callback.func, PostRoomGenerationContext{});
            clear_current_callback();
            callback.lastRan = now;
        }
    }
}
void LuaBackend::post_level_generation()
{
    if (!get_enabled())
        return;

    auto now = get_frame_count();

    lua["players"] = get_players(g_state);

    auto state_ptr = State::get().ptr();
    if ((ON)state_ptr->screen == ON::LEVEL)
    {
        for (auto uid : get_entities_by_mask(1))
        {
            auto ent = get_entity_ptr(uid)->as<Player>();
            int slot = ent->inventory_ptr->player_slot;
            if (slot == -1 && ent->linked_companion_parent == -1)
                continue;
            if (slot == -1 && ent->linked_companion_parent != -1)
            {
                Player* parent = ent;
                while (true)
                {
                    parent = get_entity_ptr(parent->linked_companion_parent)->as<Player>();
                    slot++;
                    if (parent->linked_companion_parent == -1)
                    {
                        slot += (parent->inventory_ptr->player_slot + 1) * 100;
                        break;
                    }
                }
            }
            if (slot < 0)
                continue;
            if (saved_user_datas.contains(slot))
            {
                set_user_data(*ent, saved_user_datas[slot].self.value_or(sol::nil));
                if (ent->holding_uid != -1)
                    set_user_data(ent->holding_uid, saved_user_datas[slot].held.value_or(sol::nil));
                if (ent->overlay && (ent->overlay->type->search_flags & 2) > 0)
                    set_user_data(ent->overlay->uid, saved_user_datas[slot].mount.value_or(sol::nil));
                for (auto [type, powerup] : ent->powerups)
                {
                    if (saved_user_datas[slot].powerups.contains(type))
                        set_user_data(powerup->uid, saved_user_datas[slot].powerups[type]);
                }
            }
        }
        saved_user_datas.clear();
    }

    for (auto& [id, callback] : callbacks)
    {
        if (is_callback_cleared(id))
            continue;

        if (callback.screen == ON::POST_LEVEL_GENERATION)
        {
            set_current_callback(-1, id, CallbackType::Normal);
            handle_function<void>(this, callback.func);
            clear_current_callback();
            callback.lastRan = now;
        }
    }
}
void LuaBackend::post_load_screen()
{
    if (!get_enabled())
        return;

    auto now = get_frame_count();

    for (auto& [id, callback] : callbacks)
    {
        if (is_callback_cleared(id))
            continue;

        if (callback.screen == ON::POST_LOAD_SCREEN)
        {
            set_current_callback(-1, id, CallbackType::Normal);
            handle_function<void>(this, callback.func);
            clear_current_callback();
            callback.lastRan = now;
        }
    }
}
void LuaBackend::on_death_message(STRINGID stringid)
{
    if (!get_enabled())
        return;

    auto now = get_frame_count();

    for (auto& [id, callback] : callbacks)
    {
        if (is_callback_cleared(id))
            continue;

        if (callback.screen == ON::DEATH_MESSAGE)
        {
            set_current_callback(-1, id, CallbackType::Normal);
            handle_function<void>(this, callback.func, stringid);
            clear_current_callback();
            callback.lastRan = now;
        }
    }
}

std::string LuaBackend::pre_get_random_room(int x, int y, uint8_t layer, uint16_t room_template)
{
    if (!get_enabled())
        return std::string{};

    auto now = get_frame_count();

    for (auto& [id, callback] : callbacks)
    {
        if (is_callback_cleared(id))
            continue;

        if (callback.screen == ON::PRE_GET_RANDOM_ROOM)
        {
            callback.lastRan = now;
            set_current_callback(-1, id, CallbackType::Normal);
            std::string return_value = handle_function<std::string>(this, callback.func, x, y, layer, room_template).value_or(std::string{});
            clear_current_callback();
            if (!return_value.empty())
            {
                return return_value;
            }
        }
    }
    return std::string{};
}
LuaBackend::PreHandleRoomTilesResult LuaBackend::pre_handle_room_tiles(LevelGenRoomData room_data, int x, int y, uint16_t room_template)
{
    if (!get_enabled())
        return {false, std::nullopt};

    auto now = get_frame_count();

    PreHandleRoomTilesContext ctx{room_data};

    for (auto& [id, callback] : callbacks)
    {
        if (is_callback_cleared(id))
            continue;

        if (callback.screen == ON::PRE_HANDLE_ROOM_TILES)
        {
            callback.lastRan = now;
            set_current_callback(-1, id, CallbackType::Normal);
            if (handle_function<bool>(this, callback.func, x, y, room_template, ctx).value_or(false))
            {
                clear_current_callback();
                return {true, ctx.modded_room_data};
            }
            clear_current_callback();
        }
    }
    return {false, ctx.modded_room_data};
}

Entity* LuaBackend::pre_entity_spawn(std::uint32_t entity_type, float x, float y, int layer, Entity* overlay, int spawn_type_flags)
{
    if (!get_enabled())
        return nullptr;

    for (auto& callback : pre_entity_spawn_callbacks)
    {
        if (is_callback_cleared(callback.id))
            continue;

        bool mask_match = callback.entity_mask == 0 || (get_type(entity_type)->search_flags & callback.entity_mask);
        bool flags_match = callback.spawn_type_flags & spawn_type_flags;
        if (mask_match && flags_match)
        {
            bool type_match = callback.entity_types.empty() || std::count(callback.entity_types.begin(), callback.entity_types.end(), entity_type) > 0;
            if (type_match)
            {
                set_current_callback(-1, callback.id, CallbackType::Normal);
                if (auto spawn_replacement = handle_function<std::uint32_t>(this, callback.func, entity_type, x, y, layer, overlay, spawn_type_flags))
                {
                    clear_current_callback();
                    return get_entity_ptr(spawn_replacement.value());
                }
                clear_current_callback();
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
        if (is_callback_cleared(callback.id))
            continue;

        bool mask_match = callback.entity_mask == 0 || (entity->type->search_flags & callback.entity_mask);
        bool flags_match = callback.spawn_type_flags & spawn_type_flags;
        if (mask_match && flags_match)
        {
            bool type_match = callback.entity_types.empty() || std::count(callback.entity_types.begin(), callback.entity_types.end(), entity->type->id) > 0;
            if (type_match)
            {
                set_current_callback(-1, callback.id, CallbackType::Normal);
                handle_function<void>(this, callback.func, entity, spawn_type_flags);
                clear_current_callback();
            }
        }
    }
}

bool LuaBackend::pre_entity_instagib(Entity* victim)
{
    bool skip{false};
    if (!get_enabled())
        return skip;

    for (auto& callback : pre_entity_instagib_callbacks)
    {
        if (is_callback_cleared(callback.id))
            continue;

        if (callback.uid == victim->uid)
        {
            set_current_callback(-1, callback.id, CallbackType::Normal);
            skip |= handle_function<bool>(this, callback.func, victim).value_or(false);
            clear_current_callback();
        }
    }

    return skip;
}

void LuaBackend::process_vanilla_render_callbacks(ON event)
{
    if (!get_enabled())
        return;

    auto now = get_frame_count();
    VanillaRenderContext render_ctx;
    for (auto& [id, callback] : callbacks)
    {
        if (is_callback_cleared(id))
            continue;

        if (callback.screen == event)
        {
            set_current_callback(-1, id, CallbackType::Normal);
            handle_function<void>(this, callback.func, render_ctx);
            clear_current_callback();
            callback.lastRan = now;
        }
    }
}

void LuaBackend::process_vanilla_render_draw_depth_callbacks(ON event, uint8_t draw_depth, const AABB& bbox)
{
    if (!get_enabled())
        return;

    auto now = get_frame_count();
    VanillaRenderContext render_ctx;
    render_ctx.bounding_box = bbox;
    for (auto& [id, callback] : callbacks)
    {
        if (is_callback_cleared(id))
            continue;

        if (callback.screen == event)
        {
            set_current_callback(-1, id, CallbackType::Normal);
            handle_function<void>(this, callback.func, render_ctx, draw_depth);
            clear_current_callback();
            callback.lastRan = now;
        }
    }
}

void LuaBackend::process_vanilla_render_journal_page_callbacks(ON event, JournalPageType page_type, JournalPage* page)
{
    if (!get_enabled())
        return;

    auto now = get_frame_count();
    VanillaRenderContext render_ctx;
    for (auto& [id, callback] : callbacks)
    {
        if (is_callback_cleared(id))
            continue;

        if (callback.screen == event)
        {
            set_current_callback(-1, id, CallbackType::Normal);
            handle_function<void>(this, callback.func, render_ctx, page_type, page);
            clear_current_callback();
            callback.lastRan = now;
        }
    }
}

std::u16string LuaBackend::pre_speach_bubble(Entity* entity, char16_t* buffer)
{
    if (!get_enabled())
        return std::u16string{no_return_str};

    auto now = get_frame_count();

    std::optional<std::u16string> return_value = std::nullopt;

    for (auto& [id, callback] : callbacks)
    {
        if (is_callback_cleared(id))
            continue;

        if (callback.screen == ON::SPEECH_BUBBLE)
        {
            callback.lastRan = now;
            set_current_callback(-1, id, CallbackType::Normal);
            if (auto speech_value = handle_function<std::u16string>(this, callback.func, entity, buffer))
            {
                if (!return_value)
                {
                    return_value = speech_value;
                }
            }
            clear_current_callback();
        }
    }
    return return_value.value_or(std::u16string{no_return_str});
}

std::u16string LuaBackend::pre_toast(char16_t* buffer)
{
    if (!get_enabled())
        return std::u16string{no_return_str};

    auto now = get_frame_count();

    std::optional<std::u16string> return_value = std::nullopt;

    for (auto& [id, callback] : callbacks)
    {
        if (is_callback_cleared(id))
            continue;

        if (callback.screen == ON::TOAST)
        {
            callback.lastRan = now;
            set_current_callback(-1, id, CallbackType::Normal);
            if (auto toast_value = handle_function<std::u16string>(this, callback.func, buffer))
            {
                if (!return_value)
                {
                    return_value = toast_value;
                }
            }
            clear_current_callback();
        }
    }
    return return_value.value_or(std::u16string{no_return_str});
}

bool LuaBackend::pre_load_journal_chapter(uint8_t chapter)
{
    if (!get_enabled())
        return false;

    auto now = get_frame_count();
    for (auto& [id, callback] : callbacks)
    {
        if (is_callback_cleared(id))
            continue;

        if (callback.screen == ON::PRE_LOAD_JOURNAL_CHAPTER)
        {
            callback.lastRan = now;
            set_current_callback(-1, id, CallbackType::Normal);
            if (auto return_value = handle_function<bool>(this, callback.func, chapter))
            {
                if (return_value.value())
                {
                    return true;
                }
            }
            clear_current_callback();
        }
    }
    return false;
}

std::vector<uint32_t> LuaBackend::post_load_journal_chapter(uint8_t chapter, const std::vector<uint32_t>& pages)
{
    if (!get_enabled())
        return {};

    auto now = get_frame_count();
    std::vector<uint32_t> new_pages;
    for (auto& [id, callback] : callbacks)
    {
        if (is_callback_cleared(id))
            continue;

        if (callback.screen == ON::POST_LOAD_JOURNAL_CHAPTER)
        {
            callback.lastRan = now;
            set_current_callback(-1, id, CallbackType::Normal);
            if (auto returned_pages = handle_function<sol::object>(this, callback.func, chapter, sol::as_table(pages)).value_or<sol::object>({}))
            {
                if (returned_pages.get_type() == sol::type::table || returned_pages.get_type() == sol::type::userdata)
                {
                    new_pages.clear();
                    const auto table = returned_pages.as<sol::table>();
                    for (auto something : table)
                    {
                        if (something.second.get_type() == sol::type::number)
                        {
                            new_pages.push_back(static_cast<uint32_t>(something.second.as<double>()));
                        }
                    }
                }
            }
            clear_current_callback();
        }
    }
    return new_pages;
}

std::optional<bool> LuaBackend::pre_get_feat(FEAT feat)
{
    if (!get_enabled())
        return std::nullopt;

    auto now = get_frame_count();
    for (auto& [id, callback] : callbacks)
    {
        if (is_callback_cleared(id))
            continue;

        if (callback.screen == ON::PRE_GET_FEAT)
        {
            callback.lastRan = now;
            set_current_callback(-1, id, CallbackType::Normal);
            if (auto return_value = handle_function<bool>(this, callback.func, feat))
            {
                if (return_value.has_value())
                {
                    return return_value.value();
                }
            }
            clear_current_callback();
        }
    }
    return std::nullopt;
}

bool LuaBackend::pre_set_feat(FEAT feat)
{
    if (!get_enabled())
        return false;

    auto now = get_frame_count();
    for (auto& [id, callback] : callbacks)
    {
        if (is_callback_cleared(id))
            continue;

        if (callback.screen == ON::PRE_SET_FEAT)
        {
            callback.lastRan = now;
            set_current_callback(-1, id, CallbackType::Normal);
            if (auto return_value = handle_function<bool>(this, callback.func, feat))
            {
                if (return_value.has_value() && return_value.value())
                {
                    return return_value.value();
                }
            }
            clear_current_callback();
        }
    }
    return false;
}

CurrentCallback LuaBackend::get_current_callback()
{
    return current_cb;
}

void LuaBackend::set_current_callback(int32_t aux_id, int32_t id, CallbackType type)
{
    current_cb.aux_id = aux_id;
    current_cb.id = id;
    current_cb.type = type;
}

void LuaBackend::clear_current_callback()
{
    current_cb.aux_id = 0;
    current_cb.id = 0;
    current_cb.type = CallbackType::None;
}

void LuaBackend::set_error(std::string err)
{
    result = std::move(err);

#ifdef SPEL2_EXTRA_ANNOYING_SCRIPT_ERRORS
    DEBUG("{}", result);

    std::istringstream errors{result};
    const auto now{std::chrono::system_clock::now()};
    while (!errors.eof())
    {
        std::string err_line;
        getline(errors, err_line);
        messages.push_back({err_line, now, ImVec4(1.0f, 0.2f, 0.2f, 1.0f)});
        if (messages.size() > 30)
        {
            messages.pop_front();
        }
    }
#endif
}

/**
 * static functions begin
 */
void LuaBackend::for_each_backend(std::function<bool(LockedBackend)> fun)
{
    std::lock_guard lock{g_all_backends_mutex};
    for (std::unique_ptr<ProtectedBackend>& backend : g_all_backends)
    {
        if (!fun(backend->Lock()))
        {
            break;
        }
    }
}
LuaBackend::LockedBackend LuaBackend::get_backend(std::string_view id)
{
    return get_backend_safe(id).value();
}
std::optional<LuaBackend::LockedBackend> LuaBackend::get_backend_safe(std::string_view id)
{
    std::lock_guard lock{g_all_backends_mutex};
    for (std::unique_ptr<ProtectedBackend>& backend : g_all_backends)
    {
        LockedBackend locked = backend->Lock();
        if (locked->get_path() == id)
        {
            return locked;
        }
    }
    return std::nullopt;
}
LuaBackend::LockedBackend LuaBackend::get_backend_by_id(std::string_view id, std::string_view ver)
{
    return get_backend_by_id_safe(id, ver).value();
}
std::optional<LuaBackend::LockedBackend> LuaBackend::get_backend_by_id_safe(std::string_view id, std::string_view ver)
{
    std::lock_guard lock{g_all_backends_mutex};
    for (std::unique_ptr<ProtectedBackend>& backend : g_all_backends)
    {
        LockedBackend locked = backend->Lock();
        if (locked->get_id() == id && (ver == "" || ver == locked->get_version()))
        {
            return locked;
        }
    }
    return std::nullopt;
}

// A callstack may end up as something like:
//  - backend: update
//  - script0: update
//  - script0: spawn_entity
//  - backend: post_entity_spawn
//  - script1: callback
//  > script1: errors...
//      if we were not using a stack here the error
//      would propagate to script0 instead of script1
std::stack<LuaBackend*, std::vector<LuaBackend*>> g_CallingBackend{};
LuaBackend::LockedBackend LuaBackend::get_calling_backend()
{
    return LuaBackend::get_backend(get_calling_backend_id());
}
std::string LuaBackend::get_calling_backend_id()
{
    std::lock_guard global_lock{global_lua_lock};

    if (!g_CallingBackend.empty())
    {
        return g_CallingBackend.top()->get_path();
    }

    static const sol::state& lua = get_lua_vm();
    auto get_script_id = lua["get_script_id"];
    if (get_script_id.get_type() == sol::type::function)
    {
        auto script_id = get_script_id();
        if (script_id.get_type() == sol::type::string && script_id.valid())
        {
            return script_id.get<std::string>();
        }
        else
        {
            sol::error e = script_id;
            throw std::runtime_error{e.what()};
        }
    }

    throw std::runtime_error{"Trying to get calling backend but Lua state does not seem to be setup..."};
}
void LuaBackend::push_calling_backend(LuaBackend* calling_backend)
{
    std::lock_guard global_lock{global_lua_lock};
    g_CallingBackend.push(calling_backend);
}
void LuaBackend::pop_calling_backend([[maybe_unused]] LuaBackend* calling_backend)
{
    std::lock_guard global_lock{global_lua_lock};
    g_CallingBackend.pop();
}

/**
 * static functions end
 */
