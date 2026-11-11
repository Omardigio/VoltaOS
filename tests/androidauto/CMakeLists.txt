# Standalone self-test of the Android Auto client (not part of the app build).
#   cmake -S tests/androidauto -B build/aa-test -G Ninja && cmake --build build/aa-test
#   ./build/aa-test/aa_fake_phone_test third_party/aap_cert/headunit.crt third_party/aap_cert/headunit.key
cmake_minimum_required(VERSION 3.16)
project(aa_fake_phone_test CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)   # AaSession.h has Q_OBJECT; the test file itself needs no moc
find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(Protobuf REQUIRED)
find_package(OpenSSL REQUIRED)
set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
file(GLOB_RECURSE PROTOS ${ROOT}/third_party/aap_protobuf/aap_protobuf/*.proto)
add_library(aap STATIC)
protobuf_generate(TARGET aap PROTOS ${PROTOS} IMPORT_DIRS ${ROOT}/third_party/aap_protobuf)
target_include_directories(aap PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(aap PUBLIC protobuf::libprotobuf)
target_compile_options(aap PRIVATE -w)
add_executable(aa_fake_phone_test fake_phone_test.cpp
    ${ROOT}/src/androidauto/AaFraming.cpp ${ROOT}/src/androidauto/AaTls.cpp ${ROOT}/src/androidauto/AaSession.cpp)
target_include_directories(aa_fake_phone_test PRIVATE ${ROOT}/src/androidauto)
target_link_libraries(aa_fake_phone_test PRIVATE Qt6::Core aap OpenSSL::SSL OpenSSL::Crypto)
