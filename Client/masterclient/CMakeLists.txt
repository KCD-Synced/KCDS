# Client/masterclient/ — client-side master-server directory client + server-browser model
# Pure parse/encode/filter/sort; HTTP is injected (headless-testable).
add_library(kcd2s_masterclient STATIC server_browser.cpp frontend_menu.cpp menu_session.cpp)
add_library(kcd2s::masterclient ALIAS kcd2s_masterclient)

target_include_directories(kcd2s_masterclient PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# kcds::launcher supplies the bounded JSON reader and the shared host:port rule.
target_link_libraries(kcd2s_masterclient PUBLIC kcd2s::core kcd2s::flags kcds::launcher)
if(WIN32)
  # include/kcd2s/platform/http_win.hpp (header only, Windows only).
  target_link_libraries(kcd2s_masterclient PUBLIC winhttp)
endif()

if(KCD2S_BUILD_TESTS)
  kcd2s_add_test(NAME platform.server_browser TARGET test_platform_server_browser
                 SOURCES tests/server_browser_test.cpp
                 LIBS kcd2s::masterclient)
  # The master list's own answer (its private repository, KCD-Synced/KCDS-Master), when present.
  set(kcds_master_fixture "${CMAKE_SOURCE_DIR}/Backend/platform/testdata/serverlist.json")
  if(EXISTS "${kcds_master_fixture}")
    kcd2s_add_test(NAME platform.master_contract TARGET test_platform_master_contract
                   SOURCES tests/contract_test.cpp
                   LIBS kcd2s::masterclient)
    target_compile_definitions(test_platform_master_contract PRIVATE
      KCD2S_MASTER_FIXTURE="${kcds_master_fixture}")
  endif()
  kcd2s_add_test(NAME platform.frontend_menu TARGET test_platform_frontend_menu
                 SOURCES tests/frontend_menu_test.cpp
                 LIBS kcd2s::masterclient)
  kcd2s_add_test(NAME platform.menu_session TARGET test_platform_menu_session
                 SOURCES tests/menu_session_test.cpp
                 LIBS kcd2s::masterclient)
endif()
