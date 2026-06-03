cmake_minimum_required(VERSION 3.19)
project(VolumeTool LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets)

qt_standard_project_setup()

qt_add_executable(VolumeTool
    WIN32 MACOSX_BUNDLE
    audiodevicemanager.cpp
    audiodevicemanager.h
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    voicemeeterremoteclient.cpp
    voicemeeterremoteclient.h
)

target_link_libraries(VolumeTool
    PRIVATE
        Qt::Core
        Qt::Widgets
)

install(TARGETS VolumeTool
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET VolumeTool
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
