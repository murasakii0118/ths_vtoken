cmake_minimum_required(VERSION 3.16)

project(ths_vToken LANGUAGES C CXX)

if(MSVC)
    message(FATAL_ERROR "MSVC is not supported. Use MinGW-w64 GCC.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

get_filename_component(THIRD "${CMAKE_CURRENT_SOURCE_DIR}/../3rd" ABSOLUTE)

# --------------------------------------------------------------------------
#  QuickJS（已预编译的 libquickjs.a）
# --------------------------------------------------------------------------
if(NOT EXISTS "${THIRD}/quickjs/libquickjs.a")
    message(FATAL_ERROR "libquickjs.a not found at ${THIRD}/quickjs")
endif()

add_library(quickjs STATIC IMPORTED GLOBAL)
set_target_properties(quickjs PROPERTIES
    IMPORTED_LOCATION "${THIRD}/quickjs/libquickjs.a"
    INTERFACE_INCLUDE_DIRECTORIES "${THIRD}/quickjs")

# --------------------------------------------------------------------------
#  OpenSSL + cpp-httplib
# --------------------------------------------------------------------------
if(NOT DEFINED OPENSSL_ROOT_DIR AND EXISTS "D:/mingw64/opt")
    set(OPENSSL_ROOT_DIR "D:/mingw64/opt")
endif()
find_package(OpenSSL REQUIRED)

add_library(cpp_httplib STATIC "${THIRD}/cpp-httplib/httplib.cpp")
target_include_directories(cpp_httplib PUBLIC "${THIRD}/cpp-httplib")
target_compile_definitions(cpp_httplib PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
target_compile_features(cpp_httplib PUBLIC cxx_std_17)
target_link_libraries(cpp_httplib PUBLIC OpenSSL::SSL OpenSSL::Crypto)
if(WIN32)
    target_link_libraries(cpp_httplib PUBLIC ws2_32 crypt32)
endif()

# --------------------------------------------------------------------------
#  ths_vToken 库（DLL，导出 C 符号）
# --------------------------------------------------------------------------
add_library(ths_vToken SHARED
    ths_vtoken.cpp
    ths_vToken_bc.c
)

target_include_directories(ths_vToken PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${THIRD}/quickjs")

target_compile_definitions(ths_vToken PRIVATE THS_VTOKEN_EXPORTS)

target_link_libraries(ths_vToken PRIVATE cpp_httplib quickjs)
target_link_libraries(ths_vToken PRIVATE OpenSSL::SSL OpenSSL::Crypto)
if(WIN32)
    target_link_libraries(ths_vToken PRIVATE ws2_32 crypt32)
endif()

if(MINGW)
    set_target_properties(ths_vToken PROPERTIES PREFIX "")
    target_compile_options(ths_vToken PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_link_options(ths_vToken PRIVATE -static-libgcc -static-libstdc++)
endif()

# 生成的 C 数组不需要导出
set_source_files_properties(ths_vToken_bc.c PROPERTIES COMPILE_FLAGS "-w")

# --------------------------------------------------------------------------
#  测试程序
# --------------------------------------------------------------------------
add_executable(test_ths_vtoken test_ths_vtoken.cpp)
target_link_libraries(test_ths_vtoken PRIVATE ths_vToken cpp_httplib)
target_compile_definitions(test_ths_vtoken PRIVATE THS_VTOKEN_DLL)

if(MINGW)
    target_compile_options(test_ths_vtoken PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_link_options(test_ths_vtoken PRIVATE -static-libgcc -static-libstdc++)
endif()

# --------------------------------------------------------------------------
#  运行期 DLL 拷贝
# --------------------------------------------------------------------------
if(WIN32)
    file(GLOB _openssl_dlls
        "${OPENSSL_ROOT_DIR}/bin/libssl-*.dll"
        "${OPENSSL_ROOT_DIR}/bin/libcrypto-*.dll")
    get_filename_component(_mingw_root "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_mingw_root "${_mingw_root}" DIRECTORY)
    set(_dlls ${_openssl_dlls})
    foreach(_d libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        if(EXISTS "${_mingw_root}/bin/${_d}")
            list(APPEND _dlls "${_mingw_root}/bin/${_d}")
        endif()
    endforeach()
    add_custom_command(TARGET test_ths_vtoken POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dlls} "$<TARGET_FILE_DIR:test_ths_vtoken>"
        COMMENT "Copying runtime DLLs")
endif()
