# Compiles the Android Auto protocol messages (third_party/aap_protobuf/aap_protobuf/*.proto) into a static
# library. A plain protoc rule instead of protobuf_generate(): that function puts the generated files in
# different folders depending on the CMake / protobuf version and on the IDE, and the includes
# ("aap_protobuf/service/...pb.h") stop being found.
#   include(third_party/aap_protobuf/AapProtobuf.cmake)   ->   target: aap_protobuf
find_package(Protobuf REQUIRED)

set(AAP_PROTO_ROOT ${CMAKE_CURRENT_LIST_DIR})
set(AAP_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/aap_gen)
file(GLOB_RECURSE AAP_PROTOS CONFIGURE_DEPENDS ${AAP_PROTO_ROOT}/aap_protobuf/*.proto)

set(AAP_GEN_SRCS)
set(AAP_GEN_HDRS)
foreach(proto ${AAP_PROTOS})
    file(RELATIVE_PATH rel ${AAP_PROTO_ROOT} ${proto})
    string(REGEX REPLACE "\\.proto$" "" base ${rel})
    list(APPEND AAP_GEN_SRCS ${AAP_GEN_DIR}/${base}.pb.cc)
    list(APPEND AAP_GEN_HDRS ${AAP_GEN_DIR}/${base}.pb.h)
endforeach()

if(TARGET protobuf::protoc)
    set(AAP_PROTOC $<TARGET_FILE:protobuf::protoc>)
else()
    set(AAP_PROTOC ${Protobuf_PROTOC_EXECUTABLE})
endif()

file(MAKE_DIRECTORY ${AAP_GEN_DIR})
add_custom_command(
    OUTPUT ${AAP_GEN_SRCS} ${AAP_GEN_HDRS}
    COMMAND ${AAP_PROTOC} --cpp_out=${AAP_GEN_DIR} -I ${AAP_PROTO_ROOT} ${AAP_PROTOS}
    DEPENDS ${AAP_PROTOS}
    COMMENT "Android Auto protocol messages (protoc)"
    VERBATIM)

add_library(aap_protobuf STATIC ${AAP_GEN_SRCS})
target_include_directories(aap_protobuf PUBLIC ${AAP_GEN_DIR})
target_link_libraries(aap_protobuf PUBLIC protobuf::libprotobuf)
target_compile_options(aap_protobuf PRIVATE -w)
set_target_properties(aap_protobuf PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
