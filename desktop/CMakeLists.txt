cmake_minimum_required(VERSION 3.21)
project(MosaicraftDesktop LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Sql Network LinguistTools)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/ui")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(contracts "${CMAKE_CURRENT_SOURCE_DIR}/../contracts.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${contracts}")
file(READ "${contracts}" contract_json)
string(JSON mosaicraft_version GET "${contract_json}" softwareVersion)
string(JSON library_version GET "${contract_json}" libraryVersion)

qt_add_executable(MosaicraftDesktop WIN32 MACOSX_BUNDLE
    CanvasImageItem.cpp
    CanvasImageItem.h
    DimensionEdit.h
    main.cpp
    MainWindow.cpp
    MainWindowEditor.cpp
    MainWindow.h
    MainWindowJobs.cpp
    MainWindowLibrary.cpp
    ../tests/test_desktop.cpp
    Dialogs.cpp
    Dialogs.h
    LibraryCatalog.cpp
    LibraryCatalog.h
    ui/MainWindow.ui
    ui/SettingsDialog.ui
    ui/LibrarySettingsDialog.ui
)
target_link_libraries(MosaicraftDesktop PRIVATE Qt6::Widgets Qt6::Sql Qt6::Network)
target_include_directories(MosaicraftDesktop PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(MosaicraftDesktop PRIVATE
    MOSAICRAFT_DESKTOP_VERSION="${mosaicraft_version}"
    MOSAICRAFT_LIBRARY_VERSION=${library_version}
)
if(MSVC)
    target_compile_options(MosaicraftDesktop PRIVATE /utf-8)
endif()
qt_add_translations(MosaicraftDesktop
    TS_FILES i18n/mosaicraft_en.ts
    RESOURCE_PREFIX "/i18n"
)
# Keep the Qt dialog translations available when the executable is deployed without the SDK.
if(TARGET Qt6::qmake)
    get_target_property(qt_qmake Qt6::qmake LOCATION)
    if(qt_qmake)
        execute_process(COMMAND "${qt_qmake}" -query QT_INSTALL_TRANSLATIONS
            OUTPUT_VARIABLE qt_translations_dir
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(qt_chinese_catalog "${qt_translations_dir}/qtbase_zh_CN.qm")
        if(EXISTS "${qt_chinese_catalog}")
            qt_add_resources(MosaicraftDesktop qt_dialog_translations
                PREFIX "/i18n"
                BASE "${qt_translations_dir}"
                FILES "${qt_chinese_catalog}"
            )
        endif()
    endif()
endif()

option(MOSAICRAFT_DEPLOY_QT "Install the GUI with a relocatable Qt runtime" OFF)
if(MOSAICRAFT_DEPLOY_QT)
    if(DEFINED MOSAICRAFT_QT_RELEASE_VERSION AND NOT Qt6_VERSION VERSION_EQUAL MOSAICRAFT_QT_RELEASE_VERSION)
        message(FATAL_ERROR "Desktop release requires Qt ${MOSAICRAFT_QT_RELEASE_VERSION}, found ${Qt6_VERSION}")
    endif()
    if(Qt6_VERSION VERSION_LESS 6.10)
        message(FATAL_ERROR "Desktop redistribution needs Qt 6.10+ for plugin selection")
    endif()
    include(GNUInstallDirs)
    install(TARGETS MosaicraftDesktop BUNDLE DESTINATION . RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    set(deploy_options)
    if(NOT APPLE)
        # The gallery needs SQLite and the image picker supports WebP and TIFF.
        set(included_plugins qsqlite qwebp qtiff)
        set(excluded_plugins qsqlmysql qsqlodbc qsqlpsql qsqlibase qsqlmimer qsqloci)
        if(UNIX)
            # Qt's Linux SDK defaults to XCB deployment even when it includes Wayland.
            list(APPEND included_plugins qxcb qwayland)
            # Desktop users need X11/Wayland, not device-specific framebuffer backends.
            list(APPEND excluded_plugins qeglfs qlinuxfb qminimalegl qvkkhrdisplay qvnc qgtk3)
        endif()
        list(APPEND deploy_options INCLUDE_PLUGINS ${included_plugins} EXCLUDE_PLUGINS ${excluded_plugins})
    endif()
    if(WIN32)
        # The native backend installer already puts the MSVC runtime beside the GUI.
        list(APPEND deploy_options NO_COMPILER_RUNTIME)
    endif()
    qt_generate_deploy_app_script(TARGET MosaicraftDesktop OUTPUT_SCRIPT deploy_script ${deploy_options})
    install(SCRIPT "${deploy_script}")
endif()

if(TARGET mosaicraft AND TARGET mosaicraft_desktop_worker)
    add_dependencies(MosaicraftDesktop mosaicraft mosaicraft_desktop_worker)
    if(APPLE)
        # Stage on every requested build, even when only the backend changed.
        set(backend_dir "$<TARGET_BUNDLE_CONTENT_DIR:MosaicraftDesktop>/MacOS")
        add_custom_target(mosaicraft_desktop_backend
            COMMAND ${CMAKE_COMMAND} -E make_directory "${backend_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:mosaicraft>" "$<TARGET_FILE:mosaicraft_desktop_worker>" "${backend_dir}"
            VERBATIM
        )
        add_dependencies(MosaicraftDesktop mosaicraft_desktop_backend)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME desktop_smoke COMMAND MosaicraftDesktop --smoke-test -platform offscreen)
    if(TARGET mosaicraft AND TARGET mosaicraft_desktop_worker)
        set_tests_properties(desktop_smoke PROPERTIES
            ENVIRONMENT "MOSAICRAFT_DESKTOP_SMOKE_CLI=$<TARGET_FILE_DIR:MosaicraftDesktop>/$<TARGET_FILE_NAME:mosaicraft>")
    endif()
endif()
