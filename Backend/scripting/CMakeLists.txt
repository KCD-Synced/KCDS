# Backend/scripting/ — resource host: local event bus + export registry + runtime interface (step 11).
# The concrete language runtimes (Backend/scripting/runtimes/{js,lua,csharp}) are prepare-only and built
# only where V8/LuaJIT/CoreCLR exist (a); the host here is headless.
add_library(kcd2s_scripting STATIC host.cpp resource.cpp resource_manager.cpp)
add_library(kcd2s::scripting ALIAS kcd2s_scripting)

target_include_directories(kcd2s_scripting PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcd2s_scripting PUBLIC kcd2s::core kcd2s::sdk kcd2s::flags)

if(KCD2S_BUILD_TESTS)
  kcd2s_add_test(NAME scripting.host TARGET test_scripting_host
                 SOURCES tests/host_test.cpp
                 LIBS kcd2s::scripting)
  kcd2s_add_test(NAME scripting.resource TARGET test_scripting_resource
                 SOURCES tests/resource_test.cpp
                 LIBS kcd2s::scripting)
  kcd2s_add_test(NAME scripting.resource_manager TARGET test_scripting_resource_manager
                 SOURCES tests/resource_manager_test.cpp
                 LIBS kcd2s::scripting)
endif()
