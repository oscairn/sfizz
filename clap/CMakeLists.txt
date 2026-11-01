# Oscairn's CLAP instrument around sfizz (doc/oscairn-plan.md, section 5).
# The CLAP headers (MIT) are vendored in third_party/clap with their commit.

add_library(sfizz_clap_headers INTERFACE)
target_include_directories(sfizz_clap_headers INTERFACE third_party/clap/include)

add_library(sfizz_clap_plugin STATIC plugin.cpp plugin.h)
target_link_libraries(sfizz_clap_plugin PUBLIC sfizz_clap_headers PRIVATE sfizz::static)
set_target_properties(sfizz_clap_plugin PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(sfizz_clap MODULE entry.cpp)
target_link_libraries(sfizz_clap PRIVATE sfizz_clap_plugin)
set_target_properties(sfizz_clap PROPERTIES
    PREFIX ""
    SUFFIX ".clap"
    OUTPUT_NAME oscairn-sfizz
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
if(APPLE)
    target_link_options(sfizz_clap PRIVATE -Wl,-exported_symbol,_clap_entry)
endif()
install(TARGETS sfizz_clap LIBRARY DESTINATION lib/clap)

if(SFIZZ_TESTS)
    add_executable(sfizz_clap_tests test.cpp)
    target_link_libraries(sfizz_clap_tests PRIVATE sfizz_clap_plugin ${CMAKE_DL_LIBS})
    add_dependencies(sfizz_clap_tests sfizz_clap)
endif()
