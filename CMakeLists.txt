cmake_minimum_required(VERSION 3.30)

project(Scheduler_sim)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(schedulerSim)
target_sources(schedulerSim
    PRIVATE
      src/main.cpp
  )
