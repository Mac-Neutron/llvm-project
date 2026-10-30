# Added by Mac-Neutron (2026): the AIR writer's tool.
set(LLVM_LINK_COMPONENTS
  AIRBitWriter
  Core
  IRReader
  Passes
  Support
  )

include_directories(${LLVM_MAIN_SRC_DIR}/lib/Bitcode/AIRWriter)

add_llvm_tool(llvm-air-write
  llvm-air-write.cpp

  DEPENDS
  intrinsics_gen
  )
