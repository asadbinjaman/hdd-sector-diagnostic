cmake_minimum_required(VERSION 3.20)
project(HddSectorDiagnostic LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "This project requires Windows.")
endif()

add_executable(hdddiag src/main.cpp)

target_compile_features(hdddiag PRIVATE cxx_std_17)
target_compile_definitions(hdddiag PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(hdddiag PRIVATE setupapi)

if(MSVC)
  target_compile_options(hdddiag PRIVATE /W4 /permissive- /EHsc)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # This tells MinGW to handle Windows wide-character entry points (wmain)
  target_compile_options(hdddiag PRIVATE -municode)
  target_link_options(hdddiag PRIVATE -municode -mconsole)
endif()
