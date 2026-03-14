cmake_minimum_required(VERSION 3.20)
project(cookbook VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include(GNUInstallDirs)

# ---------- vendor: pasta (static, no tests) ----------

set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
set(PASTA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/pasta)
set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})
target_compile_definitions(pasta PUBLIC PASTA_STATIC)

# ---------- vendor: alforno (static, no tests, uses our pasta) ----------

set(BUILD_SHARED_LIBS_SAVED2 ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
set(ALF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "" FORCE)
add_subdirectory(vendor/alforno)
set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED2})
unset(FETCHCONTENT_FULLY_DISCONNECTED CACHE)
target_compile_definitions(alforno PUBLIC ALF_STATIC)

# ---------- vendor: sqlite ----------

add_library(sqlite3 STATIC vendor/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC vendor/sqlite)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION
)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(sqlite3 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

# ---------- vendor: libsodium (ISC) ----------

file(GLOB_RECURSE SODIUM_SOURCES vendor/libsodium/src/libsodium/*.c)
add_library(sodium STATIC ${SODIUM_SOURCES})
target_include_directories(sodium PUBLIC
    vendor/libsodium/src/libsodium/include
    vendor/libsodium/src/libsodium/include/sodium
)
target_include_directories(sodium PRIVATE
    vendor/libsodium/src/libsodium
)
target_compile_definitions(sodium PRIVATE
    SODIUM_STATIC=1
    CONFIGURED=1
    DEV_MODE=0
    HAVE_C_VARARRAYS=1
    HAVE_CATCHABLE_SEGV=1
    HAVE_INLINE_ASM=1
    HAVE_TI_MODE=1
    HAVE_PTHREAD=1
)
if(WIN32)
    target_compile_definitions(sodium PRIVATE HAVE_RAISE=1)
    target_link_libraries(sodium PUBLIC advapi32)
else()
    target_compile_definitions(sodium PRIVATE HAVE_POSIX_MEMALIGN=1 HAVE_MMAP=1 HAVE_MPROTECT=1 HAVE_MADVISE=1 HAVE_NANOSLEEP=1)
    find_package(Threads REQUIRED)
    target_link_libraries(sodium PUBLIC Threads::Threads)
endif()
target_compile_definitions(sodium PUBLIC SODIUM_STATIC=1)

# ---------- vendor: civetweb (MIT) ----------

add_library(civetweb STATIC vendor/civetweb/civetweb.c)
target_include_directories(civetweb PUBLIC vendor/civetweb)
target_compile_definitions(civetweb PRIVATE
    USE_IPV6=1
    NO_SSL=1
    NO_CGI=1
    NO_CACHING=1
)
if(WIN32)
    target_link_libraries(civetweb PUBLIC ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(civetweb PUBLIC Threads::Threads)
endif()

# ---------- optional: PostgreSQL (libpq) ----------

find_package(PostgreSQL QUIET)
if(PostgreSQL_FOUND)
    message(STATUS "PostgreSQL found: ${PostgreSQL_VERSION_STRING}")
    set(COOKBOOK_HAS_POSTGRES ON)
else()
    message(STATUS "PostgreSQL not found — building without PG backend")
    set(COOKBOOK_HAS_POSTGRES OFF)
endif()

# ---------- library ----------

file(GLOB COOKBOOK_SOURCES src/main/c/*.c)
# exclude main.c and cookbook_import.c — they are standalone executables
list(FILTER COOKBOOK_SOURCES EXCLUDE REGEX ".*main\\.c$")
list(FILTER COOKBOOK_SOURCES EXCLUDE REGEX ".*cookbook_import\\.c$")

# exclude the PG file that doesn't match current config
if(COOKBOOK_HAS_POSTGRES)
    list(FILTER COOKBOOK_SOURCES EXCLUDE REGEX ".*cookbook_db_postgres_stub\\.c$")
else()
    list(FILTER COOKBOOK_SOURCES EXCLUDE REGEX ".*cookbook_db_postgres\\.c$")
endif()

file(GLOB COOKBOOK_HEADERS src/main/h/*.h)

add_library(cookbook STATIC ${COOKBOOK_SOURCES})

target_include_directories(cookbook
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/main/h>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_definitions(cookbook PUBLIC COOKBOOK_STATIC)
target_compile_definitions(cookbook PRIVATE BUILDING_COOKBOOK)
target_link_libraries(cookbook PUBLIC sqlite3 civetweb pasta alforno sodium)

if(COOKBOOK_HAS_POSTGRES)
    target_compile_definitions(cookbook PRIVATE COOKBOOK_HAS_POSTGRES)
    target_link_libraries(cookbook PRIVATE PostgreSQL::PostgreSQL)
endif()

# S3 store needs ws2_32 on Windows for sockets
if(WIN32)
    target_link_libraries(cookbook PRIVATE ws2_32)
endif()

# ---------- server executable ----------

add_executable(cookbook_server src/main/c/main.c)
target_link_libraries(cookbook_server PRIVATE cookbook)

# ---------- import tool (§A.4) ----------

add_executable(cookbook_import src/main/c/cookbook_import.c)
if(WIN32)
    target_link_libraries(cookbook_import PRIVATE ws2_32)
endif()

# ---------- test ----------

enable_testing()

file(GLOB COOKBOOK_TEST_SOURCES src/test/c/*.c)
# exclude stress test from unit test binary
list(FILTER COOKBOOK_TEST_SOURCES EXCLUDE REGEX ".*cookbook_stress\\.c$")

add_executable(cookbook_test ${COOKBOOK_TEST_SOURCES})
target_link_libraries(cookbook_test PRIVATE cookbook)
if(WIN32)
    target_link_options(cookbook_test PRIVATE -Wl,--stack,8388608)
endif()

target_compile_definitions(cookbook_test PRIVATE
    COOKBOOK_TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/src/test/resources"
)

# ---------- stress test ----------

add_executable(cookbook_stress src/test/c/cookbook_stress.c)
target_link_libraries(cookbook_stress PRIVATE cookbook)
if(WIN32)
    target_link_libraries(cookbook_stress PRIVATE ws2_32)
endif()

if(WIN32)
    set_target_properties(cookbook cookbook_server cookbook_test cookbook_stress PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

add_test(NAME cookbook_test COMMAND cookbook_test)

# ---------- install ----------

install(TARGETS cookbook cookbook_server cookbook_import
    EXPORT cookbookTargets
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES ${COOKBOOK_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
