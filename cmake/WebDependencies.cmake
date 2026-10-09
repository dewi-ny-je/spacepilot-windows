# Pinned, source-built dependencies. Static TLS libraries keep the service and
# the setup tool free of OpenSSL DLLs that could be shadowed on the search path.
include(FetchContent)
include(ExternalProject)
FetchContent_Declare(axial_boost
  URL https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-b2-nodocs.tar.xz
  URL_HASH SHA256=ea7b982002cc9dfbe59b0b217b206f470dc75f3de0bb2973d844118934d82411
  SOURCE_SUBDIR axial-unused)
FetchContent_MakeAvailable(axial_boost)
add_library(axial-boost INTERFACE)
target_include_directories(axial-boost SYSTEM INTERFACE "${axial_boost_SOURCE_DIR}")
target_compile_definitions(axial-boost INTERFACE BOOST_ASIO_NO_DEPRECATED BOOST_ALL_NO_LIB)
if(AXIAL_ADAPTERS_ONLY)
  return()
endif()

set(AXIAL_OPENSSL_ROOT "" CACHE PATH "Prebuilt static OpenSSL 3 (include/ and lib/); empty builds the pinned release")
if(AXIAL_OPENSSL_ROOT)
  set(tls_prefix "${AXIAL_OPENSSL_ROOT}")
else()
  set(tls_prefix "${CMAKE_BINARY_DIR}/tls/install")
  set(tls_url https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz)
  set(tls_hash SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a)
  find_program(PERL perl REQUIRED)
  if(MSVC)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(tls_platform VC-WIN64A)
    else()
      set(tls_platform VC-WIN32)
    endif()
    find_program(NMAKE nmake REQUIRED)
    ExternalProject_Add(axial_tls URL ${tls_url} URL_HASH ${tls_hash}
      PREFIX "${CMAKE_BINARY_DIR}/tls" BUILD_IN_SOURCE TRUE
      CONFIGURE_COMMAND "${PERL}" Configure ${tls_platform} no-shared no-tests no-module no-asm
        "--prefix=${tls_prefix}" "--openssldir=${tls_prefix}/ssl"
      BUILD_COMMAND "${NMAKE}" /nologo build_libs
      INSTALL_COMMAND "${NMAKE}" /nologo install_dev
      LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_INSTALL TRUE
      BUILD_BYPRODUCTS "${tls_prefix}/lib/libssl.lib" "${tls_prefix}/lib/libcrypto.lib")
  else()
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(tls_platform mingw64)
    else()
      set(tls_platform mingw)
    endif()
    set(tls_cross)
    if(CMAKE_CROSSCOMPILING AND AXIAL_MINGW_PREFIX)
      set(tls_cross "--cross-compile-prefix=${AXIAL_MINGW_PREFIX}")
    endif()
    find_program(MAKE_PROGRAM NAMES make mingw32-make REQUIRED)
    ExternalProject_Add(axial_tls URL ${tls_url} URL_HASH ${tls_hash}
      PREFIX "${CMAKE_BINARY_DIR}/tls" BUILD_IN_SOURCE TRUE
      CONFIGURE_COMMAND "${PERL}" Configure ${tls_platform} ${tls_cross} no-shared no-tests no-module
        "--prefix=${tls_prefix}" --libdir=lib
      BUILD_COMMAND "${MAKE_PROGRAM}" -j8 build_libs
      INSTALL_COMMAND "${MAKE_PROGRAM}" install_dev
      LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_INSTALL TRUE
      BUILD_BYPRODUCTS "${tls_prefix}/lib/libssl.a" "${tls_prefix}/lib/libcrypto.a")
  endif()
  file(MAKE_DIRECTORY "${tls_prefix}/include")
endif()
if(MSVC)
  set(tls_libraries "${tls_prefix}/lib/libssl.lib" "${tls_prefix}/lib/libcrypto.lib")
else()
  set(tls_libraries "${tls_prefix}/lib/libssl.a" "${tls_prefix}/lib/libcrypto.a")
endif()
add_library(axial-web-dependencies INTERFACE)
if(TARGET axial_tls)
  add_dependencies(axial-web-dependencies axial_tls)
endif()
target_include_directories(axial-web-dependencies SYSTEM INTERFACE "${tls_prefix}/include")
target_link_libraries(axial-web-dependencies INTERFACE axial-boost ${tls_libraries} crypt32 ws2_32 mswsock user32 advapi32 bcrypt)
