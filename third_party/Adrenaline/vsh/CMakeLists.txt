cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(adrenaline_vsh)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -nostdlib")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fno-exceptions")

add_executable(adrenaline_vsh
  main.c
)

set_target_properties(adrenaline_vsh PROPERTIES
  C_STANDARD 17
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS YES
)

target_link_directories(adrenaline_vsh PRIVATE ${CMAKE_BINARY_DIR}/kernel)

target_link_libraries(adrenaline_vsh
  AdrenalineKernel${ADRENALINE_MODULE_SUFFIX}_stub
  taihen_stub
  SceLibKernel_stub
  SceAppMgr_stub
  SceKernelThreadMgr_stub
  SceLsdb_stub_weak
  SceIofilemgr_stub
  SceAVConfig_stub
)

vita_create_self(adrenaline_vsh.suprx adrenaline_vsh
  CONFIG "${ADRENALINE_IDENTITY_INCLUDE_DIR}/vsh-exports.yml" UNSAFE)

#add_custom_target(copy
#  COMMAND cp adrenaline_vsh.suprx F:/app/PSPEMUCFW/sce_module/adrenaline_vsh.suprx
#  DEPENDS adrenaline_vsh.suprx
#)

#add_custom_target(pkg
#  COMMAND cp adrenaline_vsh.suprx ../../bubble/pkg/sce_module/adrenaline_vsh.suprx
#  DEPENDS adrenaline_vsh.suprx
#)

add_custom_target(vsh_all
  ALL
  DEPENDS adrenaline_vsh.suprx-self
)
