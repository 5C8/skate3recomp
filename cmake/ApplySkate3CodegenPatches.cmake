if(NOT DEFINED SKATE3_SOURCE_DIR)
  message(FATAL_ERROR "SKATE3_SOURCE_DIR is required")
endif()

file(GLOB _skate3_recomp_files
  LIST_DIRECTORIES false
  "${SKATE3_SOURCE_DIR}/generated/skate3_recomp.*.cpp")
if(NOT _skate3_recomp_files)
  message(FATAL_ERROR "No generated Skate 3 recompilation files found")
endif()

function(_skate3_add_include _contents_var _include)
  set(_contents "${${_contents_var}}")
  if(NOT _contents MATCHES "#include \"${_include}\"")
    string(REPLACE
      "#include \"skate3_init.h\"\n"
      "#include \"skate3_init.h\"\n#include \"${_include}\"\n"
      _contents
      "${_contents}")
  endif()
  set(${_contents_var} "${_contents}" PARENT_SCOPE)
endfunction()

set(_frustum_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "Skate3UltrawideGameFrustumPatchScope")
    set(_frustum_patched TRUE)
    break()
  endif()
  string(FIND "${_contents}" "ctx.r6.u64 = REX_LOAD_U32(ctx.r4.u32 + 5260);" _frustum_anchor)
  if(_frustum_anchor EQUAL -1)
    continue()
  endif()

  string(SUBSTRING "${_contents}" ${_frustum_anchor} 12000 _frustum_window)
  string(REGEX MATCH
    "\t// bl 0x[0-9a-fA-F]+\n\tctx\\.lr = 0x[0-9A-F]+;\n\tsub_[0-9A-F]+\\(ctx, base\\);"
    _frustum_call
    "${_frustum_window}")
  if(_frustum_call STREQUAL "")
    message(FATAL_ERROR "Failed to apply Skate 3 generated frustum patch; call near frustum anchor not found in ${_file}")
  endif()

  string(REGEX REPLACE
    "(\tctx\\.lr = 0x[0-9A-F]+;\n)"
    "\\1\tSkate3UltrawideGameFrustumPatchScope skate3_ultrawide_game_frustum_patch_scope(\n\t\tctx, base, ctx.r4.u32);\n"
    _frustum_patch
    "${_frustum_call}")
  string(REPLACE "${_frustum_call}" "${_frustum_patch}" _contents "${_contents}")
  _skate3_add_include(_contents "skate3_ultrawide_guest.h")
  file(WRITE "${_file}" "${_contents}")
  set(_frustum_patched TRUE)
  message(STATUS "Applied Skate 3 generated frustum patch in ${_file}")
  break()
endforeach()
if(NOT _frustum_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 generated frustum patch; frustum anchor not found")
endif()

set(_fov_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "Skate3MaybeOverrideProjectionFovRadians")
    set(_fov_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "ctx\\.f27\\.f64 = ctx\\.f1\\.f64;")
    continue()
  endif()
  if(NOT _contents MATCHES "ctx\\.f4\\.f64 = double\\(float\\(ctx\\.f1\\.f64 \\* ctx\\.f0\\.f64\\)\\);")
    continue()
  endif()

  set(_projection_fov_site "ctx.f27.f64 = ctx.f1.f64;")
  set(_projection_fov_patch
"ctx.f1.f64 = double(Skate3MaybeOverrideProjectionFovRadians(float(ctx.f1.f64)));
	ctx.f27.f64 = ctx.f1.f64;")
  string(REPLACE "${_projection_fov_site}" "${_projection_fov_patch}" _contents "${_contents}")
  _skate3_add_include(_contents "skate3_fov.h")
  file(WRITE "${_file}" "${_contents}")
  set(_fov_patched TRUE)
  message(STATUS "Applied Skate 3 generated projection FOV patch in ${_file}")
  break()
endforeach()
if(NOT _fov_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 generated projection FOV patch; projection FOV anchor not found")
endif()

# Repair generated sources from older builds, and leave new code generation's
# original movie-completion call intact.
include("${CMAKE_CURRENT_LIST_DIR}/RemoveLegacyMovieOverride.cmake")
foreach(_file IN LISTS _skate3_recomp_files)
  skate3_remove_legacy_movie_override("${_file}")
endforeach()

# ReXGlue 0.8.2 can truncate this existing shared tail when a naming include
# changes discovery order. Preserve its baseline epilogue, verified against
# the guest instructions at 0x823FE3F8..0x823FE404. No boundary is changed.
set(_skate3_tail_short [==[DEFINE_REX_FUNC(sub_823FE3F8) {
	REX_FUNC_PROLOGUE();
	// li r11,1
	ctx.r11.s64 = 1;
}
]==])
set(_skate3_tail_complete [==[DEFINE_REX_FUNC(sub_823FE3F8) {
	REX_FUNC_PROLOGUE();
	// li r11,1
	ctx.r11.s64 = 1;
	// clrlwi r3,r11,24
	ctx.r3.u64 = ctx.r11.u32 & 0xFF;
	// addi r1,r1,128
	ctx.r1.s64 = ctx.r1.s64 + 128;
	// b 0x82f52164
	__restgprlr_27(ctx, base);
	return;
}
]==])
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  string(FIND "${_contents}" "DEFINE_REX_FUNC(sub_823FE3F8)" _tail_at)
  if(_tail_at EQUAL -1)
    continue()
  endif()
  string(FIND "${_contents}" "${_skate3_tail_complete}" _complete_at)
  if(NOT _complete_at EQUAL -1)
    break()
  endif()
  string(FIND "${_contents}" "${_skate3_tail_short}" _short_at)
  if(_short_at EQUAL -1)
    message(FATAL_ERROR "Unexpected generated shared-tail body at 0x823FE3F8")
  endif()
  string(REPLACE "${_skate3_tail_short}" "${_skate3_tail_complete}" _contents "${_contents}")
  file(WRITE "${_file}" "${_contents}")
  message(STATUS "Preserved Skate 3 shared-tail epilogue at 0x823FE3F8 in ${_file}")
  break()
endforeach()

# Keep name-only metadata out of SDK function-boundary analysis.
include("${CMAKE_CURRENT_LIST_DIR}/ApplySkate3DeferredNames.cmake")
