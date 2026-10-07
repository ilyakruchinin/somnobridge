cmake_minimum_required(VERSION 3.16)

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
