add_llvm_component_group(Z80)

set(LLVM_TARGET_DEFINITIONS Z80.td)

tablegen(LLVM Z80GenAsmMatcher.inc -gen-asm-matcher)
tablegen(LLVM Z80GenAsmWriter.inc -gen-asm-writer)
tablegen(LLVM Z80GenCallingConv.inc -gen-callingconv)
tablegen(LLVM Z80GenDAGISel.inc -gen-dag-isel)
tablegen(LLVM Z80GenDisassemblerTables.inc -gen-disassembler)
tablegen(LLVM Z80GenPreLegalizeGICombiner.inc -gen-global-isel-combiner
              -combiners="Z80PreLegalizerCombiner")
tablegen(LLVM Z80GenPostLegalizeGICombiner.inc -gen-global-isel-combiner
              -combiners="Z80PostLegalizerCombiner")
tablegen(LLVM Z80GenGlobalISel.inc -gen-global-isel)
tablegen(LLVM Z80GenInstrInfo.inc -gen-instr-info)
tablegen(LLVM Z80GenMCCodeEmitter.inc -gen-emitter)
tablegen(LLVM Z80GenMCPseudoLowering.inc -gen-pseudo-lowering)
tablegen(LLVM Z80GenRegisterBank.inc -gen-register-bank)
tablegen(LLVM Z80GenRegisterInfo.inc -gen-register-info)
tablegen(LLVM Z80GenSearchableTables.inc -gen-searchable-tables)
tablegen(LLVM Z80GenSubtargetInfo.inc -gen-subtarget)

add_public_tablegen_target(Z80CommonTableGen)

add_llvm_target(Z80CodeGen
  SM83CallLowering.cpp
  Z80AsmPrinter.cpp
  Z80BranchCleanup.cpp
  Z80CallLowering.cpp
  Z80CallingConv.cpp
  Z80PreLegalizerCombiner.cpp
  Z80PostLegalizerCombiner.cpp
  Z80ExpandPseudo.cpp
  Z80FixupImplicitDefs.cpp
  Z80FrameLowering.cpp
  Z80ISelLowering.cpp
  Z80IndexIV.cpp
  Z80InlineAsmLowering.cpp
  Z80InstrCost.cpp
  Z80InstrInfo.cpp
  Z80InstructionSelector.cpp
  Z80LateOptimization.cpp
  Z80LegalizerInfo.cpp
  Z80LowerSelect.cpp
  Z80MCInstLower.cpp
  Z80PostRAScavenging.cpp
  Z80RegisterBankInfo.cpp
  Z80RegisterInfo.cpp
  Z80ShiftRotateChain.cpp
  Z80Subtarget.cpp
  Z80TargetMachine.cpp
  Z80TargetObjectFile.cpp

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  AsmPrinter
  BinaryFormat
  CodeGen
  CodeGenTypes
  Core
  GlobalISel
  IPO
  InstCombine
  LTO
  MC
  Z80Desc
  Z80Info
  Passes
  Scalar
  SelectionDAG
  Support
  Target
  TargetParser
  TransformUtils

  ADD_TO_COMPONENT
  Z80
)

add_subdirectory(AsmParser)
add_subdirectory(Disassembler)
add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)

# Assemble Z80/SM83 runtime files and create .lib archives for selective linking.
# The clang driver links crt0.rel directly and uses -k/-l for the runtime library.
set(Z80_RUNTIME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/runtime)

find_program(SDASZ80 sdasz80)
find_program(SDASGB sdasgb)
find_program(SDAR sdar)

# Z80 runtime: crt0 + per-function .asm -> .lib
if(SDASZ80)
  set(Z80_RUNTIME_OUT ${LLVM_LIBRARY_OUTPUT_INTDIR}/z80)

  # crt0 is always linked directly
  add_custom_command(
    OUTPUT ${Z80_RUNTIME_OUT}/z80_crt0.rel
    COMMAND ${CMAKE_COMMAND} -E make_directory ${Z80_RUNTIME_OUT}
    COMMAND ${SDASZ80} -g -o ${Z80_RUNTIME_OUT}/z80_crt0.rel ${Z80_RUNTIME_DIR}/z80_crt0.asm
    DEPENDS ${Z80_RUNTIME_DIR}/z80_crt0.asm
    COMMENT "Assembling Z80 crt0"
  )
  list(APPEND ALL_RUNTIME_OUTPUTS ${Z80_RUNTIME_OUT}/z80_crt0.rel)

  # Per-function runtime modules -> .lib archive (selective linking)
  if(SDAR)
    file(GLOB Z80_RT_SOURCES ${Z80_RUNTIME_DIR}/z80/*.asm)
    set(Z80_RT_RELS "")
    foreach(src ${Z80_RT_SOURCES})
      get_filename_component(name ${src} NAME_WE)
      set(rel ${Z80_RUNTIME_OUT}/${name}.rel)
      add_custom_command(
        OUTPUT ${rel}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${Z80_RUNTIME_OUT}
        COMMAND ${SDASZ80} -g -o ${rel} ${src}
        DEPENDS ${src}
        COMMENT "Assembling Z80 runtime: ${name}.asm"
      )
      list(APPEND Z80_RT_RELS ${rel})
    endforeach()
    add_custom_command(
      OUTPUT ${Z80_RUNTIME_OUT}/z80_rt.lib
      COMMAND ${SDAR} rs ${Z80_RUNTIME_OUT}/z80_rt.lib ${Z80_RT_RELS}
      DEPENDS ${Z80_RT_RELS}
      COMMENT "Creating Z80 runtime library archive"
    )
    list(APPEND ALL_RUNTIME_OUTPUTS ${Z80_RUNTIME_OUT}/z80_rt.lib)
  else()
    message(WARNING "sdar not found: Z80 runtime library will not be built")
  endif()
else()
  message(WARNING "sdasz80 not found: Z80 runtime files will not be built")
endif()

# SM83 runtime: crt0 + per-function .asm -> .lib
if(SDASGB)
  set(SM83_RUNTIME_OUT ${LLVM_LIBRARY_OUTPUT_INTDIR}/sm83)

  # crt0 is always linked directly
  add_custom_command(
    OUTPUT ${SM83_RUNTIME_OUT}/sm83_crt0.rel
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SM83_RUNTIME_OUT}
    COMMAND ${SDASGB} -g -o ${SM83_RUNTIME_OUT}/sm83_crt0.rel ${Z80_RUNTIME_DIR}/sm83_crt0.asm
    DEPENDS ${Z80_RUNTIME_DIR}/sm83_crt0.asm
    COMMENT "Assembling SM83 crt0"
  )
  list(APPEND ALL_RUNTIME_OUTPUTS ${SM83_RUNTIME_OUT}/sm83_crt0.rel)

  # Per-function runtime modules -> .lib archive (selective linking)
  if(SDAR)
    file(GLOB SM83_RT_SOURCES ${Z80_RUNTIME_DIR}/sm83/*.asm)
    set(SM83_RT_RELS "")
    foreach(src ${SM83_RT_SOURCES})
      get_filename_component(name ${src} NAME_WE)
      set(rel ${SM83_RUNTIME_OUT}/${name}.rel)
      add_custom_command(
        OUTPUT ${rel}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SM83_RUNTIME_OUT}
        COMMAND ${SDASGB} -g -o ${rel} ${src}
        DEPENDS ${src}
        COMMENT "Assembling SM83 runtime: ${name}.asm"
      )
      list(APPEND SM83_RT_RELS ${rel})
    endforeach()
    add_custom_command(
      OUTPUT ${SM83_RUNTIME_OUT}/sm83_rt.lib
      COMMAND ${SDAR} rs ${SM83_RUNTIME_OUT}/sm83_rt.lib ${SM83_RT_RELS}
      DEPENDS ${SM83_RT_RELS}
      COMMENT "Creating SM83 runtime library archive"
    )
    list(APPEND ALL_RUNTIME_OUTPUTS ${SM83_RUNTIME_OUT}/sm83_rt.lib)
  else()
    message(WARNING "sdar not found: SM83 runtime library will not be built")
  endif()
else()
  message(WARNING "sdasgb not found: SM83 runtime files will not be built")
endif()

if(ALL_RUNTIME_OUTPUTS)
  add_custom_target(Z80Runtime ALL DEPENDS ${ALL_RUNTIME_OUTPUTS})
endif()

# Install runtime files to <prefix>/lib/{z80,sm83}/ so the driver can find them.
if(SDASZ80)
  install(FILES ${Z80_RUNTIME_OUT}/z80_crt0.rel
          DESTINATION lib/z80 COMPONENT Z80Runtime)
  if(SDAR)
    install(FILES ${Z80_RUNTIME_OUT}/z80_rt.lib
            DESTINATION lib/z80 COMPONENT Z80Runtime)
  endif()
endif()
if(SDASGB)
  install(FILES ${SM83_RUNTIME_OUT}/sm83_crt0.rel
          DESTINATION lib/sm83 COMPONENT Z80Runtime)
  if(SDAR)
    install(FILES ${SM83_RUNTIME_OUT}/sm83_rt.lib
            DESTINATION lib/sm83 COMPONENT Z80Runtime)
  endif()
endif()
