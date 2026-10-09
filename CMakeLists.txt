cmake_minimum_required(VERSION 3.20)

project(CogniteCIFF LANGUAGES CXX)

find_package(Threads REQUIRED)

option(FALCON_BUILD_CLI "Build the command-line converter" ON)

add_library(CogniteCiffLib STATIC
    CogniteCiffLib/ConvertToFile.cpp
    CogniteCiffLib/ConvertToScene.cpp
    PrimitivesCIFF.cpp
)

target_compile_features(CogniteCiffLib PUBLIC cxx_std_20)
target_include_directories(CogniteCiffLib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/CogniteCiffLib"
)
target_link_libraries(CogniteCiffLib PUBLIC Threads::Threads)
set_target_properties(CogniteCiffLib PROPERTIES CXX_EXTENSIONS OFF)

if(FALCON_BUILD_CLI)
    add_executable(CogniteCIFF
        main.cpp
        PrimitivesCIFF.cpp
    )

    target_compile_features(CogniteCIFF PRIVATE cxx_std_20)
    target_include_directories(CogniteCIFF PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(CogniteCIFF PRIVATE Threads::Threads)
    set_target_properties(CogniteCIFF PROPERTIES CXX_EXTENSIONS OFF)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(CiffDwgExport tests/CiffDwgExport.cpp)
    target_link_libraries(CiffDwgExport PRIVATE CogniteCiffLib)
    add_test(NAME CiffDwgExport COMMAND CiffDwgExport "${CMAKE_CURRENT_BINARY_DIR}/ciff-dwg-regression")
    if(FALCON_BUILD_CLI)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME CiffDwgCli COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/CiffDwgCli.py" $<TARGET_FILE:CogniteCIFF>
                "${CMAKE_CURRENT_BINARY_DIR}/ciff-dwg-cli-regression")
        endif()
    endif()
endif()
