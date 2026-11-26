cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(adrenaline)
include("${VITASDK}/share/vita.cmake" REQUIRED)
set(VITA_VERSION  "01.01")
if(ADRENALINE_TEST_REPLACEMENT)
  set(VITA_VERSION "01.06")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fno-exceptions")
set(VITA_MAKE_FSELF_FLAGS "${VITA_MAKE_FSELF_FLAGS} -a 0x2800000000000001")

add_executable(adrenaline
  main.c
  utils.c
  scr_printf.c
  font.c
)

target_link_libraries(adrenaline
  taihen_stub
  SceAppMgr_stub
  SceCtrl_stub
  SceDisplay_stub
  SceHttp_stub
  SceNet_stub
  SceNetCtl_stub
  SceSysmodule_stub
  ScePgf_stub_weak
)

set(livearea_dir "${CMAKE_CURRENT_SOURCE_DIR}/pkg")
if(ADRENALINE_TEST_PROFILE)
  set(source_livearea_dir "${livearea_dir}")
  set(livearea_dir "${CMAKE_CURRENT_BINARY_DIR}/livearea")
  file(COPY "${source_livearea_dir}/sce_sys" DESTINATION "${livearea_dir}"
       PATTERN "template.xml" EXCLUDE)

  foreach(xml IN ITEMS frame.xml sce_sys/livearea/contents/template.xml)
    set(source_xml "${source_livearea_dir}/${xml}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source_xml}")
    file(READ "${source_xml}" content)
    string(REGEX REPLACE ">Adrenaline [0-9]+\\.[0-9]+\\.[0-9]+</str>"
           ">${ADRENALINE_APP_NAME}</str>" test_content "${content}")
    if(test_content STREQUAL content)
      message(FATAL_ERROR "Could not set Test LiveArea title in ${source_xml}")
    endif()
    file(CONFIGURE OUTPUT "${livearea_dir}/${xml}" CONTENT "${test_content}" @ONLY NEWLINE_STYLE LF)
  endforeach()
endif()

vita_create_self(eboot.bin adrenaline UNSAFE)
vita_create_vpk(${ADRENALINE_VPK_NAME} ${ADRENALINE_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME "${ADRENALINE_APP_NAME}"
  FILE "${livearea_dir}/sce_sys" sce_sys
       pkg/eboot.pbp eboot.pbp
       "${livearea_dir}/frame.xml" frame.xml
       ${CMAKE_BINARY_DIR}/kernel/adrenaline_kernel.skprx sce_module/adrenaline_kernel.skprx
       ${CMAKE_BINARY_DIR}/user/adrenaline_user.suprx sce_module/adrenaline_user.suprx
       ${CMAKE_BINARY_DIR}/vsh/adrenaline_vsh.suprx sce_module/adrenaline_vsh.suprx
       ${CMAKE_BINARY_DIR}/usbdevice/usbdevice.skprx sce_module/usbdevice.skprx
       "${ADRENALINE_FLASH0_DIR}/kd/pentazemin.prx" pentazemin.prx
       "${ADRENALINE_FLASH0_DIR}/kd/systemctrl.prx" systemctrl.prx
       ${CMAKE_SOURCE_DIR}/../../assets/ra_sounds/message.wav ra_sounds/message.wav
       ${CMAKE_SOURCE_DIR}/../../assets/ra_sounds/unlock.wav ra_sounds/unlock.wav
       ${CMAKE_SOURCE_DIR}/../../assets/ra_sounds/lbsubmit.wav ra_sounds/lbsubmit.wav
)
