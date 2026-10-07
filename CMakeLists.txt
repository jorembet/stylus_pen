cmake_minimum_required(VERSION 3.20)
project(stylus-pen VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/data DESTINATION ${CMAKE_BINARY_DIR})


find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)

# ---- core drawing library (no GTK dependency, unit-testable) ----
add_library(stylus_core STATIC
  src/color.cpp
  src/paper.cpp
  src/geometry.cpp
  src/brush.cpp
  src/document.cpp
  src/history.cpp
  src/platform.cpp
  src/project.cpp
  src/ocr.cpp
)
target_include_directories(stylus_core PUBLIC src)
# dl is part of libc on Windows and dlopen is absent, so platform.cpp uses the Win32
# loader there instead. Linking it unconditionally is harmless but noisy on MSVC.
target_link_libraries(stylus_core PUBLIC PkgConfig::CAIRO)
if(NOT WIN32)
  target_link_libraries(stylus_core PUBLIC ${CMAKE_DL_LIBS})
endif()

# ---- GTK application ----
add_executable(stylus-pen
  src/main.cpp
  src/window.cpp
  src/canvas.cpp
)
target_link_libraries(stylus-pen PRIVATE stylus_core PkgConfig::GTK4)

# ---- tests ----
enable_testing()

add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE stylus_core)
add_test(NAME core COMMAND test_core)

add_executable(test_render tests/test_render.cpp)
target_link_libraries(test_render PRIVATE stylus_core)
add_test(NAME render COMMAND test_render)

# Drives the canvas through synthetic GDK events. It is the only target that links
# the GTK layer, which is exactly the gap that let a signal handler which silently did
# nothing survive a green suite.
add_executable(test_canvas_events tests/test_canvas_events.cpp src/canvas.cpp)
target_include_directories(test_canvas_events PRIVATE src)
target_link_libraries(test_canvas_events PRIVATE stylus_core PkgConfig::GTK4)
add_test(NAME canvas_events COMMAND test_canvas_events)

add_executable(test_window_teardown tests/test_window_teardown.cpp src/window.cpp src/canvas.cpp)
target_include_directories(test_window_teardown PRIVATE src)
target_link_libraries(test_window_teardown PRIVATE stylus_core PkgConfig::GTK4)
add_test(NAME window_teardown COMMAND test_window_teardown)
set_tests_properties(canvas_events window_teardown PROPERTIES ENVIRONMENT "GDK_BACKEND=x11")

install(TARGETS stylus-pen RUNTIME DESTINATION bin)
install(FILES data/com.local.styluspen.desktop DESTINATION share/applications)
install(FILES data/stylus-pen.svg DESTINATION share/icons/hicolor/scalable/apps)


