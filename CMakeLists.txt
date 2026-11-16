cmake_minimum_required(VERSION 3.16)

# ESP-IDF derives PROJECT_VER from `git describe`, but the source tree is a
# bind mount inside the build container, so git refuses with "dubious
# ownership" and IDF silently falls back to the default "1" — which is what
# the LCD splash has been showing. Resolve it here instead, before project(),
# so esp_app_get_description()->version carries the real tag.
execute_process(
    COMMAND git -c safe.directory=* describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT PROJECT_VER)
    set(PROJECT_VER "unknown")
endif()

set(EXTRA_COMPONENT_DIRS components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(somnobridge)

idf_component_get_property(json_lib json COMPONENT_LIB)
target_compile_definitions(${json_lib} PRIVATE CJSON_NESTING_LIMIT=16)

if(SOMNOBRIDGE_STACK_AUDIT)
    idf_build_set_property(COMPILE_OPTIONS "-fstack-usage" APPEND)
endif()

if(SOMNOBRIDGE_HTTP_AUDIT)
    idf_component_get_property(ble_lib ble_api COMPONENT_LIB)
    target_compile_definitions(${ble_lib} PRIVATE SOMNOBRIDGE_HTTP_AUDIT=1)
endif()
