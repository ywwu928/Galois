# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.24

# Delete rule output on recipe failure.
.DELETE_ON_ERROR:

#=============================================================================
# Special targets provided by cmake.

# Disable implicit rules so canonical targets will work.
.SUFFIXES:

# Disable VCS-based implicit rules.
% : %,v

# Disable VCS-based implicit rules.
% : RCS/%

# Disable VCS-based implicit rules.
% : RCS/%,v

# Disable VCS-based implicit rules.
% : SCCS/s.%

# Disable VCS-based implicit rules.
% : s.%

.SUFFIXES: .hpux_make_needs_suffix_list

# Command-line flag to silence nested $(MAKE).
$(VERBOSE)MAKESILENT = -s

#Suppress display of executed commands.
$(VERBOSE).SILENT:

# A target that is always out of date.
cmake_force:
.PHONY : cmake_force

#=============================================================================
# Set environment variables for the build.

# The shell in which to execute make rules.
SHELL = /bin/sh

# The CMake executable.
CMAKE_COMMAND = /opt/apps/cmake/3.24.2/bin/cmake

# The command to remove a file.
RM = /opt/apps/cmake/3.24.2/bin/cmake -E rm -f

# Escaping for special characters.
EQUALS = =

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = /work/08474/ywwu/ls6/Galois

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /work/08474/ywwu/ls6/Galois

# Include any dependencies generated for this target.
include libgalois/test/CMakeFiles/unit-mem.dir/depend.make
# Include any dependencies generated by the compiler for this target.
include libgalois/test/CMakeFiles/unit-mem.dir/compiler_depend.make

# Include the progress variables for this target.
include libgalois/test/CMakeFiles/unit-mem.dir/progress.make

# Include the compile flags for this target's objects.
include libgalois/test/CMakeFiles/unit-mem.dir/flags.make

libgalois/test/CMakeFiles/unit-mem.dir/mem.cpp.o: libgalois/test/CMakeFiles/unit-mem.dir/flags.make
libgalois/test/CMakeFiles/unit-mem.dir/mem.cpp.o: libgalois/test/mem.cpp
libgalois/test/CMakeFiles/unit-mem.dir/mem.cpp.o: libgalois/test/CMakeFiles/unit-mem.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/work/08474/ywwu/ls6/Galois/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building CXX object libgalois/test/CMakeFiles/unit-mem.dir/mem.cpp.o"
	cd /work/08474/ywwu/ls6/Galois/libgalois/test && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT libgalois/test/CMakeFiles/unit-mem.dir/mem.cpp.o -MF CMakeFiles/unit-mem.dir/mem.cpp.o.d -o CMakeFiles/unit-mem.dir/mem.cpp.o -c /work/08474/ywwu/ls6/Galois/libgalois/test/mem.cpp

libgalois/test/CMakeFiles/unit-mem.dir/mem.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/unit-mem.dir/mem.cpp.i"
	cd /work/08474/ywwu/ls6/Galois/libgalois/test && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /work/08474/ywwu/ls6/Galois/libgalois/test/mem.cpp > CMakeFiles/unit-mem.dir/mem.cpp.i

libgalois/test/CMakeFiles/unit-mem.dir/mem.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/unit-mem.dir/mem.cpp.s"
	cd /work/08474/ywwu/ls6/Galois/libgalois/test && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /work/08474/ywwu/ls6/Galois/libgalois/test/mem.cpp -o CMakeFiles/unit-mem.dir/mem.cpp.s

# Object files for target unit-mem
unit__mem_OBJECTS = \
"CMakeFiles/unit-mem.dir/mem.cpp.o"

# External object files for target unit-mem
unit__mem_EXTERNAL_OBJECTS =

libgalois/test/unit-mem: libgalois/test/CMakeFiles/unit-mem.dir/mem.cpp.o
libgalois/test/unit-mem: libgalois/test/CMakeFiles/unit-mem.dir/build.make
libgalois/test/unit-mem: libgalois/libgalois_shmem.a
libgalois/test/unit-mem: lonestar/liblonestar/liblonestar.a
libgalois/test/unit-mem: libgalois/libgalois_shmem.a
libgalois/test/unit-mem: /usr/lib64/libnuma.so
libgalois/test/unit-mem: /work/08474/ywwu/ls6/source/llvm-project/build/lib/libLLVMSupport.a
libgalois/test/unit-mem: /usr/lib64/libz.so
libgalois/test/unit-mem: /usr/lib64/libzstd.so
libgalois/test/unit-mem: /usr/lib64/libtinfo.so
libgalois/test/unit-mem: /work/08474/ywwu/ls6/source/llvm-project/build/lib/libLLVMDemangle.a
libgalois/test/unit-mem: libgalois/test/CMakeFiles/unit-mem.dir/link.txt
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --bold --progress-dir=/work/08474/ywwu/ls6/Galois/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "Linking CXX executable unit-mem"
	cd /work/08474/ywwu/ls6/Galois/libgalois/test && $(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/unit-mem.dir/link.txt --verbose=$(VERBOSE)

# Rule to build all files generated by this target.
libgalois/test/CMakeFiles/unit-mem.dir/build: libgalois/test/unit-mem
.PHONY : libgalois/test/CMakeFiles/unit-mem.dir/build

libgalois/test/CMakeFiles/unit-mem.dir/clean:
	cd /work/08474/ywwu/ls6/Galois/libgalois/test && $(CMAKE_COMMAND) -P CMakeFiles/unit-mem.dir/cmake_clean.cmake
.PHONY : libgalois/test/CMakeFiles/unit-mem.dir/clean

libgalois/test/CMakeFiles/unit-mem.dir/depend:
	cd /work/08474/ywwu/ls6/Galois && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /work/08474/ywwu/ls6/Galois /work/08474/ywwu/ls6/Galois/libgalois/test /work/08474/ywwu/ls6/Galois /work/08474/ywwu/ls6/Galois/libgalois/test /work/08474/ywwu/ls6/Galois/libgalois/test/CMakeFiles/unit-mem.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : libgalois/test/CMakeFiles/unit-mem.dir/depend
