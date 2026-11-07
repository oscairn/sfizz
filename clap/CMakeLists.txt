# Oscairn Sampler, Oscairn's CLAP instrument around sfizz (doc/oscairn-plan.md, section 5).
# The CLAP headers and Oscairn's extension headers (both MIT) are vendored in third_party/
# with their commits.

add_library(oscairn_sampler_clap_headers INTERFACE)
target_include_directories(oscairn_sampler_clap_headers INTERFACE third_party/clap/include third_party/oscairn-clap/include)

# The SDK's voice-budget helper needs C++17; sfizz's headers stay C++14 (budget.h)
add_library(oscairn_sampler_clap_budget STATIC budget.cpp budget.h)
target_link_libraries(oscairn_sampler_clap_budget PUBLIC oscairn_sampler_clap_headers)
set_target_properties(oscairn_sampler_clap_budget PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_STANDARD 17)

add_library(oscairn_sampler_clap_plugin STATIC plugin.cpp plugin.h)
target_link_libraries(oscairn_sampler_clap_plugin PUBLIC oscairn_sampler_clap_headers sfizz::internal PRIVATE oscairn_sampler_clap_budget)
set_target_properties(oscairn_sampler_clap_plugin PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(oscairn_sampler_clap MODULE entry.cpp)
target_link_libraries(oscairn_sampler_clap PRIVATE oscairn_sampler_clap_plugin)
set_target_properties(oscairn_sampler_clap PROPERTIES
    PREFIX ""
    SUFFIX ".clap"
    OUTPUT_NAME oscairn-sampler
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
if(APPLE)
    target_link_options(oscairn_sampler_clap PRIVATE -Wl,-exported_symbol,_clap_entry)
endif()
install(TARGETS oscairn_sampler_clap LIBRARY DESTINATION lib/clap)

if(SFIZZ_TESTS)
    add_executable(oscairn_sampler_clap_tests test.cpp)
    target_link_libraries(oscairn_sampler_clap_tests PRIVATE oscairn_sampler_clap_plugin ${CMAKE_DL_LIBS})
    target_compile_definitions(oscairn_sampler_clap_tests PRIVATE OSCAIRN_TEST_FILES="${PROJECT_SOURCE_DIR}/tests/TestFiles")
    add_dependencies(oscairn_sampler_clap_tests oscairn_sampler_clap)
endif()
