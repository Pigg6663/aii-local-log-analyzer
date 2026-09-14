cmake_minimum_required(VERSION 3.5)

project(aii LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# QtCreator supports the following variables for Android, which are identical to qmake Android variables.
# Check http://doc.qt.io/qt-5/deployment-android.html for more information.
# They need to be set before the find_package(Qt5 ...) call.

#if(ANDROID)
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
#    if (ANDROID_ABI STREQUAL "armeabi-v7a")
#        set(ANDROID_EXTRA_LIBS
#            ${CMAKE_CURRENT_SOURCE_DIR}/path/to/libcrypto.so
#            ${CMAKE_CURRENT_SOURCE_DIR}/path/to/libssl.so)
#    endif()
#endif()

find_package(Qt5 COMPONENTS Widgets Network REQUIRED)

# llama-server 集成（W1）：主程序和 server_test 共用，单独抽成变量
set(LLAMA_INTEGRATION_SOURCES
    llamaservermanager.cpp
    llamaservermanager.h
    llamahttpclient.cpp
    llamahttpclient.h
)

if(ANDROID)
  add_library(aii SHARED
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    analysisworker.cpp
    analysisworker.h
    datatypes.h
    logparser.cpp
    logparser.h
    logchunker.cpp
    logchunker.h
    promptbuilder.cpp
    promptbuilder.h
    resultparser.cpp
    resultparser.h
    settingsdialog.cpp
    settingsdialog.h
    ${LLAMA_INTEGRATION_SOURCES}
  )
else()
  add_executable(aii
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    analysisworker.cpp
    analysisworker.h
    datatypes.h
    logparser.cpp
    logparser.h
    logchunker.cpp
    logchunker.h
    promptbuilder.cpp
    promptbuilder.h
    resultparser.cpp
    resultparser.h
    settingsdialog.cpp
    settingsdialog.h
    ${LLAMA_INTEGRATION_SOURCES}
  )
endif()

target_link_libraries(aii PRIVATE Qt5::Widgets Qt5::Network)

# W1 控制台测试程序：启动 llama-server → /health 就绪 → 一次 chat 闭环。
# 只依赖 QtCore + QtNetwork，不依赖 Widgets。
add_executable(server_test
    server_test.cpp
    ${LLAMA_INTEGRATION_SOURCES}
)
target_link_libraries(server_test PRIVATE Qt5::Core Qt5::Network)
