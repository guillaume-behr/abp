set(ABP_WEBUI_HTML "${CMAKE_CURRENT_SOURCE_DIR}/gui/web/index.html")
set(ABP_WEBUI_GENERATED "${CMAKE_BINARY_DIR}/generated/WebUi.cpp")

add_custom_command(
    OUTPUT "${ABP_WEBUI_GENERATED}"
    COMMAND ${CMAKE_COMMAND} -DINPUT=${ABP_WEBUI_HTML} -DOUTPUT=${ABP_WEBUI_GENERATED}
            -P "${CMAKE_SOURCE_DIR}/cmake/EmbedWebUi.cmake"
    DEPENDS "${ABP_WEBUI_HTML}" "${CMAKE_SOURCE_DIR}/cmake/EmbedWebUi.cmake"
    COMMENT "Embedding the web GUI into the binary"
    VERBATIM
)

set(ABP_CORE_SOURCES
    util/StringUtil.cpp
    util/Process.cpp
    util/Json.cpp
    util/Sha256.cpp
    util/FsUtil.cpp
    util/DevicePaths.cpp
    util/Logger.cpp
    adb/AdbClient.cpp
    backup/ArchiveIntegrity.cpp
    backup/Manifest.cpp
    backup/StandardBackend.cpp
    backup/RootBackend.cpp
    backup/BackupManager.cpp
    backup/BackupStore.cpp
    gui/HttpServer.cpp
    gui/GuiServer.cpp
    cli/Cli.cpp
    "${ABP_WEBUI_GENERATED}"
)

add_library(abp_core STATIC ${ABP_CORE_SOURCES})

target_include_directories(abp_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/generated
)

target_compile_features(abp_core PUBLIC cxx_std_17)
abp_set_warnings(abp_core)

find_package(Threads REQUIRED)
target_link_libraries(abp_core PUBLIC Threads::Threads)

add_executable(abp main.cpp)
target_link_libraries(abp PRIVATE abp_core)
abp_set_warnings(abp)
