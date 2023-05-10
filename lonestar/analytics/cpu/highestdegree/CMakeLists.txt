add_executable(highestdegree-cpu highestdegree.cpp)
add_dependencies(apps highestdegree-cpu)
target_link_libraries(highestdegree-cpu PRIVATE Galois::shmem lonestar)
install(TARGETS highestdegree-cpu DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT apps EXCLUDE_FROM_ALL)
