function(skate3_patch_character_load_trace)
  set(_fallback_found FALSE)
  set(_alloc_found FALSE)
  foreach(_file IN LISTS ARGN)
    file(READ "${_file}" _source)
    set(_original "${_source}")
    if(_source MATCHES "DEFINE_REX_FUNC\\(sub_827D8E48\\)")
      set(_fallback_found TRUE)
      if(NOT _source MATCHES "Skate3TraceCharacterFallback\\(")
        set(_anchor "DEFINE_REX_FUNC(sub_827D8E48) {\n\tREX_FUNC_PROLOGUE();")
        string(FIND "${_source}" "${_anchor}" _at)
        if(_at EQUAL -1)
          message(FATAL_ERROR "Unexpected character fallback function in ${_file}")
        endif()
        string(REPLACE "${_anchor}" "${_anchor}\n\tSkate3TraceCharacterFallback(uint32_t(ctx.lr), ctx.r3.u32);"
          _source "${_source}")
      endif()
      if(NOT _source MATCHES "character-load: request")
        set(_anchor "DEFINE_REX_FUNC(sub_827D60A0) {\n\tREX_FUNC_PROLOGUE();")
        string(FIND "${_source}" "${_anchor}" _at)
        if(_at EQUAL -1)
          message(FATAL_ERROR "Unexpected marquee character request function in ${_file}")
        endif()
        set(_request [==[
	if (REXCVAR_GET(skate3_character_load_trace)) {
		char recipe[65]{};
		for (uint32_t i = 0; i < 64; ++i) {
			recipe[i] = static_cast<char>(REX_LOAD_U8(ctx.r5.u32 + i));
			if (recipe[i] == '\0') break;
		}
		REXLOG_INFO("character-load: request recipe={} caller={:08X} owner={:08X}",
		            recipe, uint32_t(ctx.lr), ctx.r6.u32);
	}]==])
        string(REPLACE "${_anchor}" "${_anchor}${_request}" _source "${_source}")
      endif()
      if(NOT _source MATCHES "character-load: completion")
        foreach(_callback IN ITEMS 827D8338 827D8780)
          string(REGEX MATCH "DEFINE_REX_FUNC\\([A-Za-z0-9_]+_${_callback}\\) \\{\n\tREX_FUNC_PROLOGUE\\(\\);"
            _anchor "${_source}")
          if(_anchor STREQUAL "")
            message(FATAL_ERROR "Unexpected character load callback ${_callback} in ${_file}")
          endif()
          string(CONCAT _completion
            "\n\tif (REXCVAR_GET(skate3_character_load_trace) && "
            "ctx.r4.u32 == REX_LOAD_U32(ctx.r3.u32 + 108)) {"
            "\n\t\tREXLOG_INFO(\"character-load: completion handler=${_callback} "
            "owner={:08X} id={} payload={:08X}\", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);"
            "\n\t}")
          string(REPLACE "${_anchor}" "${_anchor}${_completion}" _source "${_source}")
        endforeach()
      endif()
    endif()

    if(_source MATCHES "DEFINE_REX_FUNC\\(sub_8298E2E8\\)")
      set(_alloc_found TRUE)
      if(NOT _source MATCHES "Skate3TraceCharacterAllocation\\(")
        set(_anchor "ctx.lr = 0x8298E33C;\n\tREX_CALL_INDIRECT_FUNC(ctx.ctr.u32);")
        string(FIND "${_source}" "${_anchor}" _at)
        if(_at EQUAL -1)
          message(FATAL_ERROR "Unexpected guest allocation call in ${_file}")
        endif()
        set(_replacement [==[ctx.lr = 0x8298E33C;
	{
		const bool trace = REXCVAR_GET(skate3_character_load_trace);
		const uint32_t category = ctx.r8.u32;
		const uint32_t heap = ctx.r3.u32;
		const uint32_t bytes = ctx.r4.u32;
		const uint32_t alignment = ctx.r5.u32;
		const uint32_t caller = trace ? REX_LOAD_U32(ctx.r1.u32 + 104) : 0;
		REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
		if (trace) {
			Skate3TraceCharacterAllocation(category, heap, bytes, alignment, caller, ctx.r3.u32);
		}
	}]==])
        string(REPLACE "${_anchor}" "${_replacement}" _source "${_source}")
      endif()
    endif()

    if(NOT _source STREQUAL _original)
      if(NOT _source MATCHES "#include \"skate3_character_load_trace.h\"")
        string(PREPEND _source "#include \"skate3_character_load_trace.h\"\n")
      endif()
      file(WRITE "${_file}" "${_source}")
    endif()
    if(_fallback_found AND _alloc_found)
      return()
    endif()
  endforeach()
  if(ARGN)
    message(FATAL_ERROR "Could not locate both guest character load trace sites")
  endif()
endfunction()
