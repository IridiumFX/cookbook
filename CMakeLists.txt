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

# ---------- library ----------

file(GLOB COOKBOOK_SOURCES src/main/c/*.c)
# exclude main.c from the library — it belongs to the server executable
list(FILTER COOKBOOK_SOURCES EXCLUDE REGEX ".*main\\.c$")

file(GLOB COOKBOOK_HEADERS src/main/h/*.h)

add_library(cookbook ${COOKBOOK_SOURCES})

target_include_directories(cookbook
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/main/h>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_definitions(cookbook PRIVATE BUILDING_COOKBOOK)
target_link_libraries(cookbook PRIVATE sqlite3 civetweb pasta)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(cookbook PUBLIC COOKBOOK_STATIC)
endif()

# ---------- server executable ----------

add_executable(cookbook_server src/main/c/main.c)
target_link_libraries(cookbook_server PRIVATE cookbook sqlite3 civetweb pasta)

# ---------- test ----------

enable_testing()

file(GLOB COOKBOOK_TEST_SOURCES src/test/c/*.c)

add_executable(cookbook_test ${COOKBOOK_TEST_SOURCES})
target_link_libraries(cookbook_test PRIVATE cookbook sqlite3 civetweb pasta)

target_compile_definitions(cookbook_test PRIVATE
    COOKBOOK_TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/src/test/resources"
)

if(WIN32)
    set_target_properties(cookbook cookbook_server cookbook_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

add_test(NAME cookbook_test COMMAND cookbook_test)

# ---------- install ----------

install(TARGETS cookbook cookbook_server
    EXPORT cookbookTargets
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES ${COOKBOOK_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
