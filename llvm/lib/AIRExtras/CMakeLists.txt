# Added by Mac-Neutron (2026): C entry points llvm-c lacks (llvm-c/AIRExtras.h).
add_llvm_component_library(LLVMAIRExtras
  AIRExtrasC.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm-c

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  BitReader
  Core
  Linker
  Passes
  Support
  TransformUtils
  )
