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
include lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/depend.make
# Include any dependencies generated by the compiler for this target.
include lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/compiler_depend.make

# Include the progress variables for this target.
include lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/progress.make

# Include the compile flags for this target's objects.
include lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/flags.make

lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/ThirdPartyMalloc.cpp.o: lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/flags.make
lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/ThirdPartyMalloc.cpp.o: lonestar/tutorial_examples/ThirdPartyMalloc.cpp
lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/ThirdPartyMalloc.cpp.o: lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/work/08474/ywwu/ls6/Galois/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building CXX object lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/ThirdPartyMalloc.cpp.o"
	cd /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/ThirdPartyMalloc.cpp.o -MF CMakeFiles/example-malloc.dir/ThirdPartyMalloc.cpp.o.d -o CMakeFiles/example-malloc.dir/ThirdPartyMalloc.cpp.o -c /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples/ThirdPartyMalloc.cpp

lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/ThirdPartyMalloc.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/example-malloc.dir/ThirdPartyMalloc.cpp.i"
	cd /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples/ThirdPartyMalloc.cpp > CMakeFiles/example-malloc.dir/ThirdPartyMalloc.cpp.i

lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/ThirdPartyMalloc.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/example-malloc.dir/ThirdPartyMalloc.cpp.s"
	cd /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples/ThirdPartyMalloc.cpp -o CMakeFiles/example-malloc.dir/ThirdPartyMalloc.cpp.s

# Object files for target example-malloc
example__malloc_OBJECTS = \
"CMakeFiles/example-malloc.dir/ThirdPartyMalloc.cpp.o"

# External object files for target example-malloc
example__malloc_EXTERNAL_OBJECTS =

lonestar/tutorial_examples/example-malloc: lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/ThirdPartyMalloc.cpp.o
lonestar/tutorial_examples/example-malloc: lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/build.make
lonestar/tutorial_examples/example-malloc: libgalois/libgalois_shmem.a
lonestar/tutorial_examples/example-malloc: lonestar/liblonestar/liblonestar.a
lonestar/tutorial_examples/example-malloc: libgalois/libgalois_shmem.a
lonestar/tutorial_examples/example-malloc: /usr/lib64/libnuma.so
lonestar/tutorial_examples/example-malloc: /work/08474/ywwu/ls6/source/llvm-project/build/lib/libLLVMSupport.a
lonestar/tutorial_examples/example-malloc: /usr/lib64/libz.so
lonestar/tutorial_examples/example-malloc: /usr/lib64/libzstd.so
lonestar/tutorial_examples/example-malloc: /usr/lib64/libtinfo.so
lonestar/tutorial_examples/example-malloc: /work/08474/ywwu/ls6/source/llvm-project/build/lib/libLLVMDemangle.a
lonestar/tutorial_examples/example-malloc: lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/link.txt
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --bold --progress-dir=/work/08474/ywwu/ls6/Galois/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "Linking CXX executable example-malloc"
	cd /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples && $(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/example-malloc.dir/link.txt --verbose=$(VERBOSE)

# Rule to build all files generated by this target.
lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/build: lonestar/tutorial_examples/example-malloc
.PHONY : lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/build

lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/clean:
	cd /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples && $(CMAKE_COMMAND) -P CMakeFiles/example-malloc.dir/cmake_clean.cmake
.PHONY : lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/clean

lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/depend:
	cd /work/08474/ywwu/ls6/Galois && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /work/08474/ywwu/ls6/Galois /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples /work/08474/ywwu/ls6/Galois /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples /work/08474/ywwu/ls6/Galois/lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : lonestar/tutorial_examples/CMakeFiles/example-malloc.dir/depend
