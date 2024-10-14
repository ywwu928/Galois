add_library(distbench STATIC src/Start.cpp src/Input.cpp src/Output.cpp)
target_include_directories(distbench PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(GALOIS_FULL_MIRRORING)
    target_compile_definitions(distbench PRIVATE GALOIS_FULL_MIRRORING=1)
endif()

target_link_libraries(distbench Galois::cusp Galois::gluon LLVMSupport)
