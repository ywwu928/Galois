add_library(distbench STATIC src/Start.cpp src/Input.cpp src/Output.cpp)
target_include_directories(distbench PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(GALOIS_NO_MIRRORING)
    target_compile_definitions(distbench PRIVATE GALOIS_NO_MIRRORING=1)
endif()
if(GALOIS_EXCHANGE_PHANTOM_LID)
    target_compile_definitions(distbench PRIVATE GALOIS_EXCHANGE_PHANTOM_LID=1)
endif()

target_link_libraries(distbench Galois::cusp Galois::gluon LLVMSupport)
