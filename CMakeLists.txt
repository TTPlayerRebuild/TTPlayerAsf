cmake_minimum_required(VERSION 3.24)
project(TtpAsf LANGUAGES C CXX RC)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
 message(FATAL_ERROR "ttp_asf requires MSVC and -A Win32")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(cmake/legacy_windows.cmake)
include(cmake/size_release.cmake)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include(cmake/version.cmake)
add_library(ttp_asf SHARED src/plugin.cpp src/reader.cpp src/decoder.cpp src/ttp_asf.def
 "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
add_dependencies(ttp_asf ttp_asf_version)
target_include_directories(ttp_asf PUBLIC include PRIVATE src)
target_compile_definitions(ttp_asf PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0501 _CRT_SECURE_NO_WARNINGS)
target_compile_options(ttp_asf PRIVATE /W4 /permissive- /EHsc /utf-8 /fp:precise /arch:SSE2)
target_link_libraries(ttp_asf PRIVATE ole32 uuid shlwapi strmiids wmcodecdspuuid)
set_target_properties(ttp_asf PROPERTIES PREFIX "" OUTPUT_NAME "ttp_asf")
add_custom_command(TARGET ttp_asf POST_BUILD COMMAND "${Python3_EXECUTABLE}"
 "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_legacy_imports.py" "$<TARGET_FILE:ttp_asf>"
 --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/5.1.2600.txt"
 --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/6.1.7600.txt"
 --report "$<TARGET_FILE_DIR:ttp_asf>/legacy-imports.json" VERBATIM)
install(TARGETS ttp_asf RUNTIME DESTINATION AddIn COMPONENT Runtime)
