#include "entities_floors_lua.hpp"

#include "entities_floors.hpp"
#include "entity.hpp"
#include "script/script_impl.hpp"

#include <sol/sol.hpp>

namespace NEntitiesFloors
{
void register_usertypes(sol::state& lua, ScriptImpl* script)
{
    lua["Entity"]["as_floor"] = &Entity::as<Floor>;
    lua["Entity"]["as_door"] = &Entity::as<Door>;
    lua["Entity"]["as_exitdoor"] = &Entity::as<ExitDoor>;
    lua["Entity"]["as_decorateddoor"] = &Entity::as<DecoratedDoor>;
    lua["Entity"]["as_lockeddoor"] = &Entity::as<LockedDoor>;
    lua["Entity"]["as_cityofgolddoor"] = &Entity::as<CityOfGoldDoor>;
    lua["Entity"]["as_mainexit"] = &Entity::as<MainExit>;
    lua["Entity"]["as_eggshipdoor"] = &Entity::as<EggShipDoor>;
    lua["Entity"]["as_arrowtrap"] = &Entity::as<Arrowtrap>;

    lua.new_usertype<Floor>(
        "Floor",
        "deco_top",
        &Floor::deco_top,
        "deco_bottom",
        &Floor::deco_bottom,
        "deco_left",
        &Floor::deco_left,
        "deco_right",
        &Floor::deco_right,
        "fix_border_tile_animation",
        &Floor::fix_border_tile_animation,
        "fix_decorations",
        &Floor::fix_decorations,
        "add_decoration",
        &Floor::add_decoration,
        "remove_decoration",
        &Floor::remove_decoration,
        sol::base_classes,
        sol::bases<Entity>());

    // The corner options only work for FLOOR_BORDERTILE and FLOOR_BORDERTILE_OCTOPUS
    lua.create_named_table(
        "FLOOR_SIDE",
        "TOP",
        FLOOR_SIDE::TOP,
        "BOTTOM",
        FLOOR_SIDE::BOTTOM,
        "LEFT",
        FLOOR_SIDE::LEFT,
        "RIGHT",
        FLOOR_SIDE::RIGHT,
        "TOP_LEFT",
        FLOOR_SIDE::TOP_LEFT,
        "TOP_RIGHT",
        FLOOR_SIDE::TOP_RIGHT,
        "BOTTOM_LEFT",
        FLOOR_SIDE::BOTTOM_LEFT,
        "BOTTOM_RIGHT",
        FLOOR_SIDE::BOTTOM_RIGHT);
    lua.new_usertype<Door>(
        "Door",
        "counter",
        &Door::counter,
        "fx_button",
        &Door::fx_button,
        sol::base_classes,
        sol::bases<Entity>());

    lua.new_usertype<ExitDoor>(
        "ExitDoor",
        "entered",
        &ExitDoor::entered,
        "special_door",
        &ExitDoor::special_door,
        "level",
        &ExitDoor::level,
        "timer",
        &ExitDoor::timer,
        "world",
        &ExitDoor::world,
        "theme",
        &ExitDoor::theme,
        sol::base_classes,
        sol::bases<Entity, Door>());

    lua.new_usertype<DecoratedDoor>(
        "DecoratedDoor",
        "special_bg",
        &DecoratedDoor::special_bg,
        sol::base_classes,
        sol::bases<Entity, Door, ExitDoor>());

    lua.new_usertype<LockedDoor>(
        "LockedDoor",
        "unlocked",
        &LockedDoor::unlocked,
        sol::base_classes,
        sol::bases<Entity, Door>());

    lua.new_usertype<CityOfGoldDoor>(
        "CityOfGoldDoor",
        "unlocked",
        &CityOfGoldDoor::unlocked,
        sol::base_classes,
        sol::bases<Entity, Door, ExitDoor, DecoratedDoor>());

    lua.new_usertype<MainExit>(
        "MainExit",
        sol::base_classes,
        sol::bases<Entity, Door, ExitDoor>());

    lua.new_usertype<EggShipDoor>(
        "EggShipDoor",
        "timer",
        &EggShipDoor::timer,
        "entered",
        &EggShipDoor::entered,
        sol::base_classes,
        sol::bases<Entity, Door>());

    lua.new_usertype<Arrowtrap>(
        "Arrowtrap",
        "arrow_shot",
        &Arrowtrap::arrow_shot,
        "rearm",
        &Arrowtrap::rearm,
        sol::base_classes,
        sol::bases<Entity>());
}
} // namespace NEntitiesFloors
