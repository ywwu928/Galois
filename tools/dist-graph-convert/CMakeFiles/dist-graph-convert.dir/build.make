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
include tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/depend.make
# Include any dependencies generated by the compiler for this target.
include tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/compiler_depend.make

# Include the progress variables for this target.
include tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/progress.make

# Include the compile flags for this target's objects.
include tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/flags.make

tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/dist-graph-convert.cpp.o: tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/flags.make
tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/dist-graph-convert.cpp.o: tools/dist-graph-convert/dist-graph-convert.cpp
tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/dist-graph-convert.cpp.o: tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/work/08474/ywwu/ls6/Galois/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building CXX object tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/dist-graph-convert.cpp.o"
	cd /work/08474/ywwu/ls6/Galois/tools/dist-graph-convert && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/dist-graph-convert.cpp.o -MF CMakeFiles/dist-graph-convert.dir/dist-graph-convert.cpp.o.d -o CMakeFiles/dist-graph-convert.dir/dist-graph-convert.cpp.o -c /work/08474/ywwu/ls6/Galois/tools/dist-graph-convert/dist-graph-convert.cpp

tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/dist-graph-convert.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/dist-graph-convert.dir/dist-graph-convert.cpp.i"
	cd /work/08474/ywwu/ls6/Galois/tools/dist-graph-convert && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /work/08474/ywwu/ls6/Galois/tools/dist-graph-convert/dist-graph-convert.cpp > CMakeFiles/dist-graph-convert.dir/dist-graph-convert.cpp.i

tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/dist-graph-convert.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/dist-graph-convert.dir/dist-graph-convert.cpp.s"
	cd /work/08474/ywwu/ls6/Galois/tools/dist-graph-convert && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /work/08474/ywwu/ls6/Galois/tools/dist-graph-convert/dist-graph-convert.cpp -o CMakeFiles/dist-graph-convert.dir/dist-graph-convert.cpp.s

tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/dist-graph-convert-helpers.cpp.o: tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/flags.make
tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/dist-graph-convert-helpers.cpp.o: tools/dist-graph-convert/dist-graph-convert-helpers.cpp
tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/dist-graph-convert-helpers.cpp.o: tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/work/08474/ywwu/ls6/Galois/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "Building CXX object tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/dist-graph-convert-helpers.cpp.o"
	cd /work/08474/ywwu/ls6/Galois/tools/dist-graph-convert && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/dist-graph-convert-helpers.cpp.o -MF CMakeFiles/dist-graph-convert.dir/dist-graph-convert-helpers.cpp.o.d -o CMakeFiles/dist-graph-convert.dir/dist-graph-convert-helpers.cpp.o -c /work/08474/ywwu/ls6/Galois/tools/dist-graph-convert/dist-graph-convert-helpers.cpp

tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/dist-graph-convert-helpers.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/dist-graph-convert.dir/dist-graph-convert-helpers.cpp.i"
	cd /work/08474/ywwu/ls6/Galois/tools/dist-graph-convert && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /work/08474/ywwu/ls6/Galois/tools/dist-graph-convert/dist-graph-convert-helpers.cpp > CMakeFiles/dist-graph-convert.dir/dist-graph-convert-helpers.cpp.i

tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/dist-graph-convert-helpers.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/dist-graph-convert.dir/dist-graph-convert-helpers.cpp.s"
	cd /work/08474/ywwu/ls6/Galois/tools/dist-graph-convert && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /work/08474/ywwu/ls6/Galois/tools/dist-graph-convert/dist-graph-convert-helpers.cpp -o CMakeFiles/dist-graph-convert.dir/dist-graph-convert-helpers.cpp.s

# Object files for target dist-graph-convert
dist__graph__convert_OBJECTS = \
"CMakeFiles/dist-graph-convert.dir/dist-graph-convert.cpp.o" \
"CMakeFiles/dist-graph-convert.dir/dist-graph-convert-helpers.cpp.o"

# External object files for target dist-graph-convert
dist__graph__convert_EXTERNAL_OBJECTS =

tools/dist-graph-convert/dist-graph-convert: tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/dist-graph-convert.cpp.o
tools/dist-graph-convert/dist-graph-convert: tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/dist-graph-convert-helpers.cpp.o
tools/dist-graph-convert/dist-graph-convert: tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/build.make
tools/dist-graph-convert/dist-graph-convert: libdist/libgalois_dist_async.a
tools/dist-graph-convert/dist-graph-convert: /work/08474/ywwu/ls6/source/llvm-project/build/lib/libLLVMSupport.a
tools/dist-graph-convert/dist-graph-convert: /opt/intel/compilers_and_libraries_2020.4.304/linux/mpi/intel64/lib/libmpicxx.so
tools/dist-graph-convert/dist-graph-convert: /opt/intel/compilers_and_libraries_2020.4.304/linux/mpi/intel64/lib/release/libmpi.so
tools/dist-graph-convert/dist-graph-convert: /usr/lib64/librt.so
tools/dist-graph-convert/dist-graph-convert: /usr/lib64/libpthread.so
tools/dist-graph-convert/dist-graph-convert: /usr/lib64/libdl.so
tools/dist-graph-convert/dist-graph-convert: libgalois/libgalois_shmem.a
tools/dist-graph-convert/dist-graph-convert: /usr/lib64/libnuma.so
tools/dist-graph-convert/dist-graph-convert: /usr/lib64/libz.so
tools/dist-graph-convert/dist-graph-convert: /usr/lib64/libzstd.so
tools/dist-graph-convert/dist-graph-convert: /usr/lib64/libtinfo.so
tools/dist-graph-convert/dist-graph-convert: /work/08474/ywwu/ls6/source/llvm-project/build/lib/libLLVMDemangle.a
tools/dist-graph-convert/dist-graph-convert: tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/link.txt
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --bold --progress-dir=/work/08474/ywwu/ls6/Galois/CMakeFiles --progress-num=$(CMAKE_PROGRESS_3) "Linking CXX executable dist-graph-convert"
	cd /work/08474/ywwu/ls6/Galois/tools/dist-graph-convert && $(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/dist-graph-convert.dir/link.txt --verbose=$(VERBOSE)

# Rule to build all files generated by this target.
tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/build: tools/dist-graph-convert/dist-graph-convert
.PHONY : tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/build

tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/clean:
	cd /work/08474/ywwu/ls6/Galois/tools/dist-graph-convert && $(CMAKE_COMMAND) -P CMakeFiles/dist-graph-convert.dir/cmake_clean.cmake
.PHONY : tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/clean

tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/depend:
	cd /work/08474/ywwu/ls6/Galois && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /work/08474/ywwu/ls6/Galois /work/08474/ywwu/ls6/Galois/tools/dist-graph-convert /work/08474/ywwu/ls6/Galois /work/08474/ywwu/ls6/Galois/tools/dist-graph-convert /work/08474/ywwu/ls6/Galois/tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : tools/dist-graph-convert/CMakeFiles/dist-graph-convert.dir/depend

