cmake_minimum_required(VERSION 3.21)

project(nixos-manager
    VERSION 0.1.0
    DESCRIPTION "Qt UI for managing NixOS configurations"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(NIXOS_MANAGER_BUILD_TESTS "Build the parser test suite" ON)
option(NIXOS_MANAGER_BUILD_GUI "Build the Qt Widgets application" ON)

include(GNUInstallDirs)

# ── Core library (no GUI dependency, so it can be tested headless) ────────────
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network)

add_library(nixos-manager-core STATIC
    src/core/NixLexer.cpp
    src/core/NixLexer.h
    src/core/NixFile.cpp
    src/core/NixFile.h
    src/core/ConfigProject.cpp
    src/core/ConfigProject.h
    src/core/PackageSearch.cpp
    src/core/PackageSearch.h
    src/core/CommandRunner.cpp
    src/core/CommandRunner.h
    src/core/SystemOps.cpp
    src/core/SystemOps.h
    src/core/GitRepo.cpp
    src/core/GitRepo.h
)
set_target_properties(nixos-manager-core PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)
target_include_directories(nixos-manager-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nixos-manager-core PUBLIC Qt6::Core Qt6::Network)
target_compile_definitions(nixos-manager-core PUBLIC
    NIXOS_MANAGER_VERSION="${PROJECT_VERSION}")

# ── Application ───────────────────────────────────────────────────────────────
if(NIXOS_MANAGER_BUILD_GUI)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Svg)
    qt_standard_project_setup()

    qt_add_executable(nixos-manager
        src/main.cpp
        src/ui/MainWindow.cpp
        src/ui/MainWindow.h
        src/ui/Theme.cpp
        src/ui/Theme.h
        src/ui/BrandHeader.cpp
        src/ui/BrandHeader.h
        src/ui/NixHighlighter.cpp
        src/ui/NixHighlighter.h
        src/ui/LogPane.cpp
        src/ui/LogPane.h
        src/ui/FileOps.cpp
        src/ui/FileOps.h
        src/ui/HostsPage.cpp
        src/ui/HostsPage.h
        src/ui/ModulesPage.cpp
        src/ui/ModulesPage.h
        src/ui/PackagesPage.cpp
        src/ui/PackagesPage.h
        src/ui/OptionsPage.cpp
        src/ui/OptionsPage.h
        src/ui/GitPage.cpp
        src/ui/GitPage.h
        src/ui/EditorPage.cpp
        src/ui/EditorPage.h
        src/ui/SystemPage.cpp
        src/ui/SystemPage.h
    )
    # The brand mark is shipped as artwork and rendered at runtime, rather than
    # being reimplemented in painting code. Drop a replacement at
    # share/icons/logo.svg (or logo.png) and rebuild to use your own file.
    set(NIXOS_MANAGER_LOGO share/icons/org.nixos.manager.svg)
    foreach(candidate share/icons/logo.svg share/icons/logo.png)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${candidate})
            set(NIXOS_MANAGER_LOGO ${candidate})
        endif()
    endforeach()
    message(STATUS "nixos-manager: using ${NIXOS_MANAGER_LOGO} as the application logo")

    get_filename_component(_logo_ext ${NIXOS_MANAGER_LOGO} EXT)
    qt_add_resources(nixos-manager "branding"
        PREFIX "/branding"
        BASE ${CMAKE_CURRENT_SOURCE_DIR}
        FILES ${NIXOS_MANAGER_LOGO})
    target_compile_definitions(nixos-manager PRIVATE
        NIXOS_MANAGER_LOGO_RESOURCE=":/branding/${NIXOS_MANAGER_LOGO}")

    target_link_libraries(nixos-manager PRIVATE nixos-manager-core Qt6::Widgets Qt6::Svg)

    install(TARGETS nixos-manager RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES share/applications/org.nixos.manager.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
    install(FILES share/icons/org.nixos.manager.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(NIXOS_MANAGER_BUILD_TESTS)
    enable_testing()
    add_executable(nixos-manager-parser-test tests/parser_test.cpp)
    set_target_properties(nixos-manager-parser-test PROPERTIES AUTOMOC ON)
    target_link_libraries(nixos-manager-parser-test PRIVATE nixos-manager-core)
    add_test(NAME parser COMMAND nixos-manager-parser-test)
endif()
