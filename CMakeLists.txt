cmake_minimum_required(VERSION 3.21)
project(ck_mpm_vulkan_starter LANGUAGES CXX)

find_package(Vulkan 1.3 REQUIRED)
find_package(glfw3 3.3 REQUIRED)
find_package(glm CONFIG REQUIRED)
find_program(GLSLC glslc REQUIRED)

foreach(STAGE vert frag)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/mesh.${STAGE}.spv
    COMMAND ${GLSLC} -O ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh.${STAGE}
            -o ${CMAKE_CURRENT_BINARY_DIR}/mesh.${STAGE}.spv
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh.${STAGE}
    VERBATIM)
  list(APPEND SHADERS ${CMAKE_CURRENT_BINARY_DIR}/mesh.${STAGE}.spv)
endforeach()

add_executable(ck_mpm_starter src/main.cpp src/renderer.cpp src/vulkan_context.cpp src/scene.cpp ${SHADERS})
target_compile_features(ck_mpm_starter PRIVATE cxx_std_20)
target_compile_definitions(ck_mpm_starter PRIVATE VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1 VULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS=1)
target_link_libraries(ck_mpm_starter PRIVATE glfw Vulkan::Vulkan glm::glm)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(ck_mpm_starter PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(scene_test tests/scene_test.cpp src/scene.cpp)
  target_compile_features(scene_test PRIVATE cxx_std_20)
  target_include_directories(scene_test PRIVATE src)
  target_link_libraries(scene_test PRIVATE glm::glm)
  add_test(NAME scene_validation COMMAND scene_test)
endif()
