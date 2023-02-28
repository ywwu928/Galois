# CMake generated Testfile for 
# Source directory: /work/08474/ywwu/ls6/Galois/libsupport/test
# Build directory: /work/08474/ywwu/ls6/Galois/libsupport/test
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(unit-getenv "/work/08474/ywwu/ls6/Galois/libsupport/test/unit-getenv")
set_tests_properties(unit-getenv PROPERTIES  ENVIRONMENT "GALOIS_DO_NOT_BIND_THREADS=1" LABELS "quick" _BACKTRACE_TRIPLES "/work/08474/ywwu/ls6/Galois/libsupport/test/CMakeLists.txt;9;add_test;/work/08474/ywwu/ls6/Galois/libsupport/test/CMakeLists.txt;19;add_test_unit;/work/08474/ywwu/ls6/Galois/libsupport/test/CMakeLists.txt;0;")
add_test(unit-logging "/work/08474/ywwu/ls6/Galois/libsupport/test/unit-logging")
set_tests_properties(unit-logging PROPERTIES  ENVIRONMENT "GALOIS_DO_NOT_BIND_THREADS=1" LABELS "quick" _BACKTRACE_TRIPLES "/work/08474/ywwu/ls6/Galois/libsupport/test/CMakeLists.txt;9;add_test;/work/08474/ywwu/ls6/Galois/libsupport/test/CMakeLists.txt;20;add_test_unit;/work/08474/ywwu/ls6/Galois/libsupport/test/CMakeLists.txt;0;")
