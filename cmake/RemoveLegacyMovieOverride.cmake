# Remove the former host-side movie-completion bypass from existing generated
# sources. Fresh code generation already contains the correct original call.
function(skate3_remove_legacy_movie_override _file)
  file(READ "${_file}" _contents)
  if(NOT _contents MATCHES "ShouldForceIntroMovieComplete")
    return()
  endif()
  set(_legacy "\tif (skate3::demo_path::ShouldForceIntroMovieComplete()) {
\t\tctx.r3.u64 = 0;
\t} else {
\t\t// bl 0x825d60c8
\t\tctx.lr = 0x825E05A0;
\t\trex_AptSafeQueueFixed_Enqueue_QAAXPAX_Z_825D60C8(ctx, base);
\t}")
  set(_original "\t// bl 0x825d60c8
\tctx.lr = 0x825E05A0;
\trex_AptSafeQueueFixed_Enqueue_QAAXPAX_Z_825D60C8(ctx, base);")
  string(REPLACE "${_legacy}" "${_original}" _contents "${_contents}")
  if(_contents MATCHES "ShouldForceIntroMovieComplete")
    message(FATAL_ERROR "Unrecognized legacy movie override in ${_file}; regenerate this file")
  endif()
  string(REPLACE "#include \"skate3_demo_path.h\"" "" _contents "${_contents}")
  file(WRITE "${_file}" "${_contents}")
  message(STATUS "Removed legacy movie-completion override from ${_file}")
endfunction()
