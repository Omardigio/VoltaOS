# Standalone self-test of the Android Auto client (not part of the app build).
#   cmake -S tests/androidauto -B build/aa-test -G Ninja && cmake --build build/aa-test
#   ./build/aa-test/aa_fake_phone_test third_party/aap_cert/headunit.crt third_party/aap_cert/headunit.key
cmake_minimum_required(VERSION 3.16)
project(aa_fake_phone_test CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)   # AaSession.h has Q_OBJECT; the test file itself needs no moc
find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(OpenSSL REQUIRED)
set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
include(${ROOT}/third_party/aap_protobuf/AapProtobuf.cmake)
add_executable(aa_fake_phone_test fake_phone_test.cpp
    ${ROOT}/src/androidauto/AaFraming.cpp ${ROOT}/src/androidauto/AaTls.cpp ${ROOT}/src/androidauto/AaSession.cpp)
target_include_directories(aa_fake_phone_test PRIVATE ${ROOT}/src/androidauto)
target_link_libraries(aa_fake_phone_test PRIVATE Qt6::Core aap_protobuf OpenSSL::SSL OpenSSL::Crypto)
