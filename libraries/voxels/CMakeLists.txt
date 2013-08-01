cmake_minimum_required(VERSION 2.8)

set(ROOT_DIR ../..)
set(MACRO_DIR ${ROOT_DIR}/cmake/macros)

# setup for find modules
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/modules/")

set(TARGET_NAME voxels)

include(${MACRO_DIR}/SetupHifiLibrary.cmake)
setup_hifi_library(${TARGET_NAME})

include(${MACRO_DIR}/IncludeGLM.cmake)
include_glm(${TARGET_NAME} ${ROOT_DIR})

include(${MACRO_DIR}/LinkHifiLibrary.cmake)
link_hifi_library(shared ${TARGET_NAME} ${ROOT_DIR})

# link ZLIB
if (WIN32)
  set(ZLIB_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../../externals/zlib/include)
  #set(ZLIB_LIBRARIES ${CMAKE_CURRENT_SOURCE_DIR}/../../externals/zlib/lib/Win32/z)
else (WIN32)
  find_package(ZLIB)
endif (WIN32)
include_directories(${ZLIB_INCLUDE_DIRS})
#target_link_libraries(${TARGET_NAME} ${ZLIB_LIBRARIES}) #don't think this is necessary

if (WIN32)
  include_directories( ${CMAKE_CURRENT_SOURCE_DIR}/../../externals/pthread/include )
  add_definitions(-DNOMINMAX)
endif (WIN32)
