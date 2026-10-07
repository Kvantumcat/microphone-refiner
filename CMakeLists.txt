cmake_minimum_required(VERSION 3.20)
project(microphone-refiner CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W3 /permissive- /Zc:__cplusplus)
    add_compile_definitions(
        _WIN32_WINNT=0x0A00
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE _UNICODE
        _CRT_SECURE_NO_WARNINGS
    )
    # Release build: full optimizations
    string(REPLACE "/O2" "/O2 /GL" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
endif()

add_executable(microphone-refiner
    src/main.cpp
    src/AudioEngine.cpp
    src/SpectralSubtractionFilter.cpp
    src/IniConfig.cpp
)

target_include_directories(microphone-refiner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# WASAPI / MMDevice / COM come from these system libs on modern Windows
target_link_libraries(microphone-refiner PRIVATE ole32 uuid avrt)
