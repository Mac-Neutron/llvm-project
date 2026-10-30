# Added by Mac-Neutron (2026): the typed-pointer AIR writer.
add_llvm_component_library(LLVMAIRBitWriter
  AIRBitcodeWriter.cpp
  AIRPointerTypes.cpp
  AIRTypedRefs.cpp
  AIRTypes.cpp
  AIRValueEnumerator.cpp
  AIRWriterC.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm-c

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  BitstreamReader
  Core
  Support
  TargetParser
  )
