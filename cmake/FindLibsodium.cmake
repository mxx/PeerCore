# FindLibsodium.cmake
# Finds the libsodium cryptography library.
#
# Result variables:
#   Libsodium_FOUND
#   Libsodium_INCLUDE_DIRS
#   Libsodium_LIBRARIES
#
# Imported targets:
#   Libsodium::Libsodium

find_path(Libsodium_INCLUDE_DIR sodium.h
  HINTS
    ENV LIBSODIUM_DIR
    /usr/local/include
    /usr/include
    /opt/homebrew/include
)

find_library(Libsodium_LIBRARY
  NAMES sodium libsodium
  HINTS
    ENV LIBSODIUM_DIR
    /usr/local/lib
    /usr/lib
    /opt/homebrew/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libsodium
  REQUIRED_VARS Libsodium_LIBRARY Libsodium_INCLUDE_DIR
)

if(Libsodium_FOUND AND NOT TARGET Libsodium::Libsodium)
  add_library(Libsodium::Libsodium UNKNOWN IMPORTED)
  set_target_properties(Libsodium::Libsodium PROPERTIES
    IMPORTED_LOCATION "${Libsodium_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${Libsodium_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(Libsodium_INCLUDE_DIR Libsodium_LIBRARY)
