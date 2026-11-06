# The server browser page, linked into the game client DLL (on CEF since milestone
# 0.1.2): an internal web view of Client/ui/web showing Client/ui/menu over the native main menu.
# See include/kcds/menu/compositor.hpp.
add_library(kcds_menu_compositor STATIC cef_menu.cpp)
add_library(kcds::menu_compositor ALIAS kcds_menu_compositor)
target_include_directories(kcds_menu_compositor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcds_menu_compositor PRIVATE kcd2s::flags kcds::web)
