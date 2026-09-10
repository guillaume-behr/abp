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
    cli/Cli.cpp
)

add_library(abp_core STATIC ${ABP_CORE_SOURCES})

target_include_directories(abp_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/generated
)

target_compile_features(abp_core PUBLIC cxx_std_17)
abp_set_warnings(abp_core)

add_executable(abp main.cpp)
target_link_libraries(abp PRIVATE abp_core)
abp_set_warnings(abp)
