cmake_minimum_required(VERSION 3.20)

# Set the plugin name — change this for your project
set(PLUGIN_NAME "example" CACHE STRING "Plugin name")

project(${PLUGIN_NAME} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 17)

# --- SDK paths ---
set(SDK_DIR "${CMAKE_SOURCE_DIR}/../4D-Plugin-SDK/4D Plugin API")

# --- Source files ---
set(PLUGIN_SOURCES
    ${PLUGIN_NAME}-4dplugin.cpp
    "${SDK_DIR}/4DPluginAPI.c"
)

set(PLUGIN_HEADERS
    ${PLUGIN_NAME}-4dplugin.h
    "${SDK_DIR}/4DPluginAPI.h"
    "${SDK_DIR}/EntryPoints.h"
    "${SDK_DIR}/PublicTypes.h"
    "${SDK_DIR}/PrivateTypes.h"
    "${SDK_DIR}/Flags.h"
)

set(PLUGIN_RESOURCES
    manifest.json
    constants.xlf
)

# --- Platform-specific target setup ---
if(APPLE)
    # macOS: create a .bundle
    add_library(${PLUGIN_NAME} MODULE ${PLUGIN_SOURCES} ${PLUGIN_HEADERS} ${PLUGIN_RESOURCES})

    set_target_properties(${PLUGIN_NAME} PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "bundle"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.4d.${PLUGIN_NAME}"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    )

    # Copy resources into the bundle
    set_source_files_properties(${PLUGIN_RESOURCES} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources
    )

    # Link CoreGraphics (required by SDK for Quartz axis functions)
    find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    target_link_libraries(${PLUGIN_NAME} PRIVATE ${COREGRAPHICS_FRAMEWORK})

    # Output to test project Plugins folder
    set_target_properties(${PLUGIN_NAME} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${PLUGIN_NAME}-test/Plugins"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/${PLUGIN_NAME}-test/Plugins"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/${PLUGIN_NAME}-test/Plugins"
    )

elseif(WIN32)
    # Windows: create a DLL with .4DX extension
    add_library(${PLUGIN_NAME} SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

    set_target_properties(${PLUGIN_NAME} PROPERTIES
        SUFFIX ".4DX"
        # Module definition file exports FourDPackex
        LINK_FLAGS "/DEF:\"${SDK_DIR}/4DPluginAPI.def\""
    )

    # Static CRT linkage
    set_property(TARGET ${PLUGIN_NAME} PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )

    # Output to test project Plugins folder (Windows structure)
    set(PLUGIN_OUT_DIR "${CMAKE_SOURCE_DIR}/${PLUGIN_NAME}-test/Plugins/${PLUGIN_NAME}/Contents/Windows64")
    set(RESOURCE_OUT_DIR "${CMAKE_SOURCE_DIR}/${PLUGIN_NAME}-test/Plugins/${PLUGIN_NAME}/Contents/Resources")

    set_target_properties(${PLUGIN_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PLUGIN_OUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PLUGIN_OUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PLUGIN_OUT_DIR}"
    )

    # Post-build: copy resources
    add_custom_command(TARGET ${PLUGIN_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RESOURCE_OUT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/manifest.json"
            "${CMAKE_SOURCE_DIR}/constants.xlf"
            "${RESOURCE_OUT_DIR}"
        COMMENT "Copying manifest.json and constants.xlf to Resources"
    )
endif()

# --- Common settings ---
target_include_directories(${PLUGIN_NAME} PRIVATE "${SDK_DIR}")

# Compile 4DPluginAPI.c as C++
set_source_files_properties("${SDK_DIR}/4DPluginAPI.c" PROPERTIES LANGUAGE CXX)

# Unicode charset
target_compile_definitions(${PLUGIN_NAME} PRIVATE UNICODE _UNICODE)
