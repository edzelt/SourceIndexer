# === CMakeLists.txt ===
cmake_minimum_required(VERSION 3.16)

project(SourcebreakerIndexer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

list(APPEND CMAKE_PREFIX_PATH
    "C:/Qt/6.10.2/msvc2022_64"
    "C:/Program Files/LLVM"
)

find_package(Qt6 REQUIRED COMPONENTS Widgets Sql)

find_path(CLANG_INCLUDE_DIR clang-c/Index.h
    PATHS "C:/Program Files/LLVM/include")
find_library(CLANG_LIBRARY NAMES libclang
    PATHS "C:/Program Files/LLVM/lib")

add_executable(SourcebreakerIndexer
    main.cpp
    ProjectDetector.h
    ProjectDetector.cpp
    Storage.h
    Storage.cpp
    CodeIndexer.h
    CodeIndexer.cpp
    IndexerConfig.h
    IndexerConfig.cpp
    IndexerConfigDialog.h
    IndexerConfigDialog.cpp
    IndexerWorker.h
    IndexerWorker.cpp
    IndexProgressDialog.h
    IndexProgressDialog.cpp
)

target_include_directories(SourcebreakerIndexer PRIVATE ${CLANG_INCLUDE_DIR})

target_link_libraries(SourcebreakerIndexer PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Sql
    ${CLANG_LIBRARY}
)

qt_finalize_executable(SourcebreakerIndexer)