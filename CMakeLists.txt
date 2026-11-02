cmake_minimum_required(VERSION 3.22)
set(IDEA4GIT_VERSION "1.0.0" CACHE STRING "idea4git application and package version")
project(idea4git VERSION "${IDEA4GIT_VERSION}" LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Concurrent)
find_path(QSCINTILLA_INCLUDE_DIR NAMES Qsci/qsciscintilla.h
    HINTS ${Qt6Widgets_INCLUDE_DIRS})
find_library(QSCINTILLA_LIBRARY NAMES qscintilla2_qt6 qscintilla2-qt6)
if(NOT QSCINTILLA_INCLUDE_DIR OR NOT QSCINTILLA_LIBRARY)
    message(FATAL_ERROR "Qt 6 QScintilla is required. On Ubuntu: apt install libqscintilla2-qt6-dev")
endif()
add_library(QScintilla::QScintilla UNKNOWN IMPORTED)
set_target_properties(QScintilla::QScintilla PROPERTIES
    IMPORTED_LOCATION "${QSCINTILLA_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${QSCINTILLA_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES Qt6::Widgets)

add_library(idea4git_core STATIC
    src/gitclient.cpp src/gitclient.h
    src/gitstatus.cpp src/gitstatus.h
    src/gitoperations.cpp src/gitoperations.h
    src/gitdialogs.cpp src/gitdialogs.h
    src/idea4git.qrc
    src/codeeditor.cpp src/codeeditor.h
    src/projectmodel.cpp src/projectmodel.h
    src/icons.cpp src/icons.h
    src/paths.h
    src/builtintranslator.h
    src/mainwindow.cpp src/mainwindow_git.cpp src/mainwindow.h)
target_include_directories(idea4git_core PUBLIC src)
target_link_libraries(idea4git_core PUBLIC Qt6::Widgets Qt6::Concurrent QScintilla::QScintilla)
if(MSVC)
    target_compile_options(idea4git_core PRIVATE /W4 /utf-8)
else()
    target_compile_options(idea4git_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(idea4git src/main.cpp)
target_link_libraries(idea4git PRIVATE idea4git_core)
target_compile_definitions(idea4git PRIVATE IDEA4GIT_VERSION=\"${PROJECT_VERSION}\")
if(APPLE)
    set_source_files_properties(packaging/macos/idea4git.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
    target_sources(idea4git PRIVATE packaging/macos/idea4git.icns)
    set_target_properties(idea4git PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.idea4git.app"
        MACOSX_BUNDLE_BUNDLE_NAME "idea4git"
        MACOSX_BUNDLE_ICON_FILE "idea4git.icns"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
elseif(WIN32)
    enable_language(RC)
    configure_file(packaging/windows/idea4git.rc.in
        "${CMAKE_CURRENT_BINARY_DIR}/idea4git.rc" @ONLY)
    target_sources(idea4git PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/idea4git.rc")
    set_target_properties(idea4git PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
install(TARGETS idea4git
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .)
if(UNIX AND NOT APPLE)
    install(FILES packaging/idea4git.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
    install(FILES packaging/icons/idea4git.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps)
endif()
install(FILES README.md docs/advanced-git.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
foreach(license_file IN ITEMS LICENSE LICENSE.txt COPYING)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${license_file}")
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${license_file}"
            DESTINATION ${CMAKE_INSTALL_DOCDIR})
    endif()
endforeach()

set(CPACK_PACKAGE_NAME "idea4git")
set(CPACK_PACKAGE_VENDOR "idea4git")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lightweight Qt desktop Git client")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "idea4git local package <local@localhost>")
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
elseif(APPLE)
    set(CPACK_GENERATOR "TGZ")
else()
    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "idea4git local package <local@localhost>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "git (>= 2.38)")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
endif()
include(CPack)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(idea4git_git_tests tests/git_tests.cpp)
    target_link_libraries(idea4git_git_tests PRIVATE idea4git_core Qt6::Test)
    add_test(NAME git-workflows COMMAND idea4git_git_tests)
    add_executable(idea4git_ui_tests tests/ui_tests.cpp)
    target_link_libraries(idea4git_ui_tests PRIVATE idea4git_core Qt6::Test)
    add_test(NAME editor-and-ui COMMAND idea4git_ui_tests)
    add_executable(idea4git_advanced_tests tests/advanced_git_tests.cpp)
    target_link_libraries(idea4git_advanced_tests PRIVATE idea4git_core Qt6::Test)
    add_test(NAME advanced-git-workflows COMMAND idea4git_advanced_tests)
    set_tests_properties(advanced-git-workflows PROPERTIES TIMEOUT 120)
    add_executable(idea4git_advanced_ui_tests tests/advanced_ui_tests.cpp)
    target_link_libraries(idea4git_advanced_ui_tests PRIVATE idea4git_core Qt6::Test)
    add_test(NAME advanced-git-ui COMMAND idea4git_advanced_ui_tests)
    set_tests_properties(advanced-git-ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
    add_executable(idea4git_demo_preview tests/demo_preview.cpp)
    target_link_libraries(idea4git_demo_preview PRIVATE idea4git_core)
    set_tests_properties(git-workflows editor-and-ui PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endif()
