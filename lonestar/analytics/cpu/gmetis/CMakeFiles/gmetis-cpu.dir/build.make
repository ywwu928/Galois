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
include lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/depend.make
# Include any dependencies generated by the compiler for this target.
include lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/compiler_depend.make

# Include the progress variables for this target.
include lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/progress.make

# Include the compile flags for this target's objects.
include lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/flags.make

lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Coarsening.cpp.o: lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/flags.make
lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Coarsening.cpp.o: lonestar/analytics/cpu/gmetis/Coarsening.cpp
lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Coarsening.cpp.o: lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/work/08474/ywwu/ls6/Galois/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building CXX object lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Coarsening.cpp.o"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Coarsening.cpp.o -MF CMakeFiles/gmetis-cpu.dir/Coarsening.cpp.o.d -o CMakeFiles/gmetis-cpu.dir/Coarsening.cpp.o -c /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis/Coarsening.cpp

lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Coarsening.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/gmetis-cpu.dir/Coarsening.cpp.i"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis/Coarsening.cpp > CMakeFiles/gmetis-cpu.dir/Coarsening.cpp.i

lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Coarsening.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/gmetis-cpu.dir/Coarsening.cpp.s"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis/Coarsening.cpp -o CMakeFiles/gmetis-cpu.dir/Coarsening.cpp.s

lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/GMetis.cpp.o: lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/flags.make
lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/GMetis.cpp.o: lonestar/analytics/cpu/gmetis/GMetis.cpp
lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/GMetis.cpp.o: lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/work/08474/ywwu/ls6/Galois/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "Building CXX object lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/GMetis.cpp.o"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/GMetis.cpp.o -MF CMakeFiles/gmetis-cpu.dir/GMetis.cpp.o.d -o CMakeFiles/gmetis-cpu.dir/GMetis.cpp.o -c /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis/GMetis.cpp

lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/GMetis.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/gmetis-cpu.dir/GMetis.cpp.i"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis/GMetis.cpp > CMakeFiles/gmetis-cpu.dir/GMetis.cpp.i

lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/GMetis.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/gmetis-cpu.dir/GMetis.cpp.s"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis/GMetis.cpp -o CMakeFiles/gmetis-cpu.dir/GMetis.cpp.s

lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Metric.cpp.o: lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/flags.make
lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Metric.cpp.o: lonestar/analytics/cpu/gmetis/Metric.cpp
lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Metric.cpp.o: lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/work/08474/ywwu/ls6/Galois/CMakeFiles --progress-num=$(CMAKE_PROGRESS_3) "Building CXX object lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Metric.cpp.o"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Metric.cpp.o -MF CMakeFiles/gmetis-cpu.dir/Metric.cpp.o.d -o CMakeFiles/gmetis-cpu.dir/Metric.cpp.o -c /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis/Metric.cpp

lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Metric.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/gmetis-cpu.dir/Metric.cpp.i"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis/Metric.cpp > CMakeFiles/gmetis-cpu.dir/Metric.cpp.i

lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Metric.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/gmetis-cpu.dir/Metric.cpp.s"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis/Metric.cpp -o CMakeFiles/gmetis-cpu.dir/Metric.cpp.s

lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Partitioning.cpp.o: lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/flags.make
lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Partitioning.cpp.o: lonestar/analytics/cpu/gmetis/Partitioning.cpp
lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Partitioning.cpp.o: lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/work/08474/ywwu/ls6/Galois/CMakeFiles --progress-num=$(CMAKE_PROGRESS_4) "Building CXX object lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Partitioning.cpp.o"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Partitioning.cpp.o -MF CMakeFiles/gmetis-cpu.dir/Partitioning.cpp.o.d -o CMakeFiles/gmetis-cpu.dir/Partitioning.cpp.o -c /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis/Partitioning.cpp

lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Partitioning.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/gmetis-cpu.dir/Partitioning.cpp.i"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis/Partitioning.cpp > CMakeFiles/gmetis-cpu.dir/Partitioning.cpp.i

lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Partitioning.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/gmetis-cpu.dir/Partitioning.cpp.s"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis/Partitioning.cpp -o CMakeFiles/gmetis-cpu.dir/Partitioning.cpp.s

lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Refine.cpp.o: lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/flags.make
lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Refine.cpp.o: lonestar/analytics/cpu/gmetis/Refine.cpp
lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Refine.cpp.o: lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/work/08474/ywwu/ls6/Galois/CMakeFiles --progress-num=$(CMAKE_PROGRESS_5) "Building CXX object lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Refine.cpp.o"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Refine.cpp.o -MF CMakeFiles/gmetis-cpu.dir/Refine.cpp.o.d -o CMakeFiles/gmetis-cpu.dir/Refine.cpp.o -c /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis/Refine.cpp

lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Refine.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/gmetis-cpu.dir/Refine.cpp.i"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis/Refine.cpp > CMakeFiles/gmetis-cpu.dir/Refine.cpp.i

lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Refine.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/gmetis-cpu.dir/Refine.cpp.s"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis/Refine.cpp -o CMakeFiles/gmetis-cpu.dir/Refine.cpp.s

# Object files for target gmetis-cpu
gmetis__cpu_OBJECTS = \
"CMakeFiles/gmetis-cpu.dir/Coarsening.cpp.o" \
"CMakeFiles/gmetis-cpu.dir/GMetis.cpp.o" \
"CMakeFiles/gmetis-cpu.dir/Metric.cpp.o" \
"CMakeFiles/gmetis-cpu.dir/Partitioning.cpp.o" \
"CMakeFiles/gmetis-cpu.dir/Refine.cpp.o"

# External object files for target gmetis-cpu
gmetis__cpu_EXTERNAL_OBJECTS =

lonestar/analytics/cpu/gmetis/gmetis-cpu: lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Coarsening.cpp.o
lonestar/analytics/cpu/gmetis/gmetis-cpu: lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/GMetis.cpp.o
lonestar/analytics/cpu/gmetis/gmetis-cpu: lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Metric.cpp.o
lonestar/analytics/cpu/gmetis/gmetis-cpu: lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Partitioning.cpp.o
lonestar/analytics/cpu/gmetis/gmetis-cpu: lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/Refine.cpp.o
lonestar/analytics/cpu/gmetis/gmetis-cpu: lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/build.make
lonestar/analytics/cpu/gmetis/gmetis-cpu: libgalois/libgalois_shmem.a
lonestar/analytics/cpu/gmetis/gmetis-cpu: lonestar/liblonestar/liblonestar.a
lonestar/analytics/cpu/gmetis/gmetis-cpu: libgalois/libgalois_shmem.a
lonestar/analytics/cpu/gmetis/gmetis-cpu: /usr/lib64/libnuma.so
lonestar/analytics/cpu/gmetis/gmetis-cpu: /work/08474/ywwu/ls6/source/llvm-project/build/lib/libLLVMSupport.a
lonestar/analytics/cpu/gmetis/gmetis-cpu: /usr/lib64/libz.so
lonestar/analytics/cpu/gmetis/gmetis-cpu: /usr/lib64/libzstd.so
lonestar/analytics/cpu/gmetis/gmetis-cpu: /usr/lib64/libtinfo.so
lonestar/analytics/cpu/gmetis/gmetis-cpu: /work/08474/ywwu/ls6/source/llvm-project/build/lib/libLLVMDemangle.a
lonestar/analytics/cpu/gmetis/gmetis-cpu: lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/link.txt
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --bold --progress-dir=/work/08474/ywwu/ls6/Galois/CMakeFiles --progress-num=$(CMAKE_PROGRESS_6) "Linking CXX executable gmetis-cpu"
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis && $(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/gmetis-cpu.dir/link.txt --verbose=$(VERBOSE)

# Rule to build all files generated by this target.
lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/build: lonestar/analytics/cpu/gmetis/gmetis-cpu
.PHONY : lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/build

lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/clean:
	cd /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis && $(CMAKE_COMMAND) -P CMakeFiles/gmetis-cpu.dir/cmake_clean.cmake
.PHONY : lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/clean

lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/depend:
	cd /work/08474/ywwu/ls6/Galois && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /work/08474/ywwu/ls6/Galois /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis /work/08474/ywwu/ls6/Galois /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis /work/08474/ywwu/ls6/Galois/lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : lonestar/analytics/cpu/gmetis/CMakeFiles/gmetis-cpu.dir/depend
