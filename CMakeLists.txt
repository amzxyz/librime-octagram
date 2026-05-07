# CMakeLists.txt
project(rime-octagram)
cmake_minimum_required(VERSION 3.14)

option(BUILD_TOOLS "Build tools" ON)

find_package(marisa QUIET)

if(marisa_FOUND)
    set(MARISA_TARGET marisa)
else()
    include(FetchContent)
    FetchContent_Declare(
      marisa_trie
      GIT_REPOSITORY https://github.com/s-yata/marisa-trie.git
      GIT_TAG        v0.3.1
    )
    FetchContent_MakeAvailable(marisa_trie)

    set(MARISA_TARGET marisa)
endif()

aux_source_directory(src octagram_src)

add_library(rime-octagram-objs OBJECT ${octagram_src})

if(NOT marisa_FOUND)
    target_include_directories(rime-octagram-objs PRIVATE ${marisa_trie_SOURCE_DIR}/include)
    target_include_directories(rime-octagram-objs PRIVATE ${marisa_trie_SOURCE_DIR}/lib)
endif()

if(BUILD_SHARED_LIBS)
  set_target_properties(rime-octagram-objs
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

set(plugin_name rime-octagram PARENT_SCOPE)
set(plugin_objs $<TARGET_OBJECTS:rime-octagram-objs> PARENT_SCOPE)
set(plugin_deps ${rime_library} ${MARISA_TARGET} PARENT_SCOPE)
set(plugin_modules "octagram" PARENT_SCOPE)

if(BUILD_TOOLS)
  add_subdirectory(tools)
endif()
