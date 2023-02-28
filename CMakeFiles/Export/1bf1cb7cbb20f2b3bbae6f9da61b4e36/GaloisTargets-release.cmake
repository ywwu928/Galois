#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Galois::support" for configuration "Release"
set_property(TARGET Galois::support APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Galois::support PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib64/libgalois_support.a"
  )

list(APPEND _cmake_import_check_targets Galois::support )
list(APPEND _cmake_import_check_files_for_Galois::support "${_IMPORT_PREFIX}/lib64/libgalois_support.a" )

# Import target "Galois::shmem" for configuration "Release"
set_property(TARGET Galois::shmem APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Galois::shmem PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib64/libgalois_shmem.a"
  )

list(APPEND _cmake_import_check_targets Galois::shmem )
list(APPEND _cmake_import_check_files_for_Galois::shmem "${_IMPORT_PREFIX}/lib64/libgalois_shmem.a" )

# Import target "Galois::dist_async" for configuration "Release"
set_property(TARGET Galois::dist_async APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Galois::dist_async PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib64/libgalois_dist_async.a"
  )

list(APPEND _cmake_import_check_targets Galois::dist_async )
list(APPEND _cmake_import_check_files_for_Galois::dist_async "${_IMPORT_PREFIX}/lib64/libgalois_dist_async.a" )

# Import target "Galois::gluon" for configuration "Release"
set_property(TARGET Galois::gluon APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Galois::gluon PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib64/libgalois_gluon.a"
  )

list(APPEND _cmake_import_check_targets Galois::gluon )
list(APPEND _cmake_import_check_files_for_Galois::gluon "${_IMPORT_PREFIX}/lib64/libgalois_gluon.a" )

# Import target "Galois::graph-convert" for configuration "Release"
set_property(TARGET Galois::graph-convert APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Galois::graph-convert PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/graph-convert"
  )

list(APPEND _cmake_import_check_targets Galois::graph-convert )
list(APPEND _cmake_import_check_files_for_Galois::graph-convert "${_IMPORT_PREFIX}/bin/graph-convert" )

# Import target "Galois::graph-convert-huge" for configuration "Release"
set_property(TARGET Galois::graph-convert-huge APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Galois::graph-convert-huge PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/graph-convert-huge"
  )

list(APPEND _cmake_import_check_targets Galois::graph-convert-huge )
list(APPEND _cmake_import_check_files_for_Galois::graph-convert-huge "${_IMPORT_PREFIX}/bin/graph-convert-huge" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
