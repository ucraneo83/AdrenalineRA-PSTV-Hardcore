# Select one input tree for both the embedded and loose PSP firmware files.
file(REAL_PATH "${CMAKE_CURRENT_LIST_DIR}/../user/flash0" standard_flash0_dir)
if(ADRENALINE_PROFILE STREQUAL "test")
  if(NOT IS_DIRECTORY "${ADRENALINE_TEST_FLASH0_DIR}")
    message(FATAL_ERROR "Set ADRENALINE_TEST_FLASH0_DIR to a staged CEF flash0 directory")
  endif()
  file(REAL_PATH "${ADRENALINE_TEST_FLASH0_DIR}" ADRENALINE_FLASH0_DIR)
  cmake_path(IS_PREFIX standard_flash0_dir "${ADRENALINE_FLASH0_DIR}" NORMALIZE in_standard_tree)
  if(in_standard_tree)
    message(FATAL_ERROR "Test requires a flash0 directory outside the standard tree")
  endif()

  # Require the same resource layout as the checked-in standard build.
  file(GLOB standard_resources RELATIVE "${standard_flash0_dir}" LIST_DIRECTORIES false
    "${standard_flash0_dir}/kd/*.prx"
    "${standard_flash0_dir}/kd/*.bin"
    "${standard_flash0_dir}/vsh/module/*.prx"
    "${standard_flash0_dir}/*.bin")
elseif(ADRENALINE_PROFILE STREQUAL "standard")
  set(ADRENALINE_FLASH0_DIR "${standard_flash0_dir}")
else()
  message(FATAL_ERROR "Unknown ADRENALINE_PROFILE: ${ADRENALINE_PROFILE}")
endif()

file(GLOB ADRENALINE_FLASH0_RESOURCES RELATIVE "${ADRENALINE_FLASH0_DIR}" LIST_DIRECTORIES false
  "${ADRENALINE_FLASH0_DIR}/kd/*.prx"
  "${ADRENALINE_FLASH0_DIR}/kd/*.bin"
  "${ADRENALINE_FLASH0_DIR}/vsh/module/*.prx"
  "${ADRENALINE_FLASH0_DIR}/*.bin")

if(ADRENALINE_PROFILE STREQUAL "test")
  if(NOT "${ADRENALINE_FLASH0_RESOURCES}" STREQUAL "${standard_resources}")
    message(FATAL_ERROR "Staged Test flash0 directory has missing or unexpected resources")
  endif()
  foreach(resource IN LISTS ADRENALINE_FLASH0_RESOURCES)
    set(staged_file "${ADRENALINE_FLASH0_DIR}/${resource}")
    file(REAL_PATH "${staged_file}" resolved_file)
    cmake_path(IS_PREFIX ADRENALINE_FLASH0_DIR "${resolved_file}" NORMALIZE in_test_tree)
    if(NOT in_test_tree)
      message(FATAL_ERROR "Staged Test flash0 resource points outside its directory: ${resource}")
    endif()
    file(SIZE "${staged_file}" resource_size)
    if(resource_size EQUAL 0)
      message(FATAL_ERROR "Staged Test flash0 resource is empty: ${resource}")
    endif()
  endforeach()
endif()

if(CMAKE_SCRIPT_MODE_FILE)
  list(LENGTH ADRENALINE_FLASH0_RESOURCES resource_count)
  message(STATUS "Selected ${resource_count} flash0 resources from ${ADRENALINE_FLASH0_DIR}")
endif()
