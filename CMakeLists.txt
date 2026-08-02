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
)
set_target_properties(nixos-manager-core PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)
target_include_directories(nixos-manager-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nixos-manager-core PUBLIC Qt6::Core Qt6::Network)
target_compile_definitions(nixos-manager-core PUBLIC
    NIXOS_MANAGER_VERSION="${PROJECT_VERSION}")

# ── Application ───────────────────────────────────────────────────────────────
if(NIXOS_MANAGER_BUILD_GUI)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
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
        src/ui/HostsPage.cpp
        src/ui/HostsPage.h
        src/ui/ModulesPage.cpp
        src/ui/ModulesPage.h
        src/ui/PackagesPage.cpp
        src/ui/PackagesPage.h
        src/ui/OptionsPage.cpp
        src/ui/OptionsPage.h
        src/ui/EditorPage.cpp
        src/ui/EditorPage.h
        src/ui/SystemPage.cpp
        src/ui/SystemPage.h
    )
    target_link_libraries(nixos-manager PRIVATE nixos-manager-core Qt6::Widgets)

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
