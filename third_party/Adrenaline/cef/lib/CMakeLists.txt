set(CEF_PREBUILT_LIB_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE PATH
    "Directory containing the Adrenaline CEF import libraries")
if(NOT IS_DIRECTORY "${CEF_PREBUILT_LIB_DIR}")
  message(FATAL_ERROR "CEF import library directory does not exist: ${CEF_PREBUILT_LIB_DIR}")
endif()
file(GLOB PREBUILT_LIBS CONFIGURE_DEPENDS "${CEF_PREBUILT_LIB_DIR}/*.a")

foreach(FILE IN LISTS PREBUILT_LIBS)
  get_filename_component(LIB_FILENAME "${FILE}" NAME_WE)
  string(REGEX REPLACE "^lib" "" LIB "${LIB_FILENAME}")

  add_library(${LIB} STATIC IMPORTED GLOBAL)

  target_include_directories(${LIB} INTERFACE ${PROJECT_SOURCE_DIR}/include)

  set_target_properties(${LIB} PROPERTIES
    IMPORTED_LOCATION ${FILE}
  )
endforeach()
