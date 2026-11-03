set(D /work/deps)
foreach(pair "ABSEIL-CPP;dawn/third_party/abseil-cpp" "XXHASH;xxhash" "FMT;fmt" "ZLIB;zlib-ng" "PNG;libpng" "FREETYPE;freetype"
             "IMGUI;imgui" "SQLITE3;sqlite3" "ZSTD;zstd" "RMLUI;rmlui" "TRACY;tracy")
  list(GET pair 0 name)
  list(GET pair 1 dir)
  set(FETCHCONTENT_SOURCE_DIR_${name} "${D}/${dir}" CACHE PATH "" FORCE)
endforeach()
