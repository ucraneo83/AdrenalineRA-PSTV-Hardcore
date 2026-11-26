cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(adrenaline_kernel)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -nostdlib")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fno-exceptions")

add_executable(adrenaline_kernel
  main.c
  utils.c
)

set_target_properties(adrenaline_kernel PROPERTIES
  C_STANDARD 17
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS YES
)

target_link_libraries(adrenaline_kernel
  taihenForKernel_stub
  taihenModuleUtils_stub
  SceCpuForDriver_stub
  SceCtrlForDriver_stub
  SceIofilemgrForDriver_stub
  SceModulemgrForDriver_stub
  SceProcessmgrForDriver_stub
  SceSysclibForDriver_stub
  SceSysmemForDriver_stub
  SceSysrootForKernel_stub
  SceSysrootForDriver_stub
  SceThreadmgrForDriver_stub
  SceDebugForDriver_stub
)

set(KERNEL_EXPORTS "${ADRENALINE_IDENTITY_INCLUDE_DIR}/kernel-exports.yml")
vita_create_self(adrenaline_kernel.skprx adrenaline_kernel CONFIG "${KERNEL_EXPORTS}" UNSAFE)

vita_create_stubs(kstubs adrenaline_kernel "${KERNEL_EXPORTS}" KERNEL)

#install(DIRECTORY ${CMAKE_BINARY_DIR}/stubs/
#  DESTINATION lib
#  FILES_MATCHING PATTERN "*.a"
#)


#add_custom_target(copy
#  COMMAND cp adrenaline_kernel.skprx H:/app/PSPEMUCFW/sce_module/adrenaline_kernel.skprx
#  DEPENDS adrenaline_kernel.skprx
#)

#add_custom_target(pkg
#  COMMAND cp adrenaline_kernel.skprx ../../bubble/pkg/sce_module/adrenaline_kernel.skprx
#  DEPENDS adrenaline_kernel.skprx
#)

#add_custom_target(updater
#  COMMAND bin2c adrenaline_kernel.skprx ../../cef/updater/adrenaline_kernel.h adrenaline_kernel
#  DEPENDS adrenaline_kernel.skprx
#)

add_custom_target(kernel_all
  ALL
  DEPENDS adrenaline_kernel.skprx-self
  DEPENDS libAdrenalineKernel${ADRENALINE_MODULE_SUFFIX}_stub.a
  DEPENDS libAdrenalineKernel${ADRENALINE_MODULE_SUFFIX}_stub_weak.a
)
