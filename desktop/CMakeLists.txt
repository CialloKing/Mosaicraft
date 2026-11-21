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

include(CTest)
if(BUILD_TESTING)
    add_test(NAME desktop_smoke COMMAND MosaicraftDesktop --smoke-test -platform offscreen)
endif()
