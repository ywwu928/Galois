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
include lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/depend.make
# Include any dependencies generated by the compiler for this target.
include lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/compiler_depend.make

# Include the progress variables for this target.
include lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/progress.make

# Include the compile flags for this target's objects.
include lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/flags.make

lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/bfs_push.cpp.o: lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/flags.make
lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/bfs_push.cpp.o: lonestar/analytics/distributed/bfs/bfs_push.cpp
lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/bfs_push.cpp.o: lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/work/08474/ywwu/ls6/Galois/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building CXX object lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/bfs_push.cpp.o"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/bfs_push.cpp.o -MF CMakeFiles/bfs-push-dist.dir/bfs_push.cpp.o.d -o CMakeFiles/bfs-push-dist.dir/bfs_push.cpp.o -c /work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs/bfs_push.cpp

lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/bfs_push.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/bfs-push-dist.dir/bfs_push.cpp.i"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs/bfs_push.cpp > CMakeFiles/bfs-push-dist.dir/bfs_push.cpp.i

lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/bfs_push.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/bfs-push-dist.dir/bfs_push.cpp.s"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs/bfs_push.cpp -o CMakeFiles/bfs-push-dist.dir/bfs_push.cpp.s

# Object files for target bfs-push-dist
bfs__push__dist_OBJECTS = \
"CMakeFiles/bfs-push-dist.dir/bfs_push.cpp.o"

# External object files for target bfs-push-dist
bfs__push__dist_EXTERNAL_OBJECTS =

lonestar/analytics/distributed/bfs/bfs-push-dist: lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/bfs_push.cpp.o
lonestar/analytics/distributed/bfs/bfs-push-dist: lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/build.make
lonestar/analytics/distributed/bfs/bfs-push-dist: libgalois/libgalois_shmem.a
lonestar/analytics/distributed/bfs/bfs-push-dist: /work/08474/ywwu/ls6/source/llvm-project/build/lib/libLLVMSupport.a
lonestar/analytics/distributed/bfs/bfs-push-dist: lonestar/libdistbench/libdistbench.a
lonestar/analytics/distributed/bfs/bfs-push-dist: /work/08474/ywwu/ls6/source/llvm-project/build/lib/libLLVMSupport.a
lonestar/analytics/distributed/bfs/bfs-push-dist: /usr/lib64/libz.so
lonestar/analytics/distributed/bfs/bfs-push-dist: /usr/lib64/libzstd.so
lonestar/analytics/distributed/bfs/bfs-push-dist: /usr/lib64/libtinfo.so
lonestar/analytics/distributed/bfs/bfs-push-dist: /work/08474/ywwu/ls6/source/llvm-project/build/lib/libLLVMDemangle.a
lonestar/analytics/distributed/bfs/bfs-push-dist: libgluon/libgalois_gluon.a
lonestar/analytics/distributed/bfs/bfs-push-dist: libdist/libgalois_dist_async.a
lonestar/analytics/distributed/bfs/bfs-push-dist: libgalois/libgalois_shmem.a
lonestar/analytics/distributed/bfs/bfs-push-dist: /usr/lib64/libnuma.so
lonestar/analytics/distributed/bfs/bfs-push-dist: /opt/intel/compilers_and_libraries_2020.4.304/linux/mpi/intel64/lib/libmpicxx.so
lonestar/analytics/distributed/bfs/bfs-push-dist: /opt/intel/compilers_and_libraries_2020.4.304/linux/mpi/intel64/lib/release/libmpi.so
lonestar/analytics/distributed/bfs/bfs-push-dist: /usr/lib64/librt.so
lonestar/analytics/distributed/bfs/bfs-push-dist: /usr/lib64/libpthread.so
lonestar/analytics/distributed/bfs/bfs-push-dist: /usr/lib64/libdl.so
lonestar/analytics/distributed/bfs/bfs-push-dist: lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/link.txt
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --bold --progress-dir=/work/08474/ywwu/ls6/Galois/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "Linking CXX executable bfs-push-dist"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs && $(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/bfs-push-dist.dir/link.txt --verbose=$(VERBOSE)

# Rule to build all files generated by this target.
lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/build: lonestar/analytics/distributed/bfs/bfs-push-dist
.PHONY : lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/build

lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/clean:
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs && $(CMAKE_COMMAND) -P CMakeFiles/bfs-push-dist.dir/cmake_clean.cmake
.PHONY : lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/clean

lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/depend:
	cd /work/08474/ywwu/ls6/Galois && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /work/08474/ywwu/ls6/Galois /work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs /work/08474/ywwu/ls6/Galois /work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs /work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : lonestar/analytics/distributed/bfs/CMakeFiles/bfs-push-dist.dir/depend
