# SDD-016: the CpapDash supervisor.
#
# One Qt Widgets application replacing the C# WinForms tray, and providing the
# shell macOS and Linux never got at all. It OWNS the hms_cpap process: the OS
# starts the supervisor at login and knows nothing else.
#
# Guarded by BUILD_SUPERVISOR (default OFF) in the top-level CMakeLists, so a
# machine without Qt builds the service exactly as before.

cmake_minimum_required(VERSION 3.16)

# Configurable two ways, from the SAME file:
#
#   - as part of hms-cpap, via -DBUILD_SUPERVISOR=ON at the top level. This is
#     what CI does, so the supervisor ships from the same build and the same
#     release as the service it supervises.
#
#   - on its own, by pointing cmake straight at desktop/qt. The supervisor needs
#     Qt and nlohmann and nothing else -- not Drogon, not libpqxx, not the MQTT
#     stack -- so a developer changing a dialog should not wait for the service's
#     entire dependency graph to configure. On Windows that is the difference
#     between a minute and the better part of an hour.
#
# HMS_CPAP_ROOT is how the standalone build finds the two things it borrows: the
# include tree and CardLayout.cpp.
if(NOT DEFINED PROJECT_NAME)
    project(cpapdash_supervisor CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    get_filename_component(HMS_CPAP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    if(NOT DEFINED HMS_CPAP_VERSION)
        file(READ "${HMS_CPAP_ROOT}/VERSION" HMS_CPAP_VERSION)
        string(STRIP "${HMS_CPAP_VERSION}" HMS_CPAP_VERSION)
    endif()
else()
    set(HMS_CPAP_ROOT "${CMAKE_SOURCE_DIR}")
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network)

qt_standard_project_setup()

# The executable keeps the name the C# shell had. That is not nostalgia: the
# installer's [Files]/[Icons]/[Registry]/[Run]/[UninstallRun] sections, the Run
# key value, six assertions in the windows-desktop-test job, and the Run key on
# every machine that already has this installed all name CpapDashDesktop.exe.
# Renaming it for tidiness would break an upgrade for existing users and buy
# nothing they can see.
set(SUPERVISOR_TARGET CpapDashDesktop)

qt_add_executable(${SUPERVISOR_TARGET}
    WIN32 MACOSX_BUNDLE
    main.cpp

    core/pure/PreflightReport.cpp
    core/pure/ConfigModel.cpp
    core/pure/FieldSpec.cpp
    core/pure/ConfigGate.cpp

    core/qt/PreflightRunner.cpp
    core/qt/ChildProcess.cpp

    app/SingleInstance.cpp
    app/Autostart.cpp

    ui/TrayShell.cpp
    ui/SettingsDialog.cpp
    ui/ConfiguratorWindow.cpp
    ui/StartupWindow.cpp
)

target_include_directories(${SUPERVISOR_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core/pure
    ${CMAKE_CURRENT_SOURCE_DIR}/core/qt
    ${CMAKE_CURRENT_SOURCE_DIR}/app
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
)

# Exactly ONE file from the service is compiled in, and it is deliberate:
# CardLayout is pure (std::filesystem and nothing else) and is the only thing
# that can tell a directory picker whether the folder the user chose is a card
# root. There is no HTTP endpoint for that question.
#
# The rule this list encodes: the supervisor gets what it needs by RUNNING
# hms_cpap or by talking HTTP to it. The moment a second .cpp appears here, the
# GUI has started absorbing the service and can begin to disagree with it.
target_sources(${SUPERVISOR_TARGET} PRIVATE
    ${HMS_CPAP_ROOT}/src/utils/CardLayout.cpp
)
target_include_directories(${SUPERVISOR_TARGET} PRIVATE ${HMS_CPAP_ROOT}/include)

# nlohmann/json is header-only and is the only non-Qt dependency. In the
# in-project build it comes from the service's own dependency graph; standalone,
# it may not be installed at all -- and requiring a developer to set up vcpkg to
# edit a dialog defeats the point of the standalone path. Fetch the single
# header instead when it is absent.
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(nlohmann_json)
endif()

target_link_libraries(${SUPERVISOR_TARGET} PRIVATE
    Qt6::Widgets
    Qt6::Network
    nlohmann_json::nlohmann_json
)

# Compiled-in icons: a desktop app that loads its icon off disk shows a blank
# square the moment someone moves the folder.
qt_add_resources(${SUPERVISOR_TARGET} "icons"
    PREFIX "/icons"
    BASE   "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES
        # Colour family: Windows and Linux tray areas.
        resources/tray-running.png
        resources/tray-running@2x.png
        resources/tray-starting.png
        resources/tray-starting@2x.png
        resources/tray-stopped.png
        resources/tray-stopped@2x.png
        resources/tray-problem.png
        resources/tray-problem@2x.png

        # Template family: the macOS menu bar tints these itself. Both families
        # ship on every platform -- they are a few KB, and leaving one out is a
        # blank status icon on exactly one OS, which is precisely the kind of
        # thing that gets discovered by a user rather than a build.
        resources/mono-running.png
        resources/mono-running@2x.png
        resources/mono-starting.png
        resources/mono-starting@2x.png
        resources/mono-stopped.png
        resources/mono-stopped@2x.png
        resources/mono-problem.png
        resources/mono-problem@2x.png

        resources/app-256.png

        # Scrollbar arrows. Qt cannot draw these from a stylesheet, so they are
        # assets like everything else.
        resources/arrow-up.png
        resources/arrow-up@2x.png
        resources/arrow-down.png
        resources/arrow-down@2x.png
)

if(APPLE)
    # The icon has to be COPIED INTO the bundle, not merely named in the plist.
    # Info.plist declares CFBundleIconFile, but a bundle with no Resources
    # directory just shows the generic application icon, which is what shipped
    # until this was noticed.
    set(SUPERVISOR_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/resources/cpapdash.icns")
    target_sources(${SUPERVISOR_TARGET} PRIVATE "${SUPERVISOR_ICNS}")
    set_source_files_properties("${SUPERVISOR_ICNS}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")

    # LSUIElement: a menu bar app with no Dock icon and no menu bar of its own.
    # Without it the supervisor shows up in the Dock and in Cmd-Tab like a
    # document app, which is wrong for something whose whole job is to sit in
    # the status area.
    set_target_properties(${SUPERVISOR_TARGET} PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME        "CpapDash"
        MACOSX_BUNDLE_GUI_IDENTIFIER     "com.hms.cpapdash.supervisor"
        MACOSX_BUNDLE_BUNDLE_VERSION     "${HMS_CPAP_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${HMS_CPAP_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in"
    )
endif()

if(WIN32)
    # Gives the exe its icon in Explorer, the taskbar, and Add/Remove Programs
    # (installer.iss points UninstallDisplayIcon at the exe itself).
    configure_file(resources/supervisor.rc.in
                   "${CMAKE_CURRENT_BINARY_DIR}/supervisor.rc" @ONLY)
    target_sources(${SUPERVISOR_TARGET} PRIVATE
                   "${CMAKE_CURRENT_BINARY_DIR}/supervisor.rc")
endif()

message(STATUS "  Supervisor:      ${SUPERVISOR_TARGET} (Qt ${Qt6_VERSION})")
