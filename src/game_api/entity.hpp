#pragma once

#include <cstddef>       // for size_t
#include <cstdint>       // for uint8_t, uint32_t, int32_t, uint16_t, int64_t
#include <functional>    // for function, equal_to
#include <span>          // for span
#include <string>        // for allocator, string
#include <string_view>   // for string_view
#include <tuple>         // for tuple
#include <type_traits>   // for move
#include <unordered_map> // for _Umap_traits<>::allocator_type, unordered_map
#include <utility>       // for pair
#include <vector>        // for vector

#include "aliases.hpp"        // for ENT_TYPE, LAYER, TEXTURE, STRINGID
#include "color.hpp"          // for Color
#include "entity_db.hpp"      // for EntityDB
#include "entity_structs.hpp" // for CollisionInfo
#include "layer.hpp"          // for EntityList
#include "math.hpp"           // for AABB

struct RenderInfo;
struct Texture;
struct SoundMeta;

class Entity;
class Movable;

struct EntityHooksInfo;
using ENT_FLAG = uint32_t;
using ENT_MORE_FLAG = uint32_t;

class Entity
{
  public:
    /// Type of the entity, contains special properties etc. If you want to edit them just for this entity look at the EntityDB
    EntityDB* type;
    Entity* overlay;
    EntityList items;
    /// see [flags.hpp](https://github.com/spelunky-fyi/overlunky/blob/main/src/game_api/flags.hpp) entity_flags
    ENT_FLAG flags;
    /// see [flags.hpp](https://github.com/spelunky-fyi/overlunky/blob/main/src/game_api/flags.hpp) more_flags
    ENT_MORE_FLAG more_flags;
    /// Unique id of the entity, save it to variable to check this entity later (don't use the whole Entity type as it will be replaced with a different one when this is destroyed)
    int32_t uid;
    /// Number (id) of the sprite in the texture
    uint16_t animation_frame;
    /// Depth level that this entity is drawn on.
    /// Don't edit this directly, use `set_draw_depth` function
    uint8_t draw_depth;
    uint8_t b3f; // depth related, changed when going thru doors etc.
    /// Position of the entity, can be relative to the platform you standing on (pushblocks, elevators), use [get_position](#get_position) to get accurate position in the game world
    float x;
    /// Position of the entity, can be relative to the platform you standing on (pushblocks, elevators), use [get_position](#get_position) to get accurate position in the game world
    float y;
    float abs_x; // only for movable entities, or entities that can be spawned without overlay, for the rest it's FLOAT_MIN?
    float abs_y;
    /// Width of the sprite
    float w;
    /// Height of the sprite
    float h;
    /// Special offset used for entities attached to others (or picked by others) that need to flip to the other side when the parent flips sides
    float special_offsetx;
    /// Special offset used for entities attached to others (or picked by others) that need to flip to the other side when the parent flips sides
    float special_offsety;
    Color color;
    union
    {
        struct
        {
            /// Offset of the hitbox in relation to the entity position
            float offsetx;
            /// Offset of the hitbox in relation to the entity position
            float offsety;
            /// Half of the width of the hitbox
            float hitboxx;
            /// Half of the height of the hitbox
            float hitboxy;
            SHAPE shape;         // 1 = rectangle, 2 = circle
            bool hitbox_enabled; // probably, off for bg, deco, logical etc
            uint8_t b82;
            uint8_t b83;
        };
        CollisionInfo collision_info;
    };
    float angle;
    RenderInfo* rendering_info;
    Texture* texture;
    /// Size of the sprite in the texture
    float tilew;
    /// Size of the sprite in the texture
    float tileh;
    /// Use `set_layer` to change
    uint8_t layer;
    uint8_t b99; // this looks like FLOORSTYLED post-processing
    uint8_t b9a;
    uint8_t b9b;
    uint32_t i9c;
    /* for the autodoc
    any user_data;
    */

    size_t pointer()
    {
        return (size_t)this;
    }

    std::pair<float, float> position();

    void teleport(float dx, float dy, bool s, float vx, float vy, bool snap);
    void teleport_abs(float dx, float dy, float vx, float vy);

    /// Moves the entity to specified layer, nothing else happens, so this does not emulate a door transition
    void set_layer(LAYER layer);
    /// Moves the entity to the limbo-layer where it can later be retrieved from again via `respawn`
    void remove();
    /// Moves the entity from the limbo-layer (where it was previously put by `remove`) to `layer`
    void respawn(LAYER layer);
    /// Performs a teleport as if the entity had a teleporter and used it. The delta coordinates are where you want the entity to teleport to relative to its current position, in tiles (so integers, not floats). Positive numbers = to the right and up, negative left and down.
    void perform_teleport(uint8_t delta_x, uint8_t delta_y);

    Entity* topmost()
    {
        auto cur = this;
        while (cur->overlay)
        {
            cur = cur->overlay;
        }
        return cur;
    }

    Entity* topmost_mount()
    {
        auto topmost = this;
        while (auto cur = topmost->overlay)
        {
            if (cur->type->search_flags <= 2)
            {
                topmost = cur;
            }
            else
                break;
        }
        return topmost;
    }

    bool overlaps_with(AABB hitbox)
    {
        return overlaps_with(hitbox.left, hitbox.bottom, hitbox.right, hitbox.top);
    }

    /// Deprecated
    /// Use `overlaps_with(AABB hitbox)` instead
    bool overlaps_with(float rect_left, float rect_bottom, float rect_right, float rect_top)
    {
        const auto [posx, posy] = position();
        const float left = posx - hitboxx + offsetx;
        const float right = posx + hitboxx + offsetx;
        const float bottom = posy - hitboxy + offsety;
        const float top = posy + hitboxy + offsety;

        return left < rect_right && rect_left < right && bottom < rect_top && rect_bottom < top;
    }

    bool overlaps_with(Entity* other)
    {
        const auto [other_posx, other_posy] = other->position();
        const float other_left = other_posx - other->hitboxx + other->offsetx;
        const float other_right = other_posx + other->hitboxx + other->offsetx;
        const float other_top = other_posy + other->hitboxy + other->offsety;
        const float other_bottom = other_posy - other->hitboxy + other->offsety;

        return overlaps_with(other_left, other_bottom, other_right, other_top);
    }

    std::pair<float, float> position_self() const;
    void remove_item(uint32_t item_uid);

    TEXTURE get_texture();
    /// Changes the entity texture, check the [textures.txt](game_data/textures.txt) for available vanilla textures or use [define_texture](#define_texture) to make custom one
    bool set_texture(TEXTURE texture_id);

    bool is_player();
    bool is_movable();
    bool is_liquid();
    bool is_cursed()
    {
        return more_flags & 0x4000;
    };

    // for supporting HookableVTable
    uint32_t get_aux_id() const
    {
        return uid;
    }

    // Needed despite HookableVTable for cleanup of arbitrary entity related data
    std::uint32_t set_on_dtor(std::function<void(Entity*)> cb)
    {
        return hook_dtor_impl(this, std::move(cb));
    }
    void clean_on_dtor(std::uint32_t dtor_cb_id)
    {
        clear_dtor_impl(this, dtor_cb_id);
    }

    void set_enable_turning(bool enabled);

    std::span<uint32_t> get_items();

    template <typename T>
    T* as()
    {
        return static_cast<T*>(this);
    }

    static void set_hook_dtor_impl(
        std::function<std::uint32_t(Entity*, std::function<void(Entity*)>)> hook_fun,
        std::function<void(Entity*, std::uint32_t)> clear_fun)
    {
        hook_dtor_impl = std::move(hook_fun);
        clear_dtor_impl = std::move(clear_fun);
    }
    inline static std::function<std::uint32_t(Entity*, std::function<void(Entity*)>)> hook_dtor_impl{};
    inline static std::function<void(Entity*, std::uint32_t)> clear_dtor_impl{};

    virtual ~Entity() = 0; // vritual 0
    virtual void create_rendering_info() = 0;
    virtual void handle_state_machine() = 0;

    /// Kills the entity, you can set responsible to `nil` to ignore it
    virtual void kill(bool destroy_corpse, Entity* responsible) = 0;

    virtual void on_collision1(Entity* other_entity) = 0; // triggers on collision between whip and hit object

    /// Completely removes the entity from existence
    virtual void destroy() = 0;

    virtual void apply_texture(Texture*) = 0;
    virtual void format_shopitem_name(char16_t*) = 0;
    virtual void generate_stomp_damage_particles(Entity* victim) = 0; // particles when jumping on top of enemy
    virtual float get_type_field_a8() = 0;
    virtual bool can_be_pushed() = 0; // (runs only for activefloors?) checks if entity type is pushblock, for chained push block checks ChainedPushBlock.is_chained, is only a check that allows for the pushing animation
    virtual bool v11() = 0;           // for arrows: returns true if it's moving (for y possibily checks for some speed as well?)
    /// Returns true if entity is in water/lava
    virtual bool is_in_liquid() = 0;
    virtual bool check_type_properties_flags_19() = 0; // checks (properties_flags >> 0x12) & 1; for hermitcrab checks if he's invisible; can't get it to trigger
    virtual uint32_t get_type_field_60() = 0;
    virtual void set_invisible(bool value) = 0;
    virtual void handle_turning_left(bool apply) = 0; // if disabled, monsters don't turn left and keep walking in the wall (and other right-left issues)
    virtual void set_draw_depth(uint8_t draw_depth) = 0;
    virtual void resume_ai() = 0; // works on entities with ai_func != 0; runs when companions are let go from being held. AI resumes anyway in 1.23.3
    virtual float friction() = 0;
    virtual void set_as_sound_source(SoundMeta*) = 0; // update sound position to entity position?
    virtual void remove_item_ptr(Entity*) = 0;
    virtual Entity* get_held_entity() = 0;
    virtual void v23(Entity* logical_trigger, Entity* who_triggered_it) = 0; // spawns LASERTRAP_SHOT from LASERTRAP, also some trigger entities use this, seam to be called right after "on_collision2", tiggers use self as the first parameter
    /// Triggers weapons and other held items like teleportter, mattock etc. You can check the [virtual-availability.md](https://github.com/spelunky-fyi/overlunky/blob/main/docs/virtual-availability.md), if entity has `open` in the `on_open` you can use this function, otherwise it does nothing. Returns false if action could not be performed (cooldown is not 0, no arrow loaded in etc. the animation could still be played thou)
    virtual bool trigger_action(Entity* user) = 0;
    /// Activates a button prompt (with the Use door/Buy button), e.g. buy shop item, activate drill, read sign, interact in camp, ... `get_entity(<udjat socket uid>):activate(players[1])` (make sure player 1 has the udjat eye though)
    virtual void activate(Entity* activator) = 0;

    virtual void on_collision2(Entity* other_entity) = 0; // needs investigating, difference between this and on_collision1, maybe this is on_hitbox_overlap as it works for logical tiggers

    /// e.g. for turkey: stores health, poison/curse state, for mattock: remaining swings (returned value is transferred)
    virtual uint16_t get_metadata() = 0;
    virtual void apply_metadata(uint16_t metadata) = 0;
    virtual void on_walked_on_by(Entity* walker) = 0;  // hits when monster/player walks on a floor, does something when walker.velocityy<-0.21 (falling onto) and walker.hitboxy * hitboxx > 0.09
    virtual void on_walked_off_by(Entity* walker) = 0; // appears to be disabled in 1.23.3? hits when monster/player walks off a floor, it checks whether the walker has floor as overlay, and if so, removes walker from floor's items by calling virtual remove_item_ptr
    virtual void on_ledge_grab(Entity* who) = 0;       // only ACTIVEFLOOR_FALLING_PLATFORM, does something with game menager
    virtual void on_stood_on_by(Entity* entity) = 0;   // e.g. pots, skulls, pushblocks, ... standing on floors
    virtual void toggle_backlayer_illumination() = 0;  // only for CHAR_*: when going to the backlayer, turns on player emitted light
    virtual void v34() = 0;                            // only ITEM_TORCH, calls Torch.light_up(false), can't get it to trigger
    virtual void liberate_from_shop() = 0;             // can also be seen as event: when you anger the shopkeeper, this function gets called for each item; can be called on shopitems individually as well and they become 'purchased'

    /// Applies changes made in `entity.type`
    virtual void apply_db() = 0; // This is actually just an initialize call that is happening once after  the entity is created
};

std::tuple<float, float, uint8_t> get_position(uint32_t uid);
std::tuple<float, float, uint8_t> get_render_position(uint32_t uid);

std::tuple<float, float> get_velocity(uint32_t uid);

AABB get_hitbox(uint32_t uid, bool use_render_pos);

struct EntityFactory* entity_factory();
Entity* get_entity_ptr(uint32_t uid);
Entity* get_entity_ptr_local(uint32_t uid);
