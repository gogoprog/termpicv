cmake_minimum_required(VERSION 3.16)
project(termpicv VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(TERMPICV_USE_MAGICK "Use ImageMagick (Magick++) for cached thumbnails" ON)

set(CURSES_NEED_WIDE TRUE)
find_package(Curses REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig)

add_executable(termpicv
    src/main.cpp
    src/app.cpp
    src/ueberzug.cpp
    src/thumbnailer.cpp
    src/animation.cpp
)

target_include_directories(termpicv PRIVATE ${CURSES_INCLUDE_DIRS})
target_link_libraries(termpicv PRIVATE ${CURSES_LIBRARIES} Threads::Threads)
target_compile_options(termpicv PRIVATE -Wall -Wextra)

if(TERMPICV_USE_MAGICK AND PkgConfig_FOUND)
    pkg_check_modules(MAGICKPP IMPORTED_TARGET Magick++)
    if(MAGICKPP_FOUND)
        message(STATUS "termpicv: Magick++ ${MAGICKPP_VERSION} found, thumbnail cache enabled")
        target_link_libraries(termpicv PRIVATE PkgConfig::MAGICKPP)
        target_compile_definitions(termpicv PRIVATE TERMPICV_HAVE_MAGICK=1)
    else()
        message(STATUS "termpicv: Magick++ not found, ueberzug will load full images")
    endif()
endif()

install(TARGETS termpicv RUNTIME DESTINATION bin)
