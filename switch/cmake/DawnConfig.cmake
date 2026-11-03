# mp2-nx: Dawn (encounter/dawn 1155e0ed + Switch patches), monolithic static, built for Horizon by
# switch/build_deps.sh. Dawn's own install step builds every target; only this library is needed.
get_filename_component(_dawn_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(NOT TARGET dawn::webgpu_dawn)
  add_library(dawn::webgpu_dawn STATIC IMPORTED GLOBAL)
  set_target_properties(dawn::webgpu_dawn PROPERTIES
    IMPORTED_LOCATION "${_dawn_prefix}/lib/libwebgpu_dawn.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_dawn_prefix}/include")
endif()
foreach(_t dawn_public_config dawncpp_headers)
  if(NOT TARGET dawn::${_t})
    add_library(dawn::${_t} INTERFACE IMPORTED GLOBAL)
    set_target_properties(dawn::${_t} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_dawn_prefix}/include")
  endif()
endforeach()
set(Dawn_FOUND TRUE)
