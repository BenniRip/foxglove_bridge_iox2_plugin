cmake_minimum_required(VERSION 3.22)

project(foxglove_bridge_iox2_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

function(enable_strict_compiler_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wold-style-cast -Wfloat-equal)
endfunction()

find_package(ament_cmake REQUIRED)
find_package(foxglove_bridge REQUIRED)
find_package(pluginlib REQUIRED)

find_package(iceoryx2-cxx QUIET)
if(NOT iceoryx2-cxx_FOUND)
  message(FATAL_ERROR
    "iceoryx2-cxx not found. It must be installed in the environment, built with "
    "IOX2_FEATURE_FLATBUFFERS=ON; add its install prefix to CMAKE_PREFIX_PATH.")
endif()

add_library(${PROJECT_NAME} SHARED
  src/iox2_node.cpp
  src/iox2_service_storage.cpp
  src/iox2_source.cpp
  src/iox2_source_settings.cpp
  src/plugin_support/instance_parameters.cpp
  src/plugin_support/problem_log.cpp
)
target_include_directories(${PROJECT_NAME} PRIVATE include)
target_link_libraries(${PROJECT_NAME}
  foxglove_bridge::plugin_api
  pluginlib::pluginlib
  # Static, so the installed plugin needs no iceoryx2 files.
  iceoryx2-cxx::static-lib-cxx
)
# Hides the static iceoryx2 and flatbuffers symbols from other copies in the process.
target_link_options(${PROJECT_NAME} PRIVATE "LINKER:--exclude-libs,ALL")
enable_strict_compiler_warnings(${PROJECT_NAME})

pluginlib_export_plugin_description_file(foxglove_bridge plugins.xml)

install(TARGETS ${PROJECT_NAME}
  LIBRARY DESTINATION lib
)
install(DIRECTORY config
  DESTINATION share/${PROJECT_NAME}/
)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(class_loader REQUIRED)
  find_package(nlohmann_json REQUIRED)
  find_package(rosgraph_msgs REQUIRED)
  find_package(websocketpp REQUIRED)

  # C++ header for the test publisher, .bfbs files for the plugin.
  find_program(FLATC flatc REQUIRED)
  set(TEST_FBS_OUT ${CMAKE_CURRENT_BINARY_DIR}/test_fbs)
  set(TEST_FBS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fbs/test_payload.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fbs/other_payload.fbs
  )
  set(TEST_FBS_OUTPUTS
    ${TEST_FBS_OUT}/test_payload_generated.h
    ${TEST_FBS_OUT}/test_payload.bfbs
    ${TEST_FBS_OUT}/other_payload.bfbs
  )
  add_custom_command(
    OUTPUT ${TEST_FBS_OUTPUTS}
    COMMAND ${FLATC} --cpp -o ${TEST_FBS_OUT} ${CMAKE_CURRENT_SOURCE_DIR}/tests/fbs/test_payload.fbs
    COMMAND ${FLATC} --binary --schema -o ${TEST_FBS_OUT} ${TEST_FBS_SOURCES}
    DEPENDS ${TEST_FBS_SOURCES}
    COMMENT "Generating test FlatBuffer schemas"
  )
  add_custom_target(test_fbs DEPENDS ${TEST_FBS_OUTPUTS})

  add_executable(iox2_test_publisher tests/iox2_test_publisher.cpp)
  add_dependencies(iox2_test_publisher test_fbs)
  target_include_directories(iox2_test_publisher PRIVATE ${TEST_FBS_OUT})
  target_link_libraries(iox2_test_publisher iceoryx2-cxx::static-lib-cxx)
  enable_strict_compiler_warnings(iox2_test_publisher)

  # C++17 for websocketpp 0.8. The test loads the plugin library instead of its C++23 headers.
  ament_add_gtest(iox2_source_test tests/iox2_source_test.cpp)
  set_target_properties(iox2_source_test PROPERTIES CXX_STANDARD 17)
  add_dependencies(iox2_source_test ${PROJECT_NAME} test_fbs iox2_test_publisher)
  target_include_directories(iox2_source_test PRIVATE ${TEST_FBS_OUT})
  target_compile_definitions(iox2_source_test PRIVATE
    ASIO_STANDALONE
    TEST_SCHEMA_DIR="${TEST_FBS_OUT}"
    TEST_PUBLISHER_PATH="$<TARGET_FILE:iox2_test_publisher>"
    PLUGIN_LIBRARY_PATH="$<TARGET_FILE:${PROJECT_NAME}>"
  )
  target_link_libraries(iox2_source_test
    class_loader::class_loader
    foxglove_bridge::plugin_api
    iceoryx2-cxx::static-lib-cxx
    nlohmann_json::nlohmann_json
    ${rosgraph_msgs_TARGETS}
    websocketpp::websocketpp
  )
  enable_strict_compiler_warnings(iox2_source_test)
endif()

ament_package()
