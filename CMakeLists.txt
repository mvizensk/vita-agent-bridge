cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
endif()
project(vita-agent-bridge C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O2")

# Kernel side: screen planes, button/analog/touch injection, keep-awake,
# kill-foreground, hold-PS-to-quit, and injection of the bridge into SceShell.
add_executable(vakern kernel/vakern.c)
target_compile_options(vakern PRIVATE -std=gnu11)  # taiHEN's TAI_CONTINUE needs pre-C23 ()
target_link_libraries(vakern SceCtrlForDriver_stub SceDisplayForDriver_stub
  SceAppMgrForDriver_stub SceThreadmgrForDriver_stub SceSysmemForDriver_stub
  SceSysclibForDriver_stub SceKernelSuspendForDriver_stub SceSysrootForDriver_stub
  SceProcessmgrForDriver_stub SceIofilemgrForDriver_stub taihenForKernel_stub
  SceModulemgrForDriver_stub SceModulemgrForKernel_363_stub)
set_target_properties(vakern PROPERTIES LINK_FLAGS "-nostdlib")
set(SAVED_SELF_FLAGS "${VITA_MAKE_FSELF_FLAGS}")
set(VITA_MAKE_FSELF_FLAGS "")
vita_create_self(vakern.skprx vakern CONFIG ${CMAKE_SOURCE_DIR}/kernel/vakern.yml UNSAFE)
set(VITA_MAKE_FSELF_FLAGS "${SAVED_SELF_FLAGS}")
vita_create_stubs(vakern_stubs vakern ${CMAKE_SOURCE_DIR}/kernel/vakern.yml KERNEL)

# User side: the TCP service, injected into SceShell by vakern so that games
# cannot evict it. Reads its token from ur0:data/vita-agent-bridge/token.txt.
add_executable(vabridge bridge/vabridge.c bridge/fileops.c bridge/sha256.c bridge/fetch.c)
add_dependencies(vabridge vakern_stubs)
target_link_libraries(vabridge ${CMAKE_CURRENT_BINARY_DIR}/vakern_stubs/libVitaAgentBridgeK1_stub.a
  SceLibKernel_stub ScePower_stub SceKernelThreadMgr_stub SceIofilemgr_stub
  SceNet_stub SceProcessmgr_stub SceAppMgr_stub SceSysmem_stub
  SceHttp_stub_weak SceSsl_stub_weak gcc)
# HTTP/SSL are weak imports: a shell without them still loads the bridge and
# fetch reports the error. -nostdlib: keep gcc from emitting memset calls.
target_compile_options(vabridge PRIVATE -fno-tree-loop-distribute-patterns)
set_target_properties(vabridge PROPERTIES LINK_FLAGS "-nostdlib")
vita_create_self(vabridge.suprx vabridge CONFIG ${CMAKE_SOURCE_DIR}/bridge/vabridge.yml UNSAFE)
