cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" PLUGIN_VERSION LIMIT_COUNT 1)
project(get-icon VERSION "${PLUGIN_VERSION}" LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../4D-Plugin-SDK/4D Plugin API")
set(PLUGIN_SOURCES get-icon-4dplugin.cpp "${SDK_DIR}/4DPluginAPI.c")
set_source_files_properties("${SDK_DIR}/4DPluginAPI.c" PROPERTIES LANGUAGE CXX)
set(TEST_PLUGINS "${CMAKE_CURRENT_SOURCE_DIR}/get-icon-test/Plugins")

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    add_library(icon-native STATIC icon-macos.mm)
    target_compile_options(icon-native PRIVATE -fobjc-arc)
    target_link_libraries(icon-native PUBLIC "-framework AppKit" "-framework CoreServices" "-framework ImageIO")
    add_library(get-icon MODULE ${PLUGIN_SOURCES} manifest.json constants.xlf)
    set_source_files_properties(manifest.json constants.xlf PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(get-icon PROPERTIES
        BUNDLE TRUE BUNDLE_EXTENSION bundle
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        LIBRARY_OUTPUT_DIRECTORY "${TEST_PLUGINS}")
    foreach(CONFIG DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(get-icon PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY_${CONFIG} "${TEST_PLUGINS}")
    endforeach()
    target_link_libraries(get-icon PRIVATE "-framework CoreGraphics")
    set_source_files_properties("${SDK_DIR}/4DPluginAPI.c" PROPERTIES
        COMPILE_OPTIONS "-Wno-multichar;-Wno-int-conversion")
elseif(WIN32)
    add_library(icon-native STATIC icon-windows.cpp)
    target_compile_definitions(icon-native PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(icon-native PUBLIC shell32 ole32 windowscodecs gdi32 advapi32)
    add_library(get-icon SHARED ${PLUGIN_SOURCES} manifest.json constants.xlf)
    set_target_properties(get-icon PROPERTIES SUFFIX ".4DX"
        RUNTIME_OUTPUT_DIRECTORY "${TEST_PLUGINS}/get-icon/Contents/Windows64"
        LINK_FLAGS "/DEF:\"${SDK_DIR}/4DPluginAPI.def\"")
    foreach(CONFIG DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(get-icon PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${CONFIG} "${TEST_PLUGINS}/get-icon/Contents/Windows64")
    endforeach()
    add_custom_command(TARGET get-icon POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TEST_PLUGINS}/get-icon/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/constants.xlf"
            "${TEST_PLUGINS}/get-icon/Contents/Resources")
else()
    message(FATAL_ERROR "Only macOS and Windows are supported")
endif()
target_include_directories(icon-native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(get-icon PRIVATE "${SDK_DIR}")
target_compile_definitions(get-icon PRIVATE UNICODE _UNICODE)
target_link_libraries(get-icon PRIVATE icon-native)

include(CTest)
if(BUILD_TESTING)
    add_executable(icon-string-ownership-tests tests/string-ownership.cpp "${SDK_DIR}/4DPluginAPI.c")
    target_include_directories(icon-string-ownership-tests PRIVATE "${SDK_DIR}")
    if(APPLE)
        target_link_libraries(icon-string-ownership-tests PRIVATE "-framework CoreGraphics")
    endif()
    add_test(NAME string-ownership COMMAND icon-string-ownership-tests)
    if(APPLE)
        add_executable(icon-native-tests tests/macos.mm)
        target_compile_options(icon-native-tests PRIVATE -fobjc-arc)
        add_executable(icon-main-process-tests tests/main-process.cpp "${SDK_DIR}/4DPluginAPI.c")
        target_include_directories(icon-main-process-tests PRIVATE "${SDK_DIR}")
        target_link_libraries(icon-main-process-tests PRIVATE "-framework CoreGraphics")
        add_test(NAME main-process-icons COMMAND icon-main-process-tests)
    else()
        add_executable(icon-native-tests tests/windows.cpp)
    endif()
    target_link_libraries(icon-native-tests PRIVATE icon-native)
    add_test(NAME native-icons COMMAND icon-native-tests)
endif()
