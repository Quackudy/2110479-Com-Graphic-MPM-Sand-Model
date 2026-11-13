cmake_minimum_required(VERSION 3.24)
project(ck_mpm_vulkan_starter LANGUAGES CXX)

# One flat output directory for every generator: shaders (.spv) are loaded from
# next to the executable, and the generator expression stops multi-config
# generators (Visual Studio) from appending Debug/ or Release/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}>")

find_package(Vulkan 1.3 REQUIRED)

# Prefer system packages; download pinned versions only when they are missing.
include(FetchContent)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw3 GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4 GIT_SHALLOW TRUE FIND_PACKAGE_ARGS 3.3 NAMES glfw3)
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.1 GIT_SHALLOW TRUE FIND_PACKAGE_ARGS CONFIG)
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3 GIT_SHALLOW TRUE FIND_PACKAGE_ARGS 3.10)
FetchContent_MakeAvailable(glfw3 glm nlohmann_json)
find_program(GLSLC glslc REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(EVALUATION_PYTHON ${Python3_EXECUTABLE})

set(SHADER_NAMES particle.vert particle.frag clear_grid.comp p2g.comp grid_update.comp g2p.comp material_update.comp particle_health.comp)
foreach(NAME IN LISTS SHADER_NAMES)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${NAME}.spv
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -I${CMAKE_CURRENT_SOURCE_DIR}/shaders
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${NAME} -o ${CMAKE_CURRENT_BINARY_DIR}/${NAME}.spv
    DEPENDS shaders/${NAME} shaders/mpm_common.glsl shaders/interpolation.glsl shaders/material.glsl
    VERBATIM)
  list(APPEND SHADERS ${CMAKE_CURRENT_BINARY_DIR}/${NAME}.spv)
endforeach()

add_executable(ck_mpm_starter src/main.cpp src/renderer.cpp src/vulkan_context.cpp src/scene.cpp
  src/simulation_config.cpp src/simulation.cpp src/scene_cache.cpp src/platform.cpp ${SHADERS})
target_link_libraries(ck_mpm_starter PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(ck_mpm_starter PRIVATE
  PROJECT_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}"
  EVALUATION_PYTHON="${EVALUATION_PYTHON}"
  PROJECT_BINARY_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
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

# Optional reference executable; the simulator itself has no CUDA dependency.
find_program(NVCC nvcc HINTS /opt/cuda/bin)
set(AUTHOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/CK-MPM)
if(NVCC AND EXISTS ${AUTHOR_DIR}/include/mpm_material.cuh)
  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/cuda_material_reference
    COMMAND ${NVCC} -std=c++20 -O2 --expt-relaxed-constexpr --extended-lambda
      -I${AUTHOR_DIR}/include -I${AUTHOR_DIR}/third_party/spdlog/include
      -I${AUTHOR_DIR}/third_party/json/include -I${AUTHOR_DIR}/third_party/partio/src/lib
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/cuda_material_reference.cu
      -o ${CMAKE_CURRENT_BINARY_DIR}/cuda_material_reference
    DEPENDS tools/cuda_material_reference.cu ${AUTHOR_DIR}/include/mpm_material.cuh ${AUTHOR_DIR}/include/mpm_math.cuh
    VERBATIM)
  add_custom_target(cuda_reference ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/cuda_material_reference)
endif()

if(BUILD_TESTING)
  add_executable(cache_test tests/cache_test.cpp src/scene_cache.cpp src/simulation_config.cpp src/platform.cpp)
  target_compile_features(cache_test PRIVATE cxx_std_20)
  target_include_directories(cache_test PRIVATE src)
  target_link_libraries(cache_test PRIVATE glm::glm nlohmann_json::nlohmann_json)
  add_test(NAME cache_and_config COMMAND cache_test)
  add_test(NAME numerical_reference COMMAND ck_mpm_starter evaluate)
  set_tests_properties(numerical_reference PROPERTIES LABELS "gpu" RUN_SERIAL TRUE TIMEOUT 180)
endif()

if(BUILD_TESTING)
  add_executable(particle_render_test tests/particle_render_test.cpp src/renderer.cpp src/vulkan_context.cpp src/scene.cpp)
  target_compile_features(particle_render_test PRIVATE cxx_std_20)
  target_compile_definitions(particle_render_test PRIVATE
    VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1 VULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS=1
    PROJECT_BINARY_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}")
  target_include_directories(particle_render_test PRIVATE src)
  target_link_libraries(particle_render_test PRIVATE glfw Vulkan::Vulkan glm::glm)
  add_dependencies(particle_render_test ck_mpm_starter)
  add_test(NAME particle_render COMMAND particle_render_test)
  add_test(NAME live_integration COMMAND ${EVALUATION_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/tests/live_smoke.py)
  set_tests_properties(particle_render live_integration PROPERTIES LABELS "gpu" RUN_SERIAL TRUE TIMEOUT 180
    FAIL_REGULAR_EXPRESSION "\\[Vulkan validation\\]")
endif()
