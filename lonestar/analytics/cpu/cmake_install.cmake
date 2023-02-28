# Install script for directory: /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "0")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("/work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/betweennesscentrality/cmake_install.cmake")
  include("/work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/bfs/cmake_install.cmake")
  include("/work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/bipart/cmake_install.cmake")
  include("/work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/spanningtree/cmake_install.cmake")
  include("/work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/clustering/cmake_install.cmake")
  include("/work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/connected-components/cmake_install.cmake")
  include("/work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis/cmake_install.cmake")
  include("/work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/independentset/cmake_install.cmake")
  include("/work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/k-core/cmake_install.cmake")
  include("/work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/k-truss/cmake_install.cmake")
  include("/work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/matching/cmake_install.cmake")
  include("/work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/matrixcompletion/cmake_install.cmake")
  include("/work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/pagerank/cmake_install.cmake")
  include("/work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/pointstoanalysis/cmake_install.cmake")
  include("/work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/preflowpush/cmake_install.cmake")
  include("/work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/sssp/cmake_install.cmake")
  include("/work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/triangle-counting/cmake_install.cmake")

endif()

