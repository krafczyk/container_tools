cmake_minimum_required(VERSION 3.20)

project(container_tools VERSION 1.0.0 LANGUAGES C)

include(GNUInstallDirs)
include(CTest)
include(cmake/ContainerToolsWarnings.cmake)

set(CONTAINER_TOOLS_VENDOR_DIGESTS
  "vendor/tomlc17/tomlc17.c=f28f3742808505b5c55189b72cf62705e824276eca6533b36e8751a8433482aa"
  "vendor/tomlc17/tomlc17.h=eef9a891b93fc6235a9552db16286cefc684be8fae49018bedf1c6ab9ca55b87"
  "vendor/tomlc17/LICENSE=e6eae7966fa001217173768e984f758fd49cef45afb2e8f471e947a9712f06c9"
  "vendor/yyjson/yyjson.c=ac2e9bbb2e2d9149d90878d40506a1d624fa0b33c979a11b61075c54782c6d6a"
  "vendor/yyjson/yyjson.h=175867c5493a5df648cec566717fa1c29aa2f6096f5f0cf1efad0b65e1f6d7b3"
  "vendor/yyjson/LICENSE=45e384d3d52c73cba3a64d6e6c25d47cd738cd8a55c30629e3201046eda62947"
)
foreach(vendor_digest IN LISTS CONTAINER_TOOLS_VENDOR_DIGESTS)
  string(REPLACE "=" ";" vendor_digest_parts "${vendor_digest}")
  list(GET vendor_digest_parts 0 vendor_file)
  list(GET vendor_digest_parts 1 vendor_expected_digest)
  file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${vendor_file}" vendor_actual_digest)
  if(NOT vendor_actual_digest STREQUAL vendor_expected_digest)
    message(FATAL_ERROR "vendored provenance drift: ${vendor_file}")
  endif()
endforeach()

set(CONTAINER_TOOLS_PRODUCT_VERSION "1.0.0-dev" CACHE STRING
    "Immutable container-tools product version")
set(CONTAINER_TOOLS_PACKAGE_EPOCH "1" CACHE STRING
    "Immutable container-tools package epoch")
if(NOT CONTAINER_TOOLS_PRODUCT_VERSION MATCHES
   "^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?$")
  message(FATAL_ERROR "container-tools requires a semantic product version")
endif()
if(NOT CONTAINER_TOOLS_PACKAGE_EPOCH MATCHES "^[0-9]+$")
  message(FATAL_ERROR "container-tools requires a numeric package epoch")
endif()
option(CONTAINER_TOOLS_STATIC "Link the package executable statically" ON)
option(CONTAINER_TOOLS_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(CONTAINER_TOOLS_STATIC AND CONTAINER_TOOLS_SANITIZERS)
  message(FATAL_ERROR "sanitizer builds must use CONTAINER_TOOLS_STATIC=OFF")
endif()
if(CONTAINER_TOOLS_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

execute_process(
  COMMAND git rev-parse HEAD
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  OUTPUT_VARIABLE CONTAINER_TOOLS_SOURCE_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE CONTAINER_TOOLS_GIT_RESULT
)
string(LENGTH "${CONTAINER_TOOLS_SOURCE_COMMIT}" CONTAINER_TOOLS_SOURCE_COMMIT_LENGTH)
if(NOT CONTAINER_TOOLS_GIT_RESULT EQUAL 0 OR
   NOT CONTAINER_TOOLS_SOURCE_COMMIT MATCHES "^[0-9a-f]+$" OR
   NOT CONTAINER_TOOLS_SOURCE_COMMIT_LENGTH EQUAL 40)
  message(FATAL_ERROR "container-tools requires an exact 40-hex source commit")
endif()

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" CONTAINER_TOOLS_ARCHITECTURE)
if(NOT CONTAINER_TOOLS_ARCHITECTURE MATCHES "^[a-z0-9_.-]+$")
  message(FATAL_ERROR "container-tools requires a non-empty portable target architecture")
endif()

function(container_tools_component_digest output)
  set(contents "")
  foreach(source IN LISTS ARGN)
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${source}" source_digest)
    string(APPEND contents "${source}:${source_digest}\n")
  endforeach()
  string(SHA256 result "${contents}")
  set(${output} "${result}" PARENT_SCOPE)
endfunction()

set(CONTAINER_TOOLS_NESTED_FILES
  src/backend_nested.c src/backend_nested.h
  src/backend_bubblewrap.c src/backend_bubblewrap.h
  src/backend_proot.c src/backend_proot.h
  src/backend_rewrite.c src/backend_rewrite.h
  src/trampoline.c src/trampoline.h
  src/doctor.c src/doctor.h)

container_tools_component_digest(CONTAINER_TOOLS_EXECUTABLE_DIGEST
  src/main.c src/cli.c src/package.c src/config.c src/config.h src/storage.c src/storage.h
  src/storage_timeout.c src/storage_timeout.h src/mount.c src/mount.h src/buildx.c src/buildx.h
  src/process.c src/process.h src/runtime.c src/runtime.h src/backend_outer.c src/backend_outer.h
  src/host_projection.c src/host_projection.h src/mount_plan.c src/mount_plan.h src/sha256.c src/sha256.h
  src/profile.c src/profile.h src/state.c src/state.h src/instance.c src/instance.h
  src/host_config.c src/host_config.h src/path_map.c src/path_map.h src/executable.c src/executable.h
  src/elf.c src/elf.h src/shebang.c src/shebang.h src/supervisor.c src/supervisor.h
  src/supervisor_process.c src/supervisor_process.h ${CONTAINER_TOOLS_NESTED_FILES}
  vendor/tomlc17/tomlc17.c vendor/tomlc17/tomlc17.h
  vendor/yyjson/yyjson.c vendor/yyjson/yyjson.h)
container_tools_component_digest(CONTAINER_TOOLS_SCRIPTS_DIGEST cmake/compat.sh.in)
container_tools_component_digest(CONTAINER_TOOLS_METADATA_DIGEST cmake/release.json.in)
container_tools_component_digest(CONTAINER_TOOLS_DOCUMENTATION_DIGEST README.md examples/README.md examples/host.toml docs/host-execution.md)
container_tools_component_digest(CONTAINER_TOOLS_LICENSES_DIGEST
  LICENSE-APACHE LICENSE-MIT vendor/tomlc17/LICENSE vendor/yyjson/LICENSE)
container_tools_component_digest(CONTAINER_TOOLS_PROTOCOLS_DIGEST protocols/ct-mount-plan-v1.md)
string(SHA256 CONTAINER_TOOLS_BUILD_IDENTITY
  "${CONTAINER_TOOLS_PRODUCT_VERSION}:${PROJECT_VERSION_MAJOR}:${CONTAINER_TOOLS_PACKAGE_EPOCH}:${CONTAINER_TOOLS_ARCHITECTURE}:${CONTAINER_TOOLS_SOURCE_COMMIT}:${CONTAINER_TOOLS_EXECUTABLE_DIGEST}:${CONTAINER_TOOLS_SCRIPTS_DIGEST}")

configure_file(src/package_identity.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/package_identity.h" @ONLY)
configure_file(cmake/release.json.in "${CMAKE_CURRENT_BINARY_DIR}/release.json" @ONLY)
foreach(script ct_exec.sh ct_shell.sh ct_instance_exec.sh ct_mount_detector.sh ct_args.sh)
  set(CONTAINER_TOOLS_COMPAT_SCRIPT "${script}")
  configure_file(cmake/compat.sh.in "${CMAKE_CURRENT_BINARY_DIR}/${script}" @ONLY)
endforeach()

add_library(container_tools_toml STATIC vendor/tomlc17/tomlc17.c)
target_include_directories(container_tools_toml PRIVATE vendor/tomlc17)

add_library(container_tools_json STATIC vendor/yyjson/yyjson.c)
target_include_directories(container_tools_json PRIVATE vendor/yyjson)

set(CONTAINER_TOOLS_CORE_SOURCES
  src/cli.c src/package.c src/config.c src/storage.c src/storage_timeout.c src/mount.c src/buildx.c
  src/process.c src/runtime.c src/backend_outer.c src/host_projection.c src/mount_plan.c src/sha256.c
  src/profile.c src/state.c src/instance.c src/host_config.c src/path_map.c src/executable.c src/elf.c
  src/shebang.c src/supervisor.c src/supervisor_process.c ${CONTAINER_TOOLS_NESTED_FILES})
add_library(container_tools_core STATIC ${CONTAINER_TOOLS_CORE_SOURCES})
target_include_directories(container_tools_core PRIVATE vendor/tomlc17 vendor/yyjson "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(container_tools_core PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(container_tools_core PRIVATE container_tools_toml container_tools_json)
if(NOT CONTAINER_TOOLS_STATIC)
  # Dynamic packages exist only to exercise deterministic CTest integrations on
  # hosts without a static libc; production packages must retain static linking.
  target_compile_definitions(container_tools_core PRIVATE CT_PACKAGE_TEST_DYNAMIC=1)
endif()
container_tools_enable_warnings(container_tools_core)

add_executable(container-tools src/main.c)
target_link_libraries(container-tools PRIVATE container_tools_core)
target_include_directories(container-tools PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
container_tools_enable_warnings(container-tools)
if(CONTAINER_TOOLS_STATIC)
  target_link_options(container-tools PRIVATE -static)
endif()

if(BUILD_TESTING)
  # The forced-timeout seam is isolated from the production executable.
  add_library(container_tools_test_core STATIC ${CONTAINER_TOOLS_CORE_SOURCES})
  target_include_directories(container_tools_test_core PRIVATE vendor/tomlc17 vendor/yyjson "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_compile_definitions(container_tools_test_core PRIVATE
    _POSIX_C_SOURCE=200809L CT_STORAGE_TIMEOUT_TEST_SEAM=1
    CT_SUPERVISOR_TEST_SEAM=1 CT_TRAMPOLINE_TEST_SEAM=1)
  target_link_libraries(container_tools_test_core PRIVATE container_tools_toml container_tools_json)
  container_tools_enable_warnings(container_tools_test_core)

  add_executable(test_storage_timeout_child tests/test_storage_timeout_child.c)
  target_link_libraries(test_storage_timeout_child PRIVATE container_tools_test_core)
  target_include_directories(test_storage_timeout_child PRIVATE src)
  container_tools_enable_warnings(test_storage_timeout_child)
endif()

install(TARGETS container-tools RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(PROGRAMS
  "${CMAKE_CURRENT_BINARY_DIR}/ct_exec.sh"
  "${CMAKE_CURRENT_BINARY_DIR}/ct_shell.sh"
  "${CMAKE_CURRENT_BINARY_DIR}/ct_instance_exec.sh"
  "${CMAKE_CURRENT_BINARY_DIR}/ct_mount_detector.sh"
  "${CMAKE_CURRENT_BINARY_DIR}/ct_args.sh"
  DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/release.json" README.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/container-tools")
install(DIRECTORY examples/ DESTINATION "${CMAKE_INSTALL_DATADIR}/container-tools/examples")
install(DIRECTORY protocols/ DESTINATION "${CMAKE_INSTALL_DATADIR}/container-tools/protocols")
install(FILES docs/host-execution.md DESTINATION "${CMAKE_INSTALL_DATADIR}/container-tools/docs")
install(FILES LICENSE-APACHE LICENSE-MIT
  DESTINATION "${CMAKE_INSTALL_DATADIR}/container-tools/licenses")
install(FILES vendor/tomlc17/LICENSE
  DESTINATION "${CMAKE_INSTALL_DATADIR}/container-tools/licenses" RENAME tomlc17-LICENSE)
install(FILES vendor/yyjson/LICENSE
  DESTINATION "${CMAKE_INSTALL_DATADIR}/container-tools/licenses" RENAME yyjson-LICENSE)

if(BUILD_TESTING)
  add_executable(test_cli tests/test_cli.c)
  target_link_libraries(test_cli PRIVATE container_tools_test_core)
  target_include_directories(test_cli PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
  container_tools_enable_warnings(test_cli)
  add_test(NAME container_tools_cli COMMAND test_cli)

  add_executable(test_package tests/test_package.c)
  target_link_libraries(test_package PRIVATE container_tools_test_core)
  target_include_directories(test_package PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
  container_tools_enable_warnings(test_package)
  add_test(NAME container_tools_package COMMAND test_package)

  foreach(test_name config storage storage_timeout mount buildx runtime host_projection mount_plan process profile instance host_config path_map executable supervisor trampoline backend_nested bubblewrap proot rewrite doctor host_use_cases)
    add_executable(test_${test_name} tests/test_${test_name}.c)
    target_link_libraries(test_${test_name} PRIVATE container_tools_test_core)
    target_include_directories(test_${test_name} PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
    container_tools_enable_warnings(test_${test_name})
    add_test(NAME container_tools_${test_name} COMMAND test_${test_name})
  endforeach()
  target_include_directories(test_doctor PRIVATE vendor/yyjson)
  target_compile_definitions(test_backend_nested PRIVATE CT_TRAMPOLINE_TEST_SEAM=1)

  configure_file(tests/test_compatibility.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/test_compatibility.cmake" @ONLY)
  add_test(NAME container_tools_compatibility COMMAND "${CMAKE_COMMAND}" -P
    "${CMAKE_CURRENT_BINARY_DIR}/test_compatibility.cmake")

  add_executable(test_mount_sort tests/test_mount_sort.c)
  target_include_directories(test_mount_sort PRIVATE src)
  container_tools_enable_warnings(test_mount_sort)
  add_test(NAME container_tools_mount_sort COMMAND test_mount_sort)

  add_executable(test_runtime_child tests/test_runtime_child.c)
  target_compile_definitions(test_runtime_child PRIVATE _POSIX_C_SOURCE=200809L)
  container_tools_enable_warnings(test_runtime_child)
  add_executable(test_buildx_child tests/test_buildx_child.c)
  target_compile_definitions(test_buildx_child PRIVATE _POSIX_C_SOURCE=200809L)
  container_tools_enable_warnings(test_buildx_child)

  if(CONTAINER_TOOLS_STATIC)
    add_executable(test_executable_static tests/test_executable.c)
    target_link_libraries(test_executable_static PRIVATE container_tools_test_core)
    target_include_directories(test_executable_static PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_options(test_executable_static PRIVATE -static)
    container_tools_enable_warnings(test_executable_static)
    add_test(NAME container_tools_executable_static COMMAND test_executable_static)

    add_executable(test_trampoline_static tests/test_trampoline.c)
    target_link_libraries(test_trampoline_static PRIVATE container_tools_test_core)
    target_include_directories(test_trampoline_static PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_options(test_trampoline_static PRIVATE -static)
    container_tools_enable_warnings(test_trampoline_static)
    add_test(NAME container_tools_trampoline_static COMMAND test_trampoline_static)

    # This target links the production trampoline without its test-only
    # admission bypass, so fake nested backends exercise static descriptor entry.
    add_executable(test_backend_nested_static tests/test_backend_nested.c)
    target_link_libraries(test_backend_nested_static PRIVATE container_tools_core)
    target_include_directories(test_backend_nested_static PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_options(test_backend_nested_static PRIVATE -static)
    container_tools_enable_warnings(test_backend_nested_static)
    add_test(NAME container_tools_backend_nested_static COMMAND test_backend_nested_static)

    configure_file(tests/test_install.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/test_install.cmake" @ONLY)
    add_test(NAME container_tools_install COMMAND "${CMAKE_COMMAND}" -P
      "${CMAKE_CURRENT_BINARY_DIR}/test_install.cmake")
    configure_file(tests/test_runtime_config.cmake.in
      "${CMAKE_CURRENT_BINARY_DIR}/test_runtime_config.cmake" @ONLY)
    add_test(NAME container_tools_runtime_config_integration COMMAND "${CMAKE_COMMAND}" -P
      "${CMAKE_CURRENT_BINARY_DIR}/test_runtime_config.cmake")
    configure_file(tests/test_foreground.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/test_foreground.cmake" @ONLY)
    add_test(NAME container_tools_foreground_integration COMMAND "${CMAKE_COMMAND}" -P
      "${CMAKE_CURRENT_BINARY_DIR}/test_foreground.cmake")
    configure_file(tests/test_native_projection.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/test_native_projection.cmake" @ONLY)
    add_test(NAME container_tools_native_projection_integration COMMAND "${CMAKE_COMMAND}" -P
      "${CMAKE_CURRENT_BINARY_DIR}/test_native_projection.cmake")
  endif()
  configure_file(tests/test_native_instance.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/test_native_instance.cmake" @ONLY)
  add_test(NAME container_tools_native_instance_integration
    COMMAND "${CMAKE_COMMAND}" -P
            "${CMAKE_CURRENT_BINARY_DIR}/test_native_instance.cmake")
endif()
