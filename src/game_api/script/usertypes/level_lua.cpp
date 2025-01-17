#include "level_lua.hpp"

#include <algorithm>        // for max, replace, transform
#include <array>            // for array
#include <cctype>           // for toupper
#include <cstdint>          // for uint32_t, uint8_t, uint...
#include <cstring>          // for size_t, memcpy, memset
#include <exception>        // for exception
#include <fmt/format.h>     // for format_error
#include <functional>       // for _Func_impl_no_alloc<>::...
#include <initializer_list> // for initializer_list
#include <list>             // for _List_iterator, _List_c...
#include <locale>           // for num_put
#include <map>              // for map, _Tree<>::iterator
#include <memory>           // for unique_ptr
#include <mutex>            // for lock_guard
#include <new>              // for operator new
#include <sol/sol.hpp>      // for global_table, proxy_key_t
#include <string_view>      // for string_view
#include <type_traits>      // for move, remove_reference_t
#include <unordered_map>    // for unordered_map, unordere...
#include <utility>          // for min, max, monostate, get

#include "containers/game_unordered_map.hpp" // for game_unordered_map
#include "entity.hpp"                        // for to_id
#include "level_api.hpp"                     // for THEME_OVERRIDE, ThemeInfo
#include "math.hpp"                          // for AABB
#include "savedata.hpp"                      // for SaveData, Constellation...
#include "script/handle_lua_function.hpp"    // for handle_function
#include "script/lua_backend.hpp"            // for LuaBackend, LevelGenCal...
#include "script/safe_cb.hpp"                // for make_safe_cb
#include "state.hpp"                         // for State, StateMemory, enu...
#include "state_structs.hpp"                 // for QuestsInfo, Camera, Que...

void PreLoadLevelFilesContext::override_level_files(std::vector<std::string> levels)
{
    override_next_levels(std::move(levels));
}
void PreLoadLevelFilesContext::add_level_files(std::vector<std::string> levels)
{
    add_next_levels(std::move(levels));
}

bool PostRoomGenerationContext::set_room_template(uint32_t x, uint32_t y, LAYER layer, ROOM_TEMPLATE room_template)
{
    const uint8_t real_layer = static_cast<int32_t>(layer) < 0 ? 0 : static_cast<uint8_t>(layer);
    return State::get().ptr_local()->level_gen->set_room_template(x, y, real_layer, room_template);
}
bool PostRoomGenerationContext::mark_as_machine_room_origin(uint32_t x, uint32_t y, LAYER layer)
{
    const uint8_t real_layer = static_cast<int32_t>(layer) < 0 ? 0 : static_cast<uint8_t>(layer);
    return State::get().ptr_local()->level_gen->mark_as_machine_room_origin(x, y, real_layer);
}
bool PostRoomGenerationContext::mark_as_set_room(uint32_t x, uint32_t y, LAYER layer)
{
    const uint8_t real_layer = static_cast<int32_t>(layer) < 0 ? 0 : static_cast<uint8_t>(layer);
    return State::get().ptr_local()->level_gen->mark_as_set_room(x, y, real_layer, true);
}
bool PostRoomGenerationContext::unmark_as_set_room(uint32_t x, uint32_t y, LAYER layer)
{
    const uint8_t real_layer = static_cast<int32_t>(layer) < 0 ? 0 : static_cast<uint8_t>(layer);
    return State::get().ptr_local()->level_gen->mark_as_set_room(x, y, real_layer, false);
}

bool PostRoomGenerationContext::set_shop_type(uint32_t x, uint32_t y, LAYER layer, int32_t shop_type)
{
    const uint8_t real_layer = static_cast<int32_t>(layer) < 0 ? 0 : static_cast<uint8_t>(layer);
    return State::get().ptr_local()->level_gen->set_shop_type(x, y, real_layer, static_cast<ShopType>(shop_type));
}

bool PostRoomGenerationContext::set_procedural_spawn_chance(PROCEDURAL_CHANCE chance_id, uint32_t inverse_chance)
{
    return State::get().ptr_local()->level_gen->set_procedural_spawn_chance(chance_id, inverse_chance);
}

void PostRoomGenerationContext::set_num_extra_spawns(std::uint32_t extra_spawn_id, std::uint32_t num_spawns_front_layer, std::uint32_t num_spawns_back_layer)
{
    State::get().ptr_local()->level_gen->data->set_num_extra_spawns(extra_spawn_id, num_spawns_front_layer, num_spawns_back_layer);
}

std::optional<SHORT_TILE_CODE> PostRoomGenerationContext::define_short_tile_code(ShortTileCodeDef short_tile_code_def)
{
    return State::get().ptr_local()->level_gen->data->define_short_tile_code(short_tile_code_def);
}
void PostRoomGenerationContext::change_short_tile_code(SHORT_TILE_CODE short_tile_code, ShortTileCodeDef short_tile_code_def)
{
    State::get().ptr_local()->level_gen->data->change_short_tile_code(short_tile_code, short_tile_code_def);
}

std::optional<SHORT_TILE_CODE> PreHandleRoomTilesContext::get_short_tile_code(uint32_t tx, uint32_t ty, LAYER layer) const
{
    if (tx >= 0 && tx < 10 && ty >= 0 && ty < 8)
    {
        const uint8_t real_layer = static_cast<int32_t>(layer) < 0 ? 0 : static_cast<uint8_t>(layer);
        if (real_layer == static_cast<uint8_t>(LAYER::FRONT))
        {
            return get_room_data().front_layer[ty][tx];
        }
        else if (has_back_layer())
        {
            return get_room_data().back_layer.value()[ty][tx];
        }
    }
    return std::nullopt;
}
bool PreHandleRoomTilesContext::set_short_tile_code(uint32_t tx, uint32_t ty, LAYER layer, SHORT_TILE_CODE short_tile_code)
{
    if (tx >= 0 && tx < 10 && ty >= 0 && ty < 8)
    {
        if (layer == LAYER::BOTH)
        {
            set_short_tile_code(tx, ty, LAYER::BACK, short_tile_code);
            layer = LAYER::FRONT;
        }

        const uint8_t real_layer = static_cast<int32_t>(layer) < 0 ? 0 : static_cast<uint8_t>(layer);
        if (real_layer == static_cast<uint8_t>(LAYER::FRONT))
        {
            get_mutable_room_data().front_layer[ty][tx] = short_tile_code;
            return true;
        }
        else if (has_back_layer())
        {
            get_mutable_room_data().back_layer.value()[ty][tx] = short_tile_code;
            return true;
        }
    }
    return false;
}
std::vector<std::tuple<uint32_t, uint32_t, LAYER>> PreHandleRoomTilesContext::find_all_short_tile_codes(LAYER layer, SHORT_TILE_CODE short_tile_code)
{
    if (layer == LAYER::BOTH)
    {
        std::vector<std::tuple<uint32_t, uint32_t, LAYER>> positions = find_all_short_tile_codes(LAYER::FRONT, short_tile_code);
        std::vector<std::tuple<uint32_t, uint32_t, LAYER>> positions_back = find_all_short_tile_codes(LAYER::BACK, short_tile_code);
        positions.insert(positions.end(), positions_back.begin(), positions_back.end());
        return positions_back;
    }

    const uint8_t real_layer = static_cast<int32_t>(layer) < 0 ? 0 : static_cast<uint8_t>(layer);
    const bool front_layer = real_layer == static_cast<uint8_t>(LAYER::FRONT);
    if (front_layer || has_back_layer())
    {
        const LevelGenRoomData& data = get_room_data();
        const SingleRoomData& mutable_room_data = front_layer ? data.front_layer : data.back_layer.value();

        std::vector<std::tuple<uint32_t, uint32_t, LAYER>> positions;
        for (uint32_t tx = 0; tx < 10; tx++)
        {
            for (uint32_t ty = 0; ty < 8; ty++)
            {
                if (mutable_room_data[ty][tx] == short_tile_code)
                {
                    positions.push_back({tx, ty, layer});
                }
            }
        }
        return positions;
    }
    return {};
}
bool PreHandleRoomTilesContext::replace_short_tile_code(LAYER layer, SHORT_TILE_CODE short_tile_code, SHORT_TILE_CODE replacement_short_tile_code)
{
    const uint8_t real_layer = static_cast<int32_t>(layer) < 0 ? 0 : static_cast<uint8_t>(layer);
    const bool front_layer = real_layer == static_cast<uint8_t>(LAYER::FRONT);
    if (front_layer || has_back_layer())
    {
        LevelGenRoomData& mutable_data = get_mutable_room_data();
        SingleRoomData& mutable_room_data = front_layer ? mutable_data.front_layer : mutable_data.back_layer.value();
        for (uint32_t tx = 0; tx < 10; tx++)
        {
            for (uint32_t ty = 0; ty < 8; ty++)
            {
                if (mutable_room_data[ty][tx] == short_tile_code)
                {
                    mutable_room_data[ty][tx] = replacement_short_tile_code;
                }
            }
        }
        return true;
    }
    return false;
}
bool PreHandleRoomTilesContext::has_back_layer() const
{
    return get_room_data().back_layer.has_value();
}
void PreHandleRoomTilesContext::add_empty_back_layer()
{
    if (!has_back_layer())
    {
        auto& data = get_mutable_room_data();
        data.back_layer.emplace();
        std::memset(data.back_layer.value().data(), '0', sizeof(SingleRoomData));
    }
}
void PreHandleRoomTilesContext::add_copied_back_layer()
{
    if (!has_back_layer())
    {
        auto& data = get_mutable_room_data();
        data.back_layer.emplace();
        std::memcpy(data.back_layer.value().data(), data.front_layer.data(), sizeof(SingleRoomData));
    }
}

const LevelGenRoomData& PreHandleRoomTilesContext::get_room_data() const
{
    return modded_room_data.has_value()
               ? modded_room_data.value()
               : room_data;
}
LevelGenRoomData& PreHandleRoomTilesContext::get_mutable_room_data()
{
    if (!modded_room_data.has_value())
    {
        modded_room_data = room_data;
    }
    return modded_room_data.value();
}

struct ThemeOverride
{
    bool enabled;
    uint8_t theme;
    sol::optional<sol::function> func;
    sol::optional<sol::function> pre;
    sol::optional<sol::function> post;
};

class CustomTheme : public ThemeInfo
{
  public:
    /// Level file to load. Probably doesn't do much in custom themes, especially if you're forcing them in PRE_LOAD_LEVEL_FILES.
    std::string level_file;
    /// Theme index. Probably shouldn't collide with the vanilla ones. Purpose unknown.
    uint8_t theme;
    /// Base THEME to load enabled functions from, when no other theme is specified.
    uint8_t base_theme;
    std::string backend_id;

    std::map<THEME_OVERRIDE, std::unique_ptr<ThemeOverride>> overrides;
    /// Add TEXTUREs here to override different dynamic textures.
    std::map<DYNAMIC_TEXTURE, uint32_t> textures;

    void override(THEME_OVERRIDE index, bool enabled_)
    {
        if (overrides.find(index) == overrides.end())
            overrides[index] = std::unique_ptr<ThemeOverride>{new ThemeOverride{enabled_, UINT8_MAX}};
        else
            overrides[index]->enabled = enabled_;
    }

    void override(THEME_OVERRIDE index, uint8_t theme_)
    {
        if (overrides.find(index) == overrides.end())
            overrides[index] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, theme_}};
        else
            overrides[index]->theme = theme_;
    }

    void override(THEME_OVERRIDE index, sol::function func_)
    {
        if (overrides.find(index) == overrides.end())
            overrides[index] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX, std::move(func_)}};
        else
        {
            overrides[index]->func = std::move(func_);
        }
    }

    /// Set a callback to be called before this theme function.
    void pre(THEME_OVERRIDE index, sol::function func_)
    {
        if (overrides.find(index) == overrides.end())
            overrides[index] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX, std::nullopt, std::move(func_)}};
        else
        {
            overrides[index]->pre = std::move(func_);
        }
    }

    /// Set a callback to be called after this theme function, to fix some changes it did for example.
    void post(THEME_OVERRIDE index, sol::function func_)
    {
        if (overrides.find(index) == overrides.end())
            overrides[index] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX, std::nullopt, std::nullopt, std::move(func_)}};
        else
        {
            overrides[index]->post = std::move(func_);
        }
    }

    uint8_t get_override_theme(THEME_OVERRIDE index)
    {
        if (overrides.find(index) != overrides.end() && overrides[index]->theme > 0 && overrides[index]->theme < 18)
            return overrides[index]->theme - 1;
        else if (overrides.find(THEME_OVERRIDE::BASE) != overrides.end() && overrides[THEME_OVERRIDE::BASE]->theme > 0 && overrides[THEME_OVERRIDE::BASE]->theme < 18)
            return overrides[THEME_OVERRIDE::BASE]->theme - 1;
        return get_base_id();
    }

    uint8_t get_override_theme_or_dwelling(THEME_OVERRIDE index)
    {
        if (overrides.find(index) != overrides.end() && overrides[index]->theme > 0 && overrides[index]->theme < 18)
            return overrides[index]->theme - 1;
        else if (overrides.find(THEME_OVERRIDE::BASE) != overrides.end() && overrides[THEME_OVERRIDE::BASE]->theme > 0 && overrides[THEME_OVERRIDE::BASE]->theme < 18)
            return overrides[THEME_OVERRIDE::BASE]->theme - 1;
        return 0;
    }

    bool get_override_enabled(THEME_OVERRIDE index)
    {
        if (overrides.find(index) != overrides.end())
        {
            return overrides[index]->enabled;
        }
        return false;
    }

    bool get_override_func_enabled(THEME_OVERRIDE index)
    {
        if (overrides.find(index) != overrides.end())
        {
            return overrides[index]->func.has_value();
        }
        return false;
    }

    bool get_pre_func_enabled(THEME_OVERRIDE index)
    {
        if (overrides.find(index) != overrides.end())
        {
            return overrides[index]->pre.has_value();
        }
        return false;
    }

    bool get_post_func_enabled(THEME_OVERRIDE index)
    {
        if (overrides.find(index) != overrides.end())
        {
            return overrides[index]->post.has_value();
        }
        return false;
    }

    template <class Ret, class... Args>
    std::optional<Ret> run_override_func(THEME_OVERRIDE index, Args&&... args)
    {
        if (overrides.find(index) != overrides.end() && get_override_func_enabled(index))
        {
            auto backend = LuaBackend::get_backend(backend_id);
            return handle_function<Ret>(backend.get(), overrides[index]->func.value(), std::forward<Args>(args)...);
        }
        return std::nullopt;
    }

    template <class Ret, class... Args>
    std::optional<Ret> run_pre_func(THEME_OVERRIDE index, Args&&... args)
    {
        if (overrides.find(index) != overrides.end() && get_pre_func_enabled(index))
        {
            auto backend = LuaBackend::get_backend(backend_id);
            return handle_function<Ret>(backend.get(), overrides[index]->pre.value(), std::forward<Args>(args)...);
        }
        return std::nullopt;
    }

    template <class Ret, class... Args>
    std::optional<Ret> run_post_func(THEME_OVERRIDE index, Args&&... args)
    {
        if (overrides.find(index) != overrides.end() && get_post_func_enabled(index))
        {
            auto backend = LuaBackend::get_backend(backend_id);
            return handle_function<Ret>(backend.get(), overrides[index]->post.value(), std::forward<Args>(args)...);
        }
        return std::nullopt;
    }

    /// Create a new theme with an id and base theme, overriding defaults. Check [theme functions that are default enabled here](https://github.com/spelunky-fyi/overlunky/blob/main/src/game_api/script/usertypes/level_lua.cpp).
    CustomTheme(uint8_t theme_id_, uint8_t base_theme_, bool defaults)
    {
        level_file = "";
        theme = theme_id_;
        base_theme = base_theme_ - 1;
        backend_id = LuaBackend::get_calling_backend_id();
        if (defaults)
        {
            overrides[THEME_OVERRIDE::INIT_FLAGS] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX}};
            overrides[THEME_OVERRIDE::SPAWN_LEVEL] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX}};
            overrides[THEME_OVERRIDE::SPAWN_PLAYERS] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, 0}};
            overrides[THEME_OVERRIDE::SPAWN_TRANSITION] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, 0}};
            overrides[THEME_OVERRIDE::POST_TRANSITION] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, 0}};
            overrides[THEME_OVERRIDE::SPAWN_PROCEDURAL] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX}};
            overrides[THEME_OVERRIDE::SPAWN_BACKGROUND] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX}};
            overrides[THEME_OVERRIDE::SPAWN_BORDER] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, 0}};
            overrides[THEME_OVERRIDE::ENT_BORDER] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX}};
            overrides[THEME_OVERRIDE::TEXTURE_DYNAMIC] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX}};
            overrides[THEME_OVERRIDE::PLAYER_DAMAGE] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX}};
            overrides[THEME_OVERRIDE::GRAVITY] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX}};
            /*
            overrides[THEME_OVERRIDE::INIT_LEVEL] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX}};
            overrides[THEME_OVERRIDE::INIT_FLAGS] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX}};
            overrides[THEME_OVERRIDE::SPAWN_EFFECTS] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX}};
            overrides[THEME_OVERRIDE::TRANSITION_MODIFIER] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX}};
            overrides[THEME_OVERRIDE::UNKNOWN_V32] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX}};
            overrides[THEME_OVERRIDE::UNKNOWN_V38] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX}};
            overrides[THEME_OVERRIDE::UNKNOWN_V47] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX}};
            overrides[THEME_OVERRIDE::SPAWN_DECORATION] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX}};
            overrides[THEME_OVERRIDE::SPAWN_DECORATION2] = std::unique_ptr<ThemeOverride>{new ThemeOverride{true, UINT8_MAX}};
            */
        }
    }
    /// Create a new theme with defaults.
    CustomTheme(uint8_t theme_id_, uint8_t base_theme_)
        : CustomTheme(theme_id_, base_theme_, true)
    {
    }
    /// Create a new theme with base dwelling and id 100.
    CustomTheme()
        : CustomTheme(100, 1, true)
    {
    }
    ~CustomTheme()
    {
        overrides.clear();
    }
    bool get_unknown1()
    {
        return unknown1;
    }
    void init_flags()
    {
        auto index = THEME_OVERRIDE::INIT_FLAGS;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->init_flags();
        run_post_func<std::monostate>(index);
    }
    void init_level()
    {
        auto index = THEME_OVERRIDE::INIT_LEVEL;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->init_level();
        run_post_func<std::monostate>(index);
    }
    void unknown_v4()
    {
        auto index = THEME_OVERRIDE::UNKNOWN_V4;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->unknown_v4();
        run_post_func<std::monostate>(index);
    }
    void unknown_v5()
    {
        auto index = THEME_OVERRIDE::UNKNOWN_V5;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->unknown_v5();
        run_post_func<std::monostate>(index);
    }
    void add_special_rooms()
    {
        auto index = THEME_OVERRIDE::SPECIAL_ROOMS;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->add_special_rooms();
        run_post_func<std::monostate>(index);
    }
    void unknown_v7()
    {
        auto index = THEME_OVERRIDE::UNKNOWN_V7;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->unknown_v7();
        run_post_func<std::monostate>(index);
    }
    void unknown_v8()
    {
        auto index = THEME_OVERRIDE::UNKNOWN_V8;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->unknown_v8();
        run_post_func<std::monostate>(index);
    }
    void add_vault()
    {
        auto index = THEME_OVERRIDE::VAULT;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->add_vault();
        run_post_func<std::monostate>(index);
    }
    void add_coffin()
    {
        auto index = THEME_OVERRIDE::COFFIN;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->add_coffin();
        run_post_func<std::monostate>(index);
    }
    void add_special_feeling()
    {
        auto index = THEME_OVERRIDE::FEELING;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->add_special_feeling();
        run_post_func<std::monostate>(index);
    }
    bool unknown_v12()
    {
        auto index = THEME_OVERRIDE::UNKNOWN_V12;
        bool ret = false;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<bool>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->unknown_v12();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    void spawn_level()
    {
        auto index = THEME_OVERRIDE::SPAWN_LEVEL;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme_or_dwelling(index)]->spawn_level();
        run_post_func<std::monostate>(index);
    }
    void spawn_border()
    {
        auto index = THEME_OVERRIDE::SPAWN_BORDER;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme_or_dwelling(index)]->spawn_border();
        run_post_func<std::monostate>(index);
    }
    void post_process_level()
    {
        auto index = THEME_OVERRIDE::POST_PROCESS_LEVEL;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->post_process_level();
        run_post_func<std::monostate>(index);
    }
    void spawn_traps()
    {
        auto index = THEME_OVERRIDE::SPAWN_TRAPS;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->spawn_traps();
        run_post_func<std::monostate>(index);
    }
    void post_process_entities()
    {
        auto index = THEME_OVERRIDE::POST_PROCESS_ENTITIES;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->post_process_entities();
        run_post_func<std::monostate>(index);
    }
    void spawn_procedural()
    {
        auto index = THEME_OVERRIDE::SPAWN_PROCEDURAL;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->spawn_procedural();
        run_post_func<std::monostate>(index);
    }
    void spawn_background()
    {
        auto index = THEME_OVERRIDE::SPAWN_BACKGROUND;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->spawn_background();
        run_post_func<std::monostate>(index);
    }
    void spawn_lights()
    {
        auto index = THEME_OVERRIDE::SPAWN_LIGHTS;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->spawn_lights();
        run_post_func<std::monostate>(index);
    }
    void spawn_transition()
    {
        auto index = THEME_OVERRIDE::SPAWN_TRANSITION;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->spawn_transition();
        run_post_func<std::monostate>(index);
    }
    void post_transition()
    {
        auto index = THEME_OVERRIDE::POST_TRANSITION;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme_or_dwelling(index)]->post_transition();
        run_post_func<std::monostate>(index);
    }
    void spawn_players()
    {
        auto index = THEME_OVERRIDE::SPAWN_PLAYERS;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme_or_dwelling(index)]->spawn_players();
        run_post_func<std::monostate>(index);
    }
    void spawn_effects()
    {
        auto index = THEME_OVERRIDE::SPAWN_EFFECTS;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->spawn_effects();
        else
        {
            // set sane camera bounds anyway for your convenience
            // you can always change this in post
            auto state = State::get().ptr();
            state->camera->bounds_left = 0.5f;
            state->camera->bounds_top = 124.5f;
            state->camera->bounds_right = 10.0f * state->w + 4.5f;
            state->camera->bounds_bottom = 120.5f - 8.0f * state->h;
        }
        run_post_func<std::monostate>(index);
    }
    const char* get_level_file()
    {
        return level_file.c_str();
    }
    uint8_t get_theme_id()
    {
        auto index = THEME_OVERRIDE::THEME_ID;
        uint8_t ret = theme;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<uint8_t>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->get_theme_id();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    uint8_t get_base_id()
    {
        auto index = THEME_OVERRIDE::BASE_ID;
        uint8_t ret = base_theme;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<uint8_t>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->get_base_id();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    uint32_t get_floor_spreading_type()
    {
        auto index = THEME_OVERRIDE::ENT_FLOOR_SPREADING;
        uint32_t ret = to_id("ENT_TYPE_FX_SHADOW");
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<uint32_t>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->get_floor_spreading_type();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    uint32_t get_floor_spreading_type2()
    {
        auto index = THEME_OVERRIDE::ENT_FLOOR_SPREADING2;
        uint32_t ret = to_id("ENT_TYPE_FX_SHADOW");
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<uint32_t>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->get_floor_spreading_type2();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    bool unknown_v30()
    {
        auto index = THEME_OVERRIDE::UNKNOWN_V30;
        bool ret = false;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<bool>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->unknown_v30();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    uint32_t get_transition_block_modifier()
    {
        auto index = THEME_OVERRIDE::TRANSITION_MODIFIER;
        uint32_t ret = 85; // TODO: don't know what this is
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<uint32_t>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->get_floor_spreading_type2();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    uint32_t unknown_v32()
    {
        auto index = THEME_OVERRIDE::UNKNOWN_V32;
        uint32_t ret = 12; // TODO: don't know what this is
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<uint32_t>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->unknown_v32();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    uint32_t get_backwall_type()
    {
        auto index = THEME_OVERRIDE::ENT_BACKWALL;
        uint32_t ret = to_id("ENT_TYPE_BG_LEVEL_BACKWALL");
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<uint32_t>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->get_backwall_type();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    uint32_t get_border_type()
    {
        auto index = THEME_OVERRIDE::ENT_BORDER;
        uint32_t ret = to_id("ENT_TYPE_FLOOR_BORDERTILE");
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<uint32_t>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->get_border_type();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    uint32_t get_critter_type()
    {
        auto index = THEME_OVERRIDE::ENT_CRITTER;
        uint32_t ret = to_id("ENT_TYPE_FX_SHADOW");
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<uint32_t>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->get_critter_type();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    float get_liquid_gravity()
    {
        auto index = THEME_OVERRIDE::GRAVITY;
        float ret = 0.0f;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<float>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->get_liquid_gravity();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    bool get_player_damage()
    {
        auto index = THEME_OVERRIDE::PLAYER_DAMAGE;
        bool ret = false;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<bool>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->get_player_damage();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    bool unknown_v38()
    {
        auto index = THEME_OVERRIDE::UNKNOWN_V38;
        bool ret = true;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<bool>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->unknown_v38();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    uint32_t get_backlayer_lut()
    {
        auto index = THEME_OVERRIDE::TEXTURE_BACKLAYER_LUT;
        uint32_t ret = 395; // default lut texture id
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<uint32_t>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->get_backlayer_lut();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    float get_backlayer_light_level()
    {
        auto index = THEME_OVERRIDE::BACKLAYER_LIGHT_LEVEL;
        float ret = 0.0f;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<float>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->get_backlayer_light_level();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    bool get_loop()
    {
        auto index = THEME_OVERRIDE::LOOP;
        bool ret = false;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<bool>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->get_loop();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    uint8_t get_vault_level()
    {
        auto index = THEME_OVERRIDE::VAULT_LEVEL;
        uint8_t ret = 255;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<uint8_t>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->get_vault_level();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    bool get_unknown_1_or_2(uint8_t index)
    {
        if (index == 0)
            return unknown1;
        return unknown2;
    }

    /// Add TEXTUREs to `textures` to override different dynamic textures easily.
    uint32_t get_dynamic_texture(int32_t texture_id)
    {
        auto index = THEME_OVERRIDE::TEXTURE_DYNAMIC;
        uint32_t ret = 0;
        run_pre_func<std::monostate>(index, texture_id);
        if (textures.find((DYNAMIC_TEXTURE)texture_id) != textures.end())
        {
            ret = textures[(DYNAMIC_TEXTURE)texture_id];
        }
        else if (get_override_func_enabled(index))
            ret = run_override_func<uint32_t>(index, texture_id).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->get_dynamic_texture(texture_id);
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    void pre_transition()
    {
        auto index = THEME_OVERRIDE::PRE_TRANSITION;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme_or_dwelling(index)]->pre_transition();
        run_post_func<std::monostate>(index);
    }
    uint32_t get_level_height()
    {
        auto index = THEME_OVERRIDE::LEVEL_HEIGHT;
        uint32_t ret = State::get().ptr_local()->h - 1;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<uint32_t>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->get_level_height();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    uint32_t unknown_v47()
    {
        auto index = THEME_OVERRIDE::UNKNOWN_V47;
        uint32_t ret = 0;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            ret = run_override_func<uint32_t>(index).value_or(ret);
        else if (get_override_enabled(index))
            ret = State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->unknown_v47();
        run_post_func<std::monostate>(index, ret);
        return ret;
    }
    void spawn_decoration()
    {
        auto index = THEME_OVERRIDE::SPAWN_DECORATION;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->spawn_decoration();
        run_post_func<std::monostate>(index);
    }
    void spawn_decoration2()
    {
        auto index = THEME_OVERRIDE::SPAWN_DECORATION2;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->spawn_decoration2();
        run_post_func<std::monostate>(index);
    }
    void spawn_extra()
    {
        auto index = THEME_OVERRIDE::SPAWN_EXTRA;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->spawn_extra();
        run_post_func<std::monostate>(index);
    }
    void unknown_v51()
    {
        auto index = THEME_OVERRIDE::UNKNOWN_V51;
        run_pre_func<std::monostate>(index);
        if (get_override_func_enabled(index))
            run_override_func<std::monostate>(index);
        else if (get_override_enabled(index))
            State::get().ptr_local()->level_gen->themes[get_override_theme(index)]->unknown_v51();
        run_post_func<std::monostate>(index);
    }
};

namespace NLevel
{
void register_usertypes(sol::state& lua)
{
    /// Default function in spawn definitions to check whether a spawn is valid or not
    lua["default_spawn_is_valid"] = default_spawn_is_valid;

    /// Check if position satifies the given POS_TYPE flags, to be used in a custom is_valid function procedural for spawns.
    lua["position_is_valid"] = position_is_valid;

    /// Add a callback for a specific tile code that is called before the game handles the tile code.
    /// Return true in order to stop the game or scripts loaded after this script from handling this tile code.
    /// For example, when returning true in this callback set for `"floor"` then no floor will spawn in the game (unless you spawn it yourself)
    /// <br/>The callback signature is bool pre_tile_code(float x, float y, int layer, ROOM_TEMPLATE room_template)
    lua["set_pre_tile_code_callback"] = [](sol::function cb, std::string tile_code) -> CallbackId
    {
        auto backend = LuaBackend::get_calling_backend();
        backend->pre_tile_code_callbacks.push_back(LevelGenCallback{backend->cbcount, std::move(tile_code), std::move(cb)});
        return backend->cbcount++;
    };
    /// Add a callback for a specific tile code that is called after the game handles the tile code.
    /// Use this to affect what the game or other scripts spawned in this position.
    /// This is received even if a previous pre-tile-code-callback has returned true
    /// <br/>The callback signature is nil post_tile_code(float x, float y, int layer, ROOM_TEMPLATE room_template)
    lua["set_post_tile_code_callback"] = [](sol::function cb, std::string tile_code) -> CallbackId
    {
        auto backend = LuaBackend::get_calling_backend();
        backend->post_tile_code_callbacks.push_back(LevelGenCallback{backend->cbcount, std::move(tile_code), std::move(cb)});
        return backend->cbcount++;
    };
    /// Define a new tile code, to make this tile code do anything you have to use either [set_pre_tile_code_callback](#set_pre_tile_code_callback) or [set_post_tile_code_callback](#set_post_tile_code_callback).
    /// If a user disables your script but still uses your level mod nothing will be spawned in place of your tile code.
    lua["define_tile_code"] = [](std::string tile_code) -> TILE_CODE
    {
        auto backend = LuaBackend::get_calling_backend();
        return backend->g_state->level_gen->data->define_tile_code(std::move(tile_code));
    };

    /// Gets a short tile code based on definition, returns `nil` if it can't be found
    lua["get_short_tile_code"] = [](ShortTileCodeDef short_tile_code_def) -> std::optional<uint8_t>
    {
        return State::get().ptr_local()->level_gen->data->get_short_tile_code(short_tile_code_def);
    };
    /// Gets the definition of a short tile code (if available), will vary depending on which file is loaded
    lua["get_short_tile_code_definition"] = [](SHORT_TILE_CODE short_tile_code) -> std::optional<ShortTileCodeDef>
    {
        return State::get().ptr_local()->level_gen->data->get_short_tile_code_def(short_tile_code);
    };

    /// Define a new procedural spawn, the function `nil do_spawn(x, y, layer)` contains your code to spawn the thing, whatever it is.
    /// The function `bool is_valid(x, y, layer)` determines whether the spawn is legal in the given position and layer.
    /// Use for example when you can spawn only on the ceiling, under water or inside a shop.
    /// Set `is_valid` to `nil` in order to use the default rule (aka. on top of floor and not obstructed).
    /// If a user disables your script but still uses your level mod nothing will be spawned in place of your procedural spawn.
    lua["define_procedural_spawn"] = [](std::string procedural_spawn, sol::function do_spawn, sol::function is_valid) -> PROCEDURAL_CHANCE
    {
        std::function<bool(float, float, int)> is_valid_call{nullptr};
        if (is_valid)
        {
            is_valid_call = make_safe_cb<bool(float, float, int)>(std::move(is_valid));
        }
        std::function<void(float, float, int)> do_spawn_call = make_safe_cb<void(float, float, int)>(std::move(do_spawn));

        auto backend = LuaBackend::get_calling_backend();
        LevelGenData* data = backend->g_state->level_gen->data;
        uint32_t chance = data->define_chance(std::move(procedural_spawn));
        std::uint32_t id = data->register_chance_logic_provider(chance, SpawnLogicProvider{std::move(is_valid_call), std::move(do_spawn_call)});
        backend->chance_callbacks.push_back(id);
        return chance;
    };

    /// Define a new extra spawn, these are semi-guaranteed level gen spawns with a fixed upper bound.
    /// The function `nil do_spawn(x, y, layer)` contains your code to spawn the thing, whatever it is.
    /// The function `bool is_valid(x, y, layer)` determines whether the spawn is legal in the given position and layer.
    /// Use for example when you can spawn only on the ceiling, under water or inside a shop.
    /// Set `is_valid` to `nil` in order to use the default rule (aka. on top of floor and not obstructed).
    /// To change the number of spawns use `PostRoomGenerationContext:set_num_extra_spawns` during `ON.POST_ROOM_GENERATION`
    /// No name is attached to the extra spawn since it is not modified from level files, instead every call to this function will return a new uniqe id.
    lua["define_extra_spawn"] = [](sol::function do_spawn, sol::function is_valid, std::uint32_t num_spawns_frontlayer, std::uint32_t num_spawns_backlayer) -> std::uint32_t
    {
        std::function<bool(float, float, int)> is_valid_call{nullptr};
        if (is_valid)
        {
            is_valid_call = make_safe_cb<bool(float, float, int)>(std::move(is_valid));
        }
        std::function<void(float, float, int)> do_spawn_call = make_safe_cb<void(float, float, int)>(std::move(do_spawn));

        auto backend = LuaBackend::get_calling_backend();
        LevelGenData* data = backend->g_state->level_gen->data;
        std::uint32_t extra_spawn_id = data->define_extra_spawn(num_spawns_frontlayer, num_spawns_backlayer, SpawnLogicProvider{std::move(is_valid_call), std::move(do_spawn_call)});
        backend->extra_spawn_callbacks.push_back(extra_spawn_id);
        return extra_spawn_id;
    };
    /// Use to query whether any of the requested spawns could not be made, usually because there were not enough valid spaces in the level.
    /// Returns missing spawns in the front layer and missing spawns in the back layer in that order.
    /// The value only makes sense after level generation is complete, aka after `ON.POST_LEVEL_GENERATION` has run.
    lua["get_missing_extra_spawns"] = [](std::uint32_t extra_spawn_chance_id) -> std::pair<std::uint32_t, std::uint32_t>
    {
        return State::get().ptr()->level_gen->data->get_missing_extra_spawns(extra_spawn_chance_id);
    };

    /// Transform a position to a room index to be used in `get_room_template` and `PostRoomGenerationContext.set_room_template`
    lua["get_room_index"] = [](float x, float y) -> std::pair<int, int>
    {
        return State::get().ptr_local()->level_gen->get_room_index(x, y);
    };
    /// Transform a room index into the top left corner position in the room
    lua["get_room_pos"] = [](int x, int y) -> std::pair<float, float>
    {
        return State::get().ptr_local()->level_gen->get_room_pos(x, y);
    };
    /// Get the room template given a certain index, returns `nil` if coordinates are out of bounds
    lua["get_room_template"] = [](int x, int y, LAYER layer) -> std::optional<uint16_t>
    {
        const uint8_t real_layer = enum_to_layer(layer);
        return State::get().ptr_local()->level_gen->get_room_template(x, y, real_layer);
    };
    /// Get whether a room is flipped at the given index, returns `false` if coordinates are out of bounds
    lua["is_room_flipped"] = [](int x, int y) -> bool
    {
        return State::get().ptr_local()->level_gen->is_room_flipped(x, y);
    };
    /// Get whether a room is the origin of a machine room
    lua["is_machine_room_origin"] = [](int x, int y) -> bool
    {
        return State::get().ptr_local()->level_gen->is_machine_room_origin(x, y);
    };
    /// For debugging only, get the name of a room template, returns `'invalid'` if room template is not defined
    lua["get_room_template_name"] = [](int16_t room_template) -> std::string_view
    {
        return State::get().ptr_local()->level_gen->get_room_template_name(room_template);
    };

    /// Define a new room template to use with `set_room_template`
    lua["define_room_template"] = [](std::string room_template, ROOM_TEMPLATE_TYPE type) -> uint16_t
    {
        return State::get().ptr_local()->level_gen->data->define_room_template(std::move(room_template), static_cast<RoomTemplateType>(type));
    };
    /// Set the size of room template in tiles, the template must be of type `ROOM_TEMPLATE_TYPE.MACHINE_ROOM`.
    lua["set_room_template_size"] = [](uint16_t room_template, uint16_t width, uint16_t height) -> bool
    {
        return State::get().ptr_local()->level_gen->data->set_room_template_size(room_template, width, height);
    };

    /// Get the inverse chance of a procedural spawn for the current level.
    /// A return value of 0 does not mean the chance is infinite, it means the chance is zero.
    lua["get_procedural_spawn_chance"] = [](PROCEDURAL_CHANCE chance_id) -> uint32_t
    {
        return State::get().ptr_local()->level_gen->get_procedural_spawn_chance(chance_id);
    };

    /// Gets the sub theme of the current cosmic ocean level, returns COSUBTHEME.NONE if the current level is not a CO level.
    lua["get_co_subtheme"] = get_co_subtheme;
    /// Forces the theme of the next cosmic ocean level(s) (use e.g. `force_co_subtheme(COSUBTHEME.JUNGLE)`. Use COSUBTHEME.RESET to reset to default random behaviour)
    lua["force_co_subtheme"] = force_co_subtheme;

    /// Gets the value for the specified config
    lua["get_level_config"] = [](LEVEL_CONFIG config) -> uint32_t
    {
        return State::get().ptr_local()->level_gen->data->level_config[config];
    };

    auto grow_vines = sol::overload(
        static_cast<void (*)(LAYER, uint32_t)>(::grow_vines),
        static_cast<void (*)(LAYER, uint32_t, AABB, bool)>(::grow_vines));

    /// Grow vines from `GROWABLE_VINE` and `VINE_TREE_TOP` entities in a level, `area` default is whole level, `destroy_broken` default is false
    lua["grow_vines"] = grow_vines;

    auto grow_poles = sol::overload(
        static_cast<void (*)(LAYER, uint32_t)>(::grow_poles),
        static_cast<void (*)(LAYER, uint32_t, AABB, bool)>(::grow_poles));

    /// Grow pole from `GROWABLE_CLIMBING_POLE` entities in a level, `area` default is whole level, `destroy_broken` default is false
    lua["grow_poles"] = grow_poles;

    auto grow_chain_and_blocks = sol::overload(
        static_cast<bool (*)(uint32_t, uint32_t)>(::grow_chain_and_blocks),
        static_cast<bool (*)()>(::grow_chain_and_blocks));

    /// Grow chains from `ENT_TYPE_FLOOR_CHAIN_CEILING` and chain with blocks on it from `ENT_TYPE_FLOOR_CHAINANDBLOCKS_CEILING`, it starts looking for the ceilings from the top left corner of a level.
    /// To limit it use the parameters, so x = 10 will only grow chains from ceilings with x < 10, with y = 10 it's ceilings that have y > (level bound top - 10)
    lua["grow_chainandblocks"] = grow_chain_and_blocks;

    /// Immediately load a screen based on [state](#state).screen_next and stuff
    lua["load_screen"] = do_load_screen;

    auto themeinfo_type = lua.new_usertype<ThemeInfo>("ThemeInfo");
    themeinfo_type["sub_theme"] = &ThemeInfo::sub_theme;
    themeinfo_type["get_unknown1"] = &ThemeInfo::get_unknown1;
    themeinfo_type["init_flags"] = &ThemeInfo::init_flags;
    themeinfo_type["init_level"] = &ThemeInfo::init_level;
    themeinfo_type["unknown_v4"] = &ThemeInfo::unknown_v4;
    themeinfo_type["unknown_v5"] = &ThemeInfo::unknown_v5;
    themeinfo_type["add_special_rooms"] = &ThemeInfo::add_special_rooms;
    themeinfo_type["unknown_v7"] = &ThemeInfo::unknown_v7;
    themeinfo_type["unknown_v8"] = &ThemeInfo::unknown_v8;
    themeinfo_type["add_vault"] = &ThemeInfo::add_vault;
    themeinfo_type["add_coffin"] = &ThemeInfo::add_coffin;
    themeinfo_type["add_special_feeling"] = &ThemeInfo::add_special_feeling;
    themeinfo_type["unknown_v12"] = &ThemeInfo::unknown_v12;
    themeinfo_type["spawn_level"] = &ThemeInfo::spawn_level;
    themeinfo_type["spawn_border"] = &ThemeInfo::spawn_border;
    themeinfo_type["post_process_level"] = &ThemeInfo::post_process_level;
    themeinfo_type["spawn_traps"] = &ThemeInfo::spawn_traps;
    themeinfo_type["post_process_entities"] = &ThemeInfo::post_process_entities;
    themeinfo_type["spawn_procedural"] = &ThemeInfo::spawn_procedural;
    themeinfo_type["spawn_background"] = &ThemeInfo::spawn_background;
    themeinfo_type["spawn_lights"] = &ThemeInfo::spawn_lights;
    themeinfo_type["spawn_transition"] = &ThemeInfo::spawn_transition;
    themeinfo_type["post_transition"] = &ThemeInfo::post_transition;
    themeinfo_type["spawn_players"] = &ThemeInfo::spawn_players;
    themeinfo_type["spawn_effects"] = &ThemeInfo::spawn_effects;
    themeinfo_type["get_level_file"] = &ThemeInfo::get_level_file;
    themeinfo_type["get_theme_id"] = &ThemeInfo::get_theme_id;
    themeinfo_type["get_base_id"] = &ThemeInfo::get_base_id;
    themeinfo_type["get_floor_spreading_type"] = &ThemeInfo::get_floor_spreading_type;
    themeinfo_type["get_floor_spreading_type2"] = &ThemeInfo::get_floor_spreading_type2;
    themeinfo_type["unknown_v30"] = &ThemeInfo::unknown_v30;
    themeinfo_type["get_transition_block_modifier"] = &ThemeInfo::get_transition_block_modifier;
    themeinfo_type["unknown_v32"] = &ThemeInfo::unknown_v32;
    themeinfo_type["get_backwall_type"] = &ThemeInfo::get_backwall_type;
    themeinfo_type["get_border_type"] = &ThemeInfo::get_border_type;
    themeinfo_type["get_critter_type"] = &ThemeInfo::get_critter_type;
    themeinfo_type["get_liquid_gravity"] = &ThemeInfo::get_liquid_gravity;
    themeinfo_type["get_player_damage"] = &ThemeInfo::get_player_damage;
    themeinfo_type["unknown_v38"] = &ThemeInfo::unknown_v38;
    themeinfo_type["get_backlayer_lut"] = &ThemeInfo::get_backlayer_lut;
    themeinfo_type["get_backlayer_light_level"] = &ThemeInfo::get_backlayer_light_level;
    themeinfo_type["get_loop"] = &ThemeInfo::get_loop;
    themeinfo_type["get_vault_level"] = &ThemeInfo::get_vault_level;
    themeinfo_type["get_unknown_1_or_2"] = &ThemeInfo::get_unknown_1_or_2;
    themeinfo_type["get_dynamic_texture"] = &ThemeInfo::get_dynamic_texture;
    themeinfo_type["pre_transition"] = &ThemeInfo::pre_transition;
    themeinfo_type["get_level_height"] = &ThemeInfo::get_level_height;
    themeinfo_type["unknown_v47"] = &ThemeInfo::unknown_v47;
    themeinfo_type["spawn_decoration"] = &ThemeInfo::spawn_decoration;
    themeinfo_type["spawn_decoration2"] = &ThemeInfo::spawn_decoration2;
    themeinfo_type["spawn_extra"] = &ThemeInfo::spawn_extra;
    themeinfo_type["unknown_v51"] = &ThemeInfo::unknown_v51;

    auto theme_override = sol::overload(
        static_cast<void (CustomTheme::*)(THEME_OVERRIDE, bool)>(&CustomTheme::override),
        static_cast<void (CustomTheme::*)(THEME_OVERRIDE, uint8_t)>(&CustomTheme::override),
        static_cast<void (CustomTheme::*)(THEME_OVERRIDE, sol::function)>(&CustomTheme::override));

    /// Customizable ThemeInfo with ability to override certain theming functions from different themes or write custom functions. Check ThemeInfo for some notes on the vanilla theme functions. Warning: We WILL change these function names, especially the unknown ones, when you figure out what they do.
    auto customtheme_type = lua.new_usertype<CustomTheme>("CustomTheme", sol::constructors<CustomTheme(), CustomTheme(uint8_t, uint8_t), CustomTheme(uint8_t, uint8_t, bool)>());
    customtheme_type["level_file"] = &CustomTheme::level_file;
    customtheme_type["theme"] = &CustomTheme::theme;
    customtheme_type["base_theme"] = sol::property([](CustomTheme& ct) -> uint8_t
                                                   {
                          if (ct.base_theme < UINT8_MAX)
                              return ct.base_theme + 1;
                          return UINT8_MAX; },
                                                   [](CustomTheme& ct, uint8_t bt)
                                                   {
                                                       if (bt > 0 && bt < UINT8_MAX)
                                                           ct.base_theme = bt - 1;
                                                       else
                                                           ct.base_theme = 0;
                                                   });
    customtheme_type["sub_theme"] = &CustomTheme::sub_theme;
    customtheme_type["textures"] = &CustomTheme::textures;
    customtheme_type["override"] = theme_override;
    customtheme_type["pre"] = &CustomTheme::pre;
    customtheme_type["post"] = &CustomTheme::post;
    customtheme_type["unknown1"] = &CustomTheme::unknown1;
    customtheme_type["unknown2"] = &CustomTheme::unknown2;
    customtheme_type["unknown3"] = &CustomTheme::unknown3;
    customtheme_type["unknown4"] = &CustomTheme::unknown4;
    customtheme_type["get_unknown1"] = &CustomTheme::get_unknown1;
    customtheme_type["init_flags"] = &CustomTheme::init_flags;
    customtheme_type["init_level"] = &CustomTheme::init_level;
    customtheme_type["unknown_v4"] = &CustomTheme::unknown_v4;
    customtheme_type["unknown_v5"] = &CustomTheme::unknown_v5;
    customtheme_type["add_special_rooms"] = &CustomTheme::add_special_rooms;
    customtheme_type["unknown_v7"] = &CustomTheme::unknown_v7;
    customtheme_type["unknown_v8"] = &CustomTheme::unknown_v8;
    customtheme_type["add_vault"] = &CustomTheme::add_vault;
    customtheme_type["add_coffin"] = &CustomTheme::add_coffin;
    customtheme_type["add_special_feeling"] = &CustomTheme::add_special_feeling;
    customtheme_type["unknown_v12"] = &CustomTheme::unknown_v12;
    customtheme_type["spawn_level"] = &CustomTheme::spawn_level;
    customtheme_type["spawn_border"] = &CustomTheme::spawn_border;
    customtheme_type["post_process_level"] = &CustomTheme::post_process_level;
    customtheme_type["spawn_traps"] = &CustomTheme::spawn_traps;
    customtheme_type["post_process_entities"] = &CustomTheme::post_process_entities;
    customtheme_type["spawn_procedural"] = &CustomTheme::spawn_procedural;
    customtheme_type["spawn_background"] = &CustomTheme::spawn_background;
    customtheme_type["spawn_lights"] = &CustomTheme::spawn_lights;
    customtheme_type["spawn_transition"] = &CustomTheme::spawn_transition;
    customtheme_type["post_transition"] = &CustomTheme::post_transition;
    customtheme_type["spawn_players"] = &CustomTheme::spawn_players;
    customtheme_type["spawn_effects"] = &CustomTheme::spawn_effects;
    customtheme_type["get_level_file"] = &CustomTheme::get_level_file;
    customtheme_type["get_theme_id"] = &CustomTheme::get_theme_id;
    customtheme_type["get_base_id"] = &CustomTheme::get_base_id;
    customtheme_type["get_floor_spreading_type"] = &CustomTheme::get_floor_spreading_type;
    customtheme_type["get_floor_spreading_type2"] = &CustomTheme::get_floor_spreading_type2;
    customtheme_type["unknown_v30"] = &CustomTheme::unknown_v30;
    customtheme_type["get_transition_block_modifier"] = &CustomTheme::get_transition_block_modifier;
    customtheme_type["unknown_v32"] = &CustomTheme::unknown_v32;
    customtheme_type["get_backwall_type"] = &CustomTheme::get_backwall_type;
    customtheme_type["get_border_type"] = &CustomTheme::get_border_type;
    customtheme_type["get_critter_type"] = &CustomTheme::get_critter_type;
    customtheme_type["get_liquid_gravity"] = &CustomTheme::get_liquid_gravity;
    customtheme_type["get_player_damage"] = &CustomTheme::get_player_damage;
    customtheme_type["unknown_v38"] = &CustomTheme::unknown_v38;
    customtheme_type["get_backlayer_lut"] = &CustomTheme::get_backlayer_lut;
    customtheme_type["get_backlayer_light_level"] = &CustomTheme::get_backlayer_light_level;
    customtheme_type["get_loop"] = &CustomTheme::get_loop;
    customtheme_type["get_vault_level"] = &CustomTheme::get_vault_level;
    customtheme_type["get_unknown_1_or_2"] = &CustomTheme::get_unknown_1_or_2;
    customtheme_type["get_dynamic_texture"] = &CustomTheme::get_dynamic_texture;
    customtheme_type["pre_transition"] = &CustomTheme::pre_transition;
    customtheme_type["get_level_height"] = &CustomTheme::get_level_height;
    customtheme_type["unknown_v47"] = &CustomTheme::unknown_v47;
    customtheme_type["spawn_decoration"] = &CustomTheme::spawn_decoration;
    customtheme_type["spawn_decoration2"] = &CustomTheme::spawn_decoration2;
    customtheme_type["spawn_extra"] = &CustomTheme::spawn_extra;
    customtheme_type["unknown_v51"] = &CustomTheme::unknown_v51;

    /* CustomTheme
    // override
    // `override(THEME_OVERRIDE override, bool enabled)` To disable or enable theme functions using the base_theme.
    // `override(THEME_OVERRIDE override, THEME theme)` To override a theme function with another theme.
    // `override(THEME_OVERRIDE override, function func)` To override a theme function with a lua function.
    */

    lua.create_named_table("DYNAMIC_TEXTURE", "INVISIBLE", DYNAMIC_TEXTURE::INVISIBLE, "BACKGROUND", DYNAMIC_TEXTURE::BACKGROUND, "FLOOR", DYNAMIC_TEXTURE::FLOOR, "DOOR", DYNAMIC_TEXTURE::DOOR, "DOOR_LAYER", DYNAMIC_TEXTURE::DOOR_LAYER, "BACKGROUND_DECORATION", DYNAMIC_TEXTURE::BACKGROUND_DECORATION, "KALI_STATUE", DYNAMIC_TEXTURE::KALI_STATUE, "COFFIN", DYNAMIC_TEXTURE::COFFIN);

    /// Overrides for different CustomTheme functions. Warning: We WILL change these, especially the unknown ones, and even the known ones if they turn out wrong in testing.
    lua.create_named_table("THEME_OVERRIDE", "BASE", THEME_OVERRIDE::BASE, "UNKNOWN_V1", THEME_OVERRIDE::UNKNOWN_V1, "INIT_FLAGS", THEME_OVERRIDE::INIT_FLAGS, "INIT_LEVEL", THEME_OVERRIDE::INIT_LEVEL, "UNKNOWN_V4", THEME_OVERRIDE::UNKNOWN_V4, "UNKNOWN_V5", THEME_OVERRIDE::UNKNOWN_V5, "SPECIAL_ROOMS", THEME_OVERRIDE::SPECIAL_ROOMS, "UNKNOWN_V7", THEME_OVERRIDE::UNKNOWN_V7, "UNKNOWN_V8", THEME_OVERRIDE::UNKNOWN_V8, "VAULT", THEME_OVERRIDE::VAULT, "COFFIN", THEME_OVERRIDE::COFFIN, "FEELING", THEME_OVERRIDE::FEELING, "UNKNOWN_V12", THEME_OVERRIDE::UNKNOWN_V12, "SPAWN_LEVEL", THEME_OVERRIDE::SPAWN_LEVEL, "SPAWN_BORDER", THEME_OVERRIDE::SPAWN_BORDER, "POST_PROCESS_LEVEL", THEME_OVERRIDE::POST_PROCESS_LEVEL, "SPAWN_TRAPS", THEME_OVERRIDE::SPAWN_TRAPS, "POST_PROCESS_ENTITIES", THEME_OVERRIDE::POST_PROCESS_ENTITIES, "SPAWN_PROCEDURAL", THEME_OVERRIDE::SPAWN_PROCEDURAL, "SPAWN_BACKGROUND", THEME_OVERRIDE::SPAWN_BACKGROUND, "SPAWN_LIGHTS", THEME_OVERRIDE::SPAWN_LIGHTS, "SPAWN_TRANSITION", THEME_OVERRIDE::SPAWN_TRANSITION, "POST_TRANSITION", THEME_OVERRIDE::POST_TRANSITION, "SPAWN_PLAYERS", THEME_OVERRIDE::SPAWN_PLAYERS, "SPAWN_EFFECTS", THEME_OVERRIDE::SPAWN_EFFECTS, "LVL_FILE", THEME_OVERRIDE::LVL_FILE, "THEME_ID", THEME_OVERRIDE::THEME_ID, "BASE_ID", THEME_OVERRIDE::BASE_ID, "ENT_FLOOR_SPREADING", THEME_OVERRIDE::ENT_FLOOR_SPREADING, "ENT_FLOOR_SPREADING2", THEME_OVERRIDE::ENT_FLOOR_SPREADING2, "UNKNOWN_V30", THEME_OVERRIDE::UNKNOWN_V30, "TRANSITION_MODIFIER", THEME_OVERRIDE::TRANSITION_MODIFIER, "UNKNOWN_V32", THEME_OVERRIDE::UNKNOWN_V32, "ENT_BACKWALL", THEME_OVERRIDE::ENT_BACKWALL, "ENT_BORDER", THEME_OVERRIDE::ENT_BORDER, "ENT_CRITTER", THEME_OVERRIDE::ENT_CRITTER, "GRAVITY", THEME_OVERRIDE::GRAVITY, "PLAYER_DAMAGE", THEME_OVERRIDE::PLAYER_DAMAGE, "UNKNOWN_V38", THEME_OVERRIDE::UNKNOWN_V38, "TEXTURE_BACKLAYER_LUT", THEME_OVERRIDE::TEXTURE_BACKLAYER_LUT, "BACKLAYER_LIGHT_LEVEL", THEME_OVERRIDE::BACKLAYER_LIGHT_LEVEL, "LOOP", THEME_OVERRIDE::LOOP, "VAULT_LEVEL", THEME_OVERRIDE::VAULT_LEVEL, "GET_UNKNOWN1_OR_2", THEME_OVERRIDE::GET_UNKNOWN1_OR_2, "TEXTURE_DYNAMIC", THEME_OVERRIDE::TEXTURE_DYNAMIC, "PRE_TRANSITION", THEME_OVERRIDE::PRE_TRANSITION, "LEVEL_HEIGHT", THEME_OVERRIDE::LEVEL_HEIGHT, "UNKNOWN_V47", THEME_OVERRIDE::UNKNOWN_V47, "SPAWN_DECORATION", THEME_OVERRIDE::SPAWN_DECORATION, "SPAWN_DECORATION2", THEME_OVERRIDE::SPAWN_DECORATION2, "SPAWN_EXTRA", THEME_OVERRIDE::SPAWN_EXTRA, "UNKNOWN_V51", THEME_OVERRIDE::UNKNOWN_V51);

    /// Force a theme in PRE_LOAD_LEVEL_FILES, POST_ROOM_GENERATION or PRE_LEVEL_GENERATION to change different aspects of the levelgen. You can pass a CustomTheme, ThemeInfo or THEME.
    // lua["force_custom_theme"] = [](* customtheme)
    lua["force_custom_theme"] = sol::overload(
        [](CustomTheme* customtheme)
        {
            State::get().ptr()->current_theme = customtheme;
        },
        [](ThemeInfo* customtheme)
        {
            State::get().ptr()->current_theme = customtheme;
        },
        [](uint32_t customtheme)
        {
            if (customtheme < 18)
                State::get().ptr()->current_theme = State::get().ptr()->level_gen->themes[customtheme - 1];
        });

    /// Force current subtheme used in the CO theme. You can pass a CustomTheme, ThemeInfo or THEME. Not to be confused with force_co_subtheme.
    // lua["force_custom_subtheme"] = [](* customtheme)
    lua["force_custom_subtheme"] = sol::overload(
        [](CustomTheme* customtheme)
        {
            State::get().ptr()->level_gen->theme_cosmicocean->sub_theme = customtheme;
        },
        [](ThemeInfo* customtheme)
        {
            State::get().ptr()->level_gen->theme_cosmicocean->sub_theme = customtheme;
        },
        [](uint32_t customtheme)
        {
            if (customtheme < 18)
                State::get().ptr()->level_gen->theme_cosmicocean->sub_theme = State::get().ptr()->level_gen->themes[customtheme - 1];
        });

    /// Context received in ON.PRE_LOAD_LEVEL_FILES, used for forcing specific `.lvl` files to load.
    lua.new_usertype<PreLoadLevelFilesContext>(
        "PreLoadLevelFilesContext",
        sol::no_constructor,
        "override_level_files",
        &PreLoadLevelFilesContext::override_level_files,
        "add_level_files",
        &PreLoadLevelFilesContext::add_level_files);

    /// Deprecated
    ///  kept for backward compatibility, don't use, check LevelGenSystem.exit_doors
    lua.new_usertype<DoorCoords>("DoorCoords", sol::no_constructor, "door1_x", &DoorCoords::door1_x, "door1_y", &DoorCoords::door1_y, "door2_x", &DoorCoords::door2_x, "door2_y", &DoorCoords::door2_y);

    /// Data relating to level generation, changing anything in here from ON.LEVEL or later will likely have no effect, used in StateMemory
    lua.new_usertype<LevelGenSystem>(
        "LevelGenSystem",
        sol::no_constructor,
        "shop_type",
        &LevelGenSystem::shop_type,
        "backlayer_shop_type",
        &LevelGenSystem::backlayer_shop_type,
        "spawn_x",
        &LevelGenSystem::spawn_x,
        "spawn_y",
        &LevelGenSystem::spawn_y,
        "spawn_room_x",
        &LevelGenSystem::spawn_room_x,
        "spawn_room_y",
        &LevelGenSystem::spawn_room_y,
        "exits",
        &LevelGenSystem::exit_doors_locations,
        "exit_doors",
        &LevelGenSystem::exit_doors,
        "themes",
        sol::property([](LevelGenSystem& lgs)
                      { return std::ref(lgs.themes); }),
        "flags",
        &LevelGenSystem::flags);

    /// Context received in ON.POST_ROOM_GENERATION.
    /// Used to change the room templates in the level and other shenanigans that affect level gen.
    lua.new_usertype<PostRoomGenerationContext>(
        "PostRoomGenerationContext",
        "set_room_template",
        &PostRoomGenerationContext::set_room_template,
        "mark_as_machine_room_origin",
        &PostRoomGenerationContext::mark_as_machine_room_origin,
        "mark_as_set_room",
        &PostRoomGenerationContext::mark_as_set_room,
        "unmark_as_set_room",
        &PostRoomGenerationContext::unmark_as_set_room,
        "set_shop_type",
        &PostRoomGenerationContext::set_shop_type,
        "set_procedural_spawn_chance",
        &PostRoomGenerationContext::set_procedural_spawn_chance,
        "set_num_extra_spawns",
        &PostRoomGenerationContext::set_num_extra_spawns,
        "define_short_tile_code",
        &PostRoomGenerationContext::define_short_tile_code,
        "change_short_tile_code",
        &PostRoomGenerationContext::change_short_tile_code);

    auto find_all_short_tile_codes = [&lua](PreHandleRoomTilesContext& ctx, LAYER layer, SHORT_TILE_CODE short_tile_code)
    {
        auto positions = ctx.find_all_short_tile_codes(layer, short_tile_code);
        sol::table positions_converted = lua.create_table();
        for (size_t i = 0; i < positions.size(); i++)
        {
            auto& tuple = positions[i];
            sol::table tuple_table = lua.create_table();
            tuple_table[1] = std::get<0>(tuple);
            tuple_table[2] = std::get<1>(tuple);
            tuple_table[3] = std::get<2>(tuple);
            positions_converted[i + 1] = tuple_table;
        }
        return positions_converted;
    };

    /// Context received in ON.PRE_HANDLE_ROOM_TILES.
    /// Used to change the room data as well as add a backlayer room if none is set yet.
    lua.new_usertype<PreHandleRoomTilesContext>(
        "PreHandleRoomTilesContext",
        sol::no_constructor,
        "get_short_tile_code",
        &PreHandleRoomTilesContext::get_short_tile_code,
        "set_short_tile_code",
        &PreHandleRoomTilesContext::set_short_tile_code,
        "find_all_short_tile_codes",
        find_all_short_tile_codes,
        "replace_short_tile_code",
        &PreHandleRoomTilesContext::replace_short_tile_code,
        "has_back_layer",
        &PreHandleRoomTilesContext::has_back_layer,
        "add_empty_back_layer",
        &PreHandleRoomTilesContext::add_empty_back_layer,
        "add_copied_back_layer",
        &PreHandleRoomTilesContext::add_copied_back_layer);

    /// Used in [get_short_tile_code](#get_short_tile_code), [get_short_tile_code_definition](#get_short_tile_code_definition) and PostRoomGenerationContext
    lua.new_usertype<ShortTileCodeDef>(
        "ShortTileCodeDef",
        "tile_code",
        &ShortTileCodeDef::tile_code,
        "chance",
        &ShortTileCodeDef::chance,
        "alt_tile_code",
        &ShortTileCodeDef::alt_tile_code);

    lua.new_usertype<QuestsInfo>(
        "QuestsInfo",
        "yang_state",
        &QuestsInfo::yang_state,
        "jungle_sisters_flags",
        &QuestsInfo::jungle_sisters_flags,
        "van_horsing_state",
        &QuestsInfo::van_horsing_state,
        "sparrow_state",
        &QuestsInfo::sparrow_state,
        "madame_tusk_state",
        &QuestsInfo::madame_tusk_state,
        "beg_state",
        &QuestsInfo::beg_state);

    auto savedata_type = lua.new_usertype<SaveData>("SaveData");
    savedata_type["places"] = &SaveData::places;
    savedata_type["bestiary"] = &SaveData::bestiary;
    savedata_type["people"] = &SaveData::people;
    savedata_type["items"] = &SaveData::items;
    savedata_type["traps"] = &SaveData::traps;
    savedata_type["last_daily"] = &SaveData::last_daily;
    savedata_type["characters"] = &SaveData::characters;
    savedata_type["shortcuts"] = &SaveData::shortcuts;
    savedata_type["bestiary_killed"] = &SaveData::bestiary_killed;
    savedata_type["bestiary_killed_by"] = &SaveData::bestiary_killed_by;
    savedata_type["people_killed"] = &SaveData::people_killed;
    savedata_type["people_killed_by"] = &SaveData::people_killed_by;
    savedata_type["plays"] = &SaveData::plays;
    savedata_type["deaths"] = &SaveData::deaths;
    savedata_type["wins_normal"] = &SaveData::wins_normal;
    savedata_type["wins_hard"] = &SaveData::wins_hard;
    savedata_type["wins_special"] = &SaveData::wins_special;
    savedata_type["score_total"] = &SaveData::score_total;
    savedata_type["score_top"] = &SaveData::score_top;
    savedata_type["deepest_area"] = &SaveData::deepest_area;
    savedata_type["deepest_level"] = &SaveData::deepest_level;
    savedata_type["time_best"] = &SaveData::time_best;
    savedata_type["time_total"] = &SaveData::time_total;
    savedata_type["time_tutorial"] = &SaveData::time_tutorial;
    savedata_type["character_deaths"] = &SaveData::character_deaths;
    savedata_type["pets_rescued"] = &SaveData::pets_rescued;
    savedata_type["completed_normal"] = &SaveData::completed_normal;
    savedata_type["completed_ironman"] = &SaveData::completed_ironman;
    savedata_type["completed_hard"] = &SaveData::completed_hard;
    savedata_type["profile_seen"] = &SaveData::profile_seen;
    savedata_type["seeded_unlocked"] = &SaveData::seeded_unlocked;
    savedata_type["world_last"] = &SaveData::world_last;
    savedata_type["level_last"] = &SaveData::level_last;
    savedata_type["score_last"] = &SaveData::score_last;
    savedata_type["time_last"] = &SaveData::time_last;
    savedata_type["stickers"] = &SaveData::stickers;
    savedata_type["players"] = &SaveData::players;
    savedata_type["constellation"] = &SaveData::constellation;

    lua.new_usertype<Constellation>(
        "Constellation",
        "star_count",
        &Constellation::star_count,
        "stars",
        &Constellation::stars,
        "scale",
        &Constellation::scale,
        "line_count",
        &Constellation::line_count,
        "lines",
        &Constellation::lines,
        "line_red_intensity",
        &Constellation::line_red_intensity);

    lua.new_usertype<ConstellationStar>(
        "ConstellationStar",
        "type",
        &ConstellationStar::type,
        "x",
        &ConstellationStar::x,
        "y",
        &ConstellationStar::y,
        "size",
        &ConstellationStar::size,
        "red",
        &ConstellationStar::red,
        "green",
        &ConstellationStar::green,
        "blue",
        &ConstellationStar::blue,
        "alpha",
        &ConstellationStar::alpha,
        "halo_red",
        &ConstellationStar::halo_red,
        "halo_green",
        &ConstellationStar::halo_green,
        "halo_blue",
        &ConstellationStar::halo_blue,
        "halo_alpha",
        &ConstellationStar::halo_alpha,
        "canis_ring",
        &ConstellationStar::canis_ring,
        "fidelis_ring",
        &ConstellationStar::fidelis_ring);

    lua.new_usertype<ConstellationLine>(
        "ConstellationLine",
        "from",
        &ConstellationLine::from,
        "to",
        &ConstellationLine::to);

    lua.create_named_table("LAYER", "FRONT", 0, "BACK", 1, "PLAYER", -1, "PLAYER1", -1, "PLAYER2", -2, "PLAYER3", -3, "PLAYER4", -4, "BOTH", -128);
    lua.create_named_table(
        "THEME",
        "DWELLING",
        1,
        "JUNGLE",
        2,
        "VOLCANA",
        3,
        "OLMEC",
        4,
        "TIDE_POOL",
        5,
        "TEMPLE",
        6,
        "ICE_CAVES",
        7,
        "NEO_BABYLON",
        8,
        "SUNKEN_CITY",
        9,
        "COSMIC_OCEAN",
        10,
        "CITY_OF_GOLD",
        11,
        "DUAT",
        12,
        "ABZU",
        13,
        "TIAMAT",
        14,
        "EGGPLANT_WORLD",
        15,
        "HUNDUN",
        16,
        "BASE_CAMP",
        17,
        "ARENA",
        18);

    /// Parameter to force_co_subtheme
    lua.create_named_table("COSUBTHEME", "NONE", -1, "RESET", -1, "DWELLING", 0, "JUNGLE", 1, "VOLCANA", 2, "TIDE_POOL", 3, "TEMPLE", 4, "ICE_CAVES", 5, "NEO_BABYLON", 6, "SUNKEN_CITY", 7);

    /// Yang quest states
    lua.create_named_table("YANG", "ANGRY", -1, "QUEST_NOT_STARTED", 0, "TURKEY_PEN_SPAWNED", 2, "BOTH_TURKEYS_DELIVERED", 3, "TURKEY_SHOP_SPAWNED", 4, "ONE_TURKEY_BOUGHT", 5, "TWO_TURKEYS_BOUGHT", 6, "THREE_TURKEYS_BOUGHT", 7);

    /// Jungle sister quest flags (angry = -1)
    lua.create_named_table("JUNGLESISTERS", "PARSLEY_RESCUED", 1, "PARSNIP_RESCUED", 2, "PARMESAN_RESCUED", 3, "WARNING_ONE_WAY_DOOR", 4, "GREAT_PARTY_HUH", 5, "I_WISH_BROUGHT_A_JACKET", 6);

    /// Van Horsing quest states
    lua.create_named_table("VANHORSING", "QUEST_NOT_STARTED", 0, "JAILCELL_SPAWNED", 1, "FIRST_ENCOUNTER_DIAMOND_THROWN", 2, "SPAWNED_IN_VLADS_CASTLE", 3, "SHOT_VLAD", 4, "TEMPLE_HIDEOUT_SPAWNED", 5, "SECOND_ENCOUNTER_COMPASS_THROWN", 6, "TUSK_CELLAR", 7);

    /// Sparrow quest states
    lua.create_named_table("SPARROW", "ANGRY", -2, "DEAD", -1, "QUEST_NOT_STARTED", 0, "THIEF_STATUS", 1, "FINISHED_LEVEL_WITH_THIEF_STATUS", 2, "FIRST_HIDEOUT_SPAWNED_ROPE_THROW", 3, "FIRST_ENCOUNTER_ROPES_THROWN", 4, "TUSK_IDOL_STOLEN", 5, "SECOND_HIDEOUT_SPAWNED_NEOBAB", 6, "SECOND_ENCOUNTER_INTERACTED", 7, "MEETING_AT_TUSK_BASEMENT", 8, "FINAL_REWARD_THROWN", 9);

    /// Madame Tusk quest states
    lua.create_named_table("TUSK", "ANGRY", -2, "DEAD", -1, "QUEST_NOT_STARTED", 0, "DICE_HOUSE_SPAWNED", 1, "HIGH_ROLLER_STATUS", 2, "PALACE_WELCOME_MESSAGE", 3);

    /// Beg quest states
    lua.create_named_table("BEG", "QUEST_NOT_STARTED", 0, "ALTAR_DESTROYED", 1, "SPAWNED_WITH_BOMBBAG", 2, "BOMBBAG_THROWN", 3, "SPAWNED_WITH_TRUECROWN", 4, "TRUECROWN_THROWN", 5);

    /// Use in `define_room_template` to declare whether a room template has any special behavior
    lua.create_named_table("ROOM_TEMPLATE_TYPE", "NONE", 0, "ENTRANCE", 1, "EXIT", 2, "SHOP", 3, "MACHINE_ROOM", 4);

    /// Determines which kind of shop spawns in the level, if any
    lua.create_named_table(
        "SHOP_TYPE",
        "GENERAL_STORE",
        0,
        "CLOTHING_SHOP",
        1,
        "WEAPON_SHOP",
        2,
        "SPECIALTY_SHOP",
        3,
        "HIRED_HAND_SHOP",
        4,
        "PET_SHOP",
        5,
        "DICE_SHOP",
        6,
        "HEDJET_SHOP",
        8,
        "CURIO_SHOP",
        9,
        "CAVEMAN_SHOP",
        10,
        "TURKEY_SHOP",
        11,
        "GHIST_SHOP",
        12,
        "TUSK_DICE_SHOP",
        13);

    /// Use with `get_level_config`
    lua.create_named_table(
        "LEVEL_CONFIG",
        "BACK_ROOM_CHANCE",
        0,
        "BACK_ROOM_INTERCONNECTION_CHANCE",
        1,
        "BACK_ROOM_HIDDEN_DOOR_CHANCE",
        2,
        "BACK_ROOM_HIDDEN_DOOR_CACHE_CHANCE",
        3,
        "MOUNT_CHANCE",
        4,
        "ALTAR_ROOM_CHANCE",
        5,
        "IDOL_ROOM_CHANCE",
        6,
        "FLOOR_SIDE_SPREAD_CHANCE",
        7,
        "FLOOR_BOTTOM_SPREAD_CHANCE",
        8,
        "BACKGROUND_CHANCE",
        9,
        "GROUND_BACKGROUND_CHANCE",
        10,
        "MACHINE_BIGROOM_CHANCE",
        11,
        "MACHINE_WIDEROOM_CHANCE",
        12,
        "MACHINE_TALLROOM_CHANCE",
        13,
        "MACHINE_REWARDROOM_CHANCE",
        14,
        "MAX_LIQUID_PARTICLES",
        15,
        "FLAGGED_LIQUID_ROOMS",
        16);

    StateMemory* main_state = State::get().ptr_main();

    lua.create_named_table("TILE_CODE"
                           //, "EMPTY", 0
                           //, "", ...check__[tile_codes.txt]\[game_data/tile_codes.txt\]...
    );
    for (const auto& [tile_code_name, tile_code] : main_state->level_gen->data->tile_codes)
    {
        std::string clean_tile_code_name = tile_code_name.c_str();
        std::transform(
            clean_tile_code_name.begin(), clean_tile_code_name.end(), clean_tile_code_name.begin(), [](unsigned char c)
            { return (unsigned char)std::toupper(c); });
        std::replace(clean_tile_code_name.begin(), clean_tile_code_name.end(), '-', '_');
        lua["TILE_CODE"][std::move(clean_tile_code_name)] = tile_code.id;
    };

    lua.create_named_table("ROOM_TEMPLATE"
                           //, "SIDE", 0
                           //, "", ...check__[room_templates.txt]\[game_data/room_templates.txt\]...
    );

    auto room_templates = main_state->level_gen->data->room_templates;
    room_templates["empty_backlayer"] = {9};
    room_templates["boss_arena"] = {22};
    room_templates["shop_jail_backlayer"] = {44};
    room_templates["waddler"] = {86};
    room_templates["ghistshop_backlayer"] = {87};
    room_templates["challange_entrance_backlayer"] = {90};
    room_templates["blackmarket"] = {118};
    room_templates["mothership_room"] = {125};
    for (const auto& [room_name, room_template] : room_templates)
    {
        std::string clean_room_name = room_name.c_str();
        std::transform(
            clean_room_name.begin(), clean_room_name.end(), clean_room_name.begin(), [](unsigned char c)
            { return (unsigned char)std::toupper(c); });
        std::replace(clean_room_name.begin(), clean_room_name.end(), '-', '_');
        lua["ROOM_TEMPLATE"][std::move(clean_room_name)] = room_template.id;
    };

    lua.create_named_table("PROCEDURAL_CHANCE"
                           //, "ARROWTRAP_CHANCE", 0
                           //, "", ...check__[spawn_chances.txt]\[game_data/spawn_chances.txt\]...
    );
    for (auto* chances : {&main_state->level_gen->data->monster_chances, &main_state->level_gen->data->trap_chances})
    {
        for (const auto& [chance_name, chance] : *chances)
        {
            std::string clean_chance_name = chance_name.c_str();
            std::transform(
                clean_chance_name.begin(), clean_chance_name.end(), clean_chance_name.begin(), [](unsigned char c)
                { return (unsigned char)std::toupper(c); });
            std::replace(clean_chance_name.begin(), clean_chance_name.end(), '-', '_');
            lua["PROCEDURAL_CHANCE"][std::move(clean_chance_name)] = chance.id;
        }
    }

    lua.create_named_table("POS_TYPE", "FLOOR", POS_TYPE::FLOOR, "CEILING", POS_TYPE::CEILING, "AIR", POS_TYPE::AIR, "WALL", POS_TYPE::WALL, "ALCOVE", POS_TYPE::ALCOVE, "PIT", POS_TYPE::PIT, "HOLE", POS_TYPE::HOLE, "WATER", POS_TYPE::WATER, "LAVA", POS_TYPE::LAVA, "SAFE", POS_TYPE::SAFE, "EMPTY", POS_TYPE::EMPTY, "SOLID", POS_TYPE::SOLID, "DEFAULT", POS_TYPE::DEFAULT, "WALL_LEFT", POS_TYPE::WALL_LEFT, "WALL_RIGHT", POS_TYPE::WALL_RIGHT);

    /* POS_TYPE
    // FLOOR
    // On top of solid floor
    // CEILING
    // Below solid ceiling
    // AIR
    // Is a non-solid tile (no need to explicitly add this to everything)
    // WALL
    // Next to a wall
    // WALL_LEFT
    // Next to a wall on the left
    // WALL_RIGHT
    // Next to a wall on the right
    // ALCOVE
    // Has a floor, ceiling and exactly one wall
    // PIT
    // Has a floor, two walls and no ceiling
    // HOLE
    // Air pocket surrounded by floors
    // WATER
    // Is in water (otherwise assumed not in water)
    // LAVA
    // Is in lava (otherwise assumed not in lava)
    // SAFE
    // Avoid hazards, like certain traps, shops and any special floor
    // EMPTY
    // Has nothing but decoration and background in it
    // SOLID
    // Is inside solid floor or activefloor
    // DEFAULT
    // FLOOR | SAFE | EMPTY
    */
}
}; // namespace NLevel
