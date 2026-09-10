# Fetch libwebp (BSD) and build only the codec libraries, pinned to a tag.
include(FetchContent)

set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX ON CACHE BOOL "" FORCE)   # ICC profiles ride in the ICCP chunk
set(WEBP_LINK_STATIC ON CACHE BOOL "" FORCE)
set(WEBP_UNICODE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  libwebp
  GIT_REPOSITORY https://github.com/webmproject/libwebp.git
  GIT_TAG        v1.4.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(libwebp)

# Third-party warnings are not ours to fix.
foreach(t webp webpdecoder webpdecode webpdsp webpdspdecode webpencode webputils webputilsdecode webpdemux libwebpmux sharpyuv cpufeatures)
  if(TARGET ${t})
    if(MSVC)
      target_compile_options(${t} PRIVATE /w)
    else()
      target_compile_options(${t} PRIVATE -w)
    endif()
  endif()
endforeach()
