cmake_minimum_required(VERSION 3.20)
project(cookbook VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include(GNUInstallDirs)

# ---------- vendor: apennines (TLS 1.3 building blocks + pasta/alforno) ----------
# Crypto/encoding/math modules from the apennines project.
# Used for: AES-GCM, ChaCha20-Poly1305, X25519, ECDH P-256,
# RSA verify, X.509 cert parsing, HKDF, ASN.1/DER, PEM, bigint.

add_library(apennines STATIC
    vendor/apennines/sources/t1/buffer/buf.c
    vendor/apennines/sources/t1/buffer/utf8.c
    vendor/apennines/sources/t1/random/entropy.c
    vendor/apennines/sources/t1/sync/mutex/mutex.c
    vendor/apennines/sources/t1/sync/thread/thread.c
    vendor/apennines/sources/t2/random/rng.c
    vendor/apennines/sources/t2/crypto/argon2.c
    vendor/apennines/sources/t2/crypto/blake2b.c
    vendor/apennines/sources/t2/crypto/cipher.c
    vendor/apennines/sources/t2/crypto/ct.c
    vendor/apennines/sources/t2/crypto/ec.c
    vendor/apennines/sources/t2/crypto/ecdsa.c
    vendor/apennines/sources/t2/crypto/hash.c
    vendor/apennines/sources/t2/crypto/rsa.c
    vendor/apennines/sources/t2/crypto/secret.c
    vendor/apennines/sources/t2/crypto/x509.c
    vendor/apennines/sources/t2/encoding/asn1_der.c
    vendor/apennines/sources/t2/encoding/base.c
    vendor/apennines/sources/t2/encoding/pem.c
    vendor/apennines/sources/t2/math/bigint/bigint.c
    vendor/apennines/sources/t2/compress/compress.c
    vendor/apennines/sources/t2/net/addr.c
    vendor/apennines/sources/t2/string/fmt.c
    vendor/apennines/sources/t2/string/str.c
    vendor/apennines/sources/t3/async/threadpool.c
    vendor/apennines/sources/t3/crypto/pki.c
    vendor/apennines/sources/t3/db/kv.c
    vendor/apennines/sources/t3/db/sql.c
    vendor/apennines/sources/t3/db/wal.c
    vendor/apennines/sources/t4/db/database.c
    vendor/apennines/sources/t3/net/http.c
    vendor/apennines/sources/t3/net/tcp.c
    vendor/apennines/sources/t3/net/dns.c
    vendor/apennines/sources/t3/net/h2.c
    vendor/apennines/sources/t3/net/tls.c
    vendor/apennines/sources/t4/net/http_client.c
    vendor/apennines/sources/t4/net/https_client.c
    vendor/apennines/sources/t4/net/http_server.c
    vendor/apennines/sources/t4/pasta/pasta_parser.c
    vendor/apennines/sources/t4/pasta/pasta_writer.c
    vendor/apennines/sources/t4/pasta/pasta_value.c
    vendor/apennines/sources/t4/pasta/alforno.c
)
target_include_directories(apennines
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/apennines/headers>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor/apennines/sources/t4/pasta
)
target_compile_definitions(apennines PUBLIC APENNINES_STATIC)
target_compile_definitions(apennines PRIVATE APENNINES_BUILDING)
if(WIN32)
    # bcrypt: apennines/t1/random/entropy calls BCryptGenRandom.
    target_link_libraries(apennines PUBLIC ws2_32 bcrypt)
endif()

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
target_link_libraries(cookbook PUBLIC sqlite3 apennines)
# alforno_compat.c needs pasta_internal.h for in-place include stripping.
target_include_directories(cookbook PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/apennines/sources/t4/pasta
)

if(COOKBOOK_HAS_POSTGRES)
    target_compile_definitions(cookbook PRIVATE COOKBOOK_HAS_POSTGRES)
    target_link_libraries(cookbook PRIVATE PostgreSQL::PostgreSQL)
endif()

# S3 store + socket layer still call Winsock directly on Windows.
# (bcrypt now pulled in transitively via apennines.)
if(WIN32)
    target_link_libraries(cookbook PRIVATE ws2_32)
endif()

# ---------- server executable ----------

add_executable(cookbook_server src/main/c/main.c)
target_link_libraries(cookbook_server PRIVATE cookbook)

# Fully static build option — eliminates MinGW DLL dependencies.
# Enable with: cmake --preset default -DCOOKBOOK_STATIC_LINK=ON
option(COOKBOOK_STATIC_LINK "Link executables fully static (no DLLs)" OFF)
if(COOKBOOK_STATIC_LINK AND WIN32)
    target_link_options(cookbook_server PRIVATE -static)
endif()

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
