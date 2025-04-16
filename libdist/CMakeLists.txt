add_library(galois_dist_async STATIC)
add_library(Galois::dist_async ALIAS galois_dist_async)
add_dependencies(lib galois_dist_async)
set_target_properties(galois_dist_async PROPERTIES EXPORT_NAME dist_async)

target_sources(galois_dist_async PRIVATE
        src/Barrier.cpp
        src/DistGalois.cpp
        src/DistStats.cpp
        src/Network.cpp
)

target_include_directories(galois_dist_async PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(galois_dist_async PUBLIC MPI::MPI_CXX)
target_link_libraries(galois_dist_async PUBLIC galois_shmem LLVMSupport)

target_compile_definitions(galois_dist_async PRIVATE GALOIS_SUPPORT_ASYNC=1)

if (GALOIS_USE_BARE_MPI)
  target_compile_definitions(galois_dist_async PRIVATE GALOIS_USE_BARE_MPI=1)
endif()

if(GALOIS_FULL_MIRRORING)
    target_compile_definitions(galois_dist_async PRIVATE GALOIS_FULL_MIRRORING=1)
endif()
if(GALOIS_NO_MIRRORING)
    target_compile_definitions(galois_dist_async PRIVATE GALOIS_NO_MIRRORING=1)
endif()

install(
  DIRECTORY include/
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  COMPONENT dev
  FILES_MATCHING PATTERN "*.h"
)

install(TARGETS galois_dist_async
  EXPORT GaloisTargets
  LIBRARY
    DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    COMPONENT shlib
  ARCHIVE
    DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    COMPONENT lib
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
