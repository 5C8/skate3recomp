// Keep diffuse cutouts independent of optional specular/normal bindings.
// overlay.w also identifies those maps, not just composited decal art.
// This scalar-only predicate is shared with the C++ regression check.
bool SceneDiffuseAlphaTestEnabled(float skin_flag, float alpha_state,
                                  float overlay_binding, float shader_family) {
  bool cutout_family =
      (shader_family < -6.5 && shader_family > -7.5) ||
      (shader_family < -8.5 && shader_family > -10.5);
  return skin_flag == 0.0 && alpha_state >= 0.0 &&
         (overlay_binding < 0.5 || cutout_family) && shader_family > -20.5;
}
