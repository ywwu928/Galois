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
include lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/depend.make
# Include any dependencies generated by the compiler for this target.
include lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/compiler_depend.make

# Include the progress variables for this target.
include lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/progress.make

# Include the compile flags for this target's objects.
include lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/flags.make

lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/TorusConstruction.cpp.o: lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/flags.make
lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/TorusConstruction.cpp.o: lonestar/tutorial_examples/TorusConstruction.cpp
lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/TorusConstruction.cpp.o: lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/work/08474/ywwu/ls6/Galois/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building CXX object lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/TorusConstruction.cpp.o"
	cd /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/TorusConstruction.cpp.o -MF CMakeFiles/example-torus-construction.dir/TorusConstruction.cpp.o.d -o CMakeFiles/example-torus-construction.dir/TorusConstruction.cpp.o -c /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples/TorusConstruction.cpp

lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/TorusConstruction.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/example-torus-construction.dir/TorusConstruction.cpp.i"
	cd /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples/TorusConstruction.cpp > CMakeFiles/example-torus-construction.dir/TorusConstruction.cpp.i

lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/TorusConstruction.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/example-torus-construction.dir/TorusConstruction.cpp.s"
	cd /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples/TorusConstruction.cpp -o CMakeFiles/example-torus-construction.dir/TorusConstruction.cpp.s

# Object files for target example-torus-construction
example__torus__construction_OBJECTS = \
"CMakeFiles/example-torus-construction.dir/TorusConstruction.cpp.o"

# External object files for target example-torus-construction
example__torus__construction_EXTERNAL_OBJECTS =

lonestar/tutorial_examples/example-torus-construction: lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/TorusConstruction.cpp.o
lonestar/tutorial_examples/example-torus-construction: lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/build.make
lonestar/tutorial_examples/example-torus-construction: libgalois/libgalois_shmem.a
lonestar/tutorial_examples/example-torus-construction: lonestar/liblonestar/liblonestar.a
lonestar/tutorial_examples/example-torus-construction: libgalois/libgalois_shmem.a
lonestar/tutorial_examples/example-torus-construction: /usr/lib64/libnuma.so
lonestar/tutorial_examples/example-torus-construction: /work/08474/ywwu/ls6/source/llvm-project/build/lib/libLLVMSupport.a
lonestar/tutorial_examples/example-torus-construction: /usr/lib64/libz.so
lonestar/tutorial_examples/example-torus-construction: /usr/lib64/libzstd.so
lonestar/tutorial_examples/example-torus-construction: /usr/lib64/libtinfo.so
lonestar/tutorial_examples/example-torus-construction: /work/08474/ywwu/ls6/source/llvm-project/build/lib/libLLVMDemangle.a
lonestar/tutorial_examples/example-torus-construction: lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/link.txt
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --bold --progress-dir=/work/08474/ywwu/ls6/Galois/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "Linking CXX executable example-torus-construction"
	cd /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples && $(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/example-torus-construction.dir/link.txt --verbose=$(VERBOSE)

# Rule to build all files generated by this target.
lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/build: lonestar/tutorial_examples/example-torus-construction
.PHONY : lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/build

lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/clean:
	cd /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples && $(CMAKE_COMMAND) -P CMakeFiles/example-torus-construction.dir/cmake_clean.cmake
.PHONY : lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/clean

lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/depend:
	cd /work/08474/ywwu/ls6/Galois && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /work/08474/ywwu/ls6/Galois /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples /work/08474/ywwu/ls6/Galois /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : lonestar/tutorial_examples/CMakeFiles/example-torus-construction.dir/depend
