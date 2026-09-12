find_package(SDL2 REQUIRED)
# SDL2 creates the context through legacy libGL; link the same library so
# our direct GL calls (texture upload) share its dispatch. GLVND libOpenGL
# silently no-ops here.
if(NOT WIN32)
  set(OpenGL_GL_PREFERENCE LEGACY)
endif()
find_package(OpenGL REQUIRED)
include(${CMAKE_SOURCE_DIR}/cmake/imgui.cmake)

# The window icon, embedded as a byte array so the binary is self-contained.
set(FIRN_ICON_CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/IconData.cpp)
add_custom_command(
  OUTPUT ${FIRN_ICON_CPP}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_SOURCE_DIR}/assets/icon-128.png -DOUTPUT=${FIRN_ICON_CPP}
          -DSYMBOL=kFirnIconPng -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${CMAKE_SOURCE_DIR}/assets/icon-128.png ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  COMMENT "Embedding the window icon")

# Version, git commit and build date, refreshed on every build.
set(FIRN_VERSION_CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/Version.cpp)
add_custom_target(firn_version
  COMMAND ${CMAKE_COMMAND} -DOUTPUT=${FIRN_VERSION_CPP} -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
          -DVERSION=${PROJECT_VERSION} -P ${CMAKE_SOURCE_DIR}/cmake/version.cmake
  BYPRODUCTS ${FIRN_VERSION_CPP}
  COMMENT "Recording the version and commit")

set(FIRN_COMMANDS_H ${CMAKE_CURRENT_BINARY_DIR}/generated/InheritedCommands.h)
add_custom_command(
  OUTPUT ${FIRN_COMMANDS_H}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/src/Script.cpp -DOUTPUT=${FIRN_COMMANDS_H}
          -P ${CMAKE_SOURCE_DIR}/cmake/commands.cmake
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/Script.cpp ${CMAKE_SOURCE_DIR}/cmake/commands.cmake
  COMMENT "Reading the inherited command names out of Script.cpp")

# Share the application implementation with headless state/menu regression tests.
add_library(firn_app OBJECT
  ${FIRN_ICON_CPP}
  ${FIRN_VERSION_CPP}
  ${FIRN_COMMANDS_H}
  src/ui/About.cpp
  src/ui/Theme.cpp
  src/ui/ThemeEditor.cpp
  src/ui/EffectBrowser.cpp
  src/App.cpp
  src/Vectors.cpp
  src/Drive.cpp
  src/Actions.cpp
  src/ActionSchema.cpp
  src/DrawingActions.cpp
  src/VectorActions.cpp
  src/Script.cpp
  src/Config.cpp
  src/Clipboard.cpp
  src/UpdateCheck.cpp
  src/Autosave.cpp
  src/Tablet.cpp
  src/ui/Canvas.cpp
  src/ui/Palettes.cpp
  src/ui/Menu.cpp
  src/ui/FileDialog.cpp
  src/ui/Adjust.cpp
  src/ui/TextDialog.cpp
  src/ui/VectorDialog.cpp
  src/ui/AdjustmentLayer.cpp
  src/ui/LayerStyles.cpp
  src/ui/ImageMenu.cpp
  src/ui/Toolbar.cpp
  src/ui/Icons.cpp
  src/ui/MaterialDialog.cpp
  src/ui/SelectionMenu.cpp
  src/tools/Tools.cpp
  src/tools/VectorTools.cpp
  src/tools/DeformTools.cpp
  src/tools/WarpTools.cpp
)
target_include_directories(firn_app PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated src)
add_dependencies(firn_app firn_version)
# Lets a development build find the sample tubes in the (gitignored) backup.
target_compile_definitions(firn_app PUBLIC FIRN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(firn_app PUBLIC firncore imgui SDL2::SDL2 OpenGL::GL)
if(WIN32)
  target_link_libraries(firn_app PUBLIC ws2_32)   # the driver's loopback socket
endif()
# Pen tablets: XInput2 on X11 (optional at build time), pointer messages on
# Windows, NSEvent tablet data on macOS.
if(UNIX AND NOT APPLE)
  find_package(X11)
  if(X11_FOUND AND X11_Xi_FOUND)
    target_compile_definitions(firn_app PUBLIC FIRN_HAVE_XI2)
    target_link_libraries(firn_app PUBLIC X11::X11 X11::Xi)
    message(STATUS "Pen tablets: XInput2")
  else()
    message(STATUS "Pen tablets: libxi not found, pressure disabled")
  endif()
endif()
if(APPLE)
  enable_language(OBJCXX)
  target_sources(firn_app PRIVATE src/Tablet_mac.mm src/NativeMenu_mac.mm src/NativePointer_mac.mm)
  target_link_libraries(firn_app PUBLIC "-framework Cocoa")
  # ARC: NativeMenu_mac.mm allocates one Objective-C object per native menu
  # item on every frame (see FirnMenuAction in NativeMenu_mac.mm); without
  # ARC those would need manual retain/release bookkeeping to avoid leaking.
  set_source_files_properties(src/Tablet_mac.mm src/NativeMenu_mac.mm src/NativePointer_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()
add_executable(firn src/main.cpp)
target_link_libraries(firn PRIVATE firn_app)
if(WIN32)
  # Plain main(); SDL's own entry point is not used. A GUI subsystem keeps
  # the console window away; the entry point still runs the plain main().
  target_compile_definitions(firn PRIVATE SDL_MAIN_HANDLED)
  target_sources(firn PRIVATE ${CMAKE_SOURCE_DIR}/assets/firn.rc)
  set_target_properties(firn PROPERTIES WIN32_EXECUTABLE TRUE)
  if(MSVC)
    target_link_options(firn PRIVATE /ENTRY:mainCRTStartup)
  else()
    target_link_options(firn PRIVATE -mwindows -Wl,-e,mainCRTStartup)
  endif()
  # Put SDL2.dll beside the built exe so it runs from the build tree. vcpkg's
  # toolchain does this itself; SDL's own VC package (SDL2_DIR) does not.
  firn_copy_runtime_dlls(firn)
endif()

# Keep the CLI-friendly executable path, and also produce a discoverable
# macOS app for Finder and accessibility/computer-use clients.
if(APPLE)
  configure_file(${CMAKE_SOURCE_DIR}/assets/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Firn-Info.plist @ONLY)
  add_custom_target(firn_bundle ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/Firn.app/Contents/MacOS
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:firn> ${CMAKE_CURRENT_BINARY_DIR}/Firn.app/Contents/MacOS/firn
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_BINARY_DIR}/Firn-Info.plist ${CMAKE_CURRENT_BINARY_DIR}/Firn.app/Contents/Info.plist
    DEPENDS firn VERBATIM)
endif()
