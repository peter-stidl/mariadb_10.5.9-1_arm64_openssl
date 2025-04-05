# Install script for directory: https://github.com/peter-stidl/mariadb_10.5.9-1_arm64_openssl/mariadb-10.5.9-1-arm64-openssl

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "RelWithDebInfo")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Base Dir
set(MY_MARIADB_PATH ${CMAKE_SOURCE_DIR}/mariadb-10.5.9-1-arm64-openssl)

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xReadmex" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/mariadb-server-10.5" TYPE FILE FILES
    "${MY_MARIADB_PATH}/README.md"
    "${MY_MARIADB_PATH}/CREDITS"
    "${MY_MARIADB_PATH}/COPYING"
    "${MY_MARIADB_PATH}/THIRDPARTY"
    "${MY_MARIADB_PATH}/EXCEPTIONS-CLIENT"
  )
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xReadmex" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/mariadb-server-10.5" TYPE FILE FILES
    "${MY_MARIADB_PATH}/Docs/INSTALL-BINARY"
    "${MY_MARIADB_PATH}/Docs/README-wsrep"
  )
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
include("${MY_MARIADB_PATH}/wsrep-lib/cmake_install.cmake")
include("${MY_MARIADB_PATH}/unittest/mytap/cmake_install.cmake")
include("${MY_MARIADB_PATH}/unittest/strings/cmake_install.cmake")
include("${MY_MARIADB_PATH}/unittest/examples/cmake_install.cmake")
include("${MY_MARIADB_PATH}/unittest/mysys/cmake_install.cmake")
include("${MY_MARIADB_PATH}/unittest/my_decimal/cmake_install.cmake")
include("${MY_MARIADB_PATH}/unittest/json_lib/cmake_install.cmake")
include("${MY_MARIADB_PATH}/unittest/sql/cmake_install.cmake")
include("${MY_MARIADB_PATH}/libmariadb/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/archive/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/blackhole/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/cassandra/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/columnstore/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/connect/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/csv/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/example/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/federated/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/federatedx/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/heap/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/innobase/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/maria/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/mroonga/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/myisam/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/myisammrg/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/oqgraph/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/perfschema/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/rocksdb/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/sequence/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/sphinx/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/spider/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/test_sql_discovery/cmake_install.cmake")
include("${MY_MARIADB_PATH}/storage/tokudb/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/audit_null/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/auth_dialog/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/auth_ed25519/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/auth_examples/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/auth_gssapi/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/auth_pam/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/auth_pipe/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/auth_socket/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/aws_key_management/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/cracklib_password_check/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/daemon_example/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/debug_key_management/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/disks/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/example_key_management/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/feedback/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/file_key_management/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/fulltext/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/func_test/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/handler_socket/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/locale_info/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/metadata_lock_info/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/qc_info/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/query_response_time/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/server_audit/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/simple_password_check/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/sql_errlog/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/test_sql_service/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/type_geom/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/type_inet/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/type_mysql_json/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/type_test/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/user_variables/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/userstat/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/versioning/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/win_auth_client/cmake_install.cmake")
include("${MY_MARIADB_PATH}/plugin/wsrep_info/cmake_install.cmake")
include("${MY_MARIADB_PATH}/include/cmake_install.cmake")
include("${MY_MARIADB_PATH}/dbug/cmake_install.cmake")
include("${MY_MARIADB_PATH}/strings/cmake_install.cmake")
include("${MY_MARIADB_PATH}/vio/cmake_install.cmake")
include("${MY_MARIADB_PATH}/mysys/cmake_install.cmake")
include("${MY_MARIADB_PATH}/mysys_ssl/cmake_install.cmake")
include("${MY_MARIADB_PATH}/client/cmake_install.cmake")
include("${MY_MARIADB_PATH}/extra/cmake_install.cmake")
include("${MY_MARIADB_PATH}/libservices/cmake_install.cmake")
include("${MY_MARIADB_PATH}/sql/share/cmake_install.cmake")
include("${MY_MARIADB_PATH}/tpool/cmake_install.cmake")
include("${MY_MARIADB_PATH}/tests/cmake_install.cmake")
include("${MY_MARIADB_PATH}/sql/cmake_install.cmake")
include("${MY_MARIADB_PATH}/mysql-test/cmake_install.cmake")
include("${MY_MARIADB_PATH}/mysql-test/lib/My/SafeProcess/cmake_install.cmake")
include("${MY_MARIADB_PATH}/sql-bench/cmake_install.cmake")
include("${MY_MARIADB_PATH}/man/cmake_install.cmake")
include("${MY_MARIADB_PATH}/scripts/cmake_install.cmake")
include("${MY_MARIADB_PATH}/support-files/cmake_install.cmake")
include("${MY_MARIADB_PATH}/extra/aws_sdk/cmake_install.cmake")

endif()

if(CMAKE_INSTALL_COMPONENT)
  set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
file(WRITE "${MY_MARIADB_PATH}/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")

