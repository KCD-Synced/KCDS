# Native main-menu hooks, linked into the game client DLL: hide the native main-menu
# buttons under our server browser and filter single buttons (save/load in multiplayer). Uses the
# game client's MinHook instance; see include/kcds/menu_native/main_menu.hpp.
add_library(kcds_menu_native STATIC main_menu.cpp)
add_library(kcds::menu_native ALIAS kcds_menu_native)
target_include_directories(kcds_menu_native PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcds_menu_native PRIVATE kcd2s_runtime_minhook kcd2s::flags)
