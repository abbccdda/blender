/* Blender OpenColorIO implementation */

uniform sampler1D curve_mapping_texture;
uniform sampler2D image_texture;
uniform sampler3D lut3d_texture;

uniform float dither;
uniform bool predivide;
uniform bool curve_mapping;

layout(std140) uniform OCIO_GLSLCurveMappingParameters
{
  /* Curve mapping parameters
   *
   * See documentation for OCIO_CurveMappingSettings to get fields descriptions.
   * (this ones pretty much copies stuff from C structure.)
   */
  vec4 curve_mapping_mintable;
  vec4 curve_mapping_range;
  vec4 curve_mapping_ext_in_x;
  vec4 curve_mapping_ext_in_y;
  vec4 curve_mapping_ext_out_x;
  vec4 curve_mapping_ext_out_y;
  vec4 curve_mapping_first_x;
  vec4 curve_mapping_first_y;
  vec4 curve_mapping_last_x;
  vec4 curve_mapping_last_y;
  vec4 curve_mapping_black;
  vec4 curve_mapping_bwmul;
  int curve_mapping_lut_size;
  int curve_mapping_use_extend_extrapolate;
};

float read_curve_mapping(int table, int index)
{
  return texelFetch(curve_mapping_texture, index, 0)[table];
}

float curvemap_calc_extend(int table, float x, vec2 first, vec2 last)
{
  if (x <= first[0]) {
    if (curve_mapping_use_extend_extrapolate == 0) {
      /* horizontal extrapolation */
      return first[1];
    }
    else {
      float fac = (curve_mapping_ext_in_x[table] != 0.0) ?
                      ((x - first[0]) / curve_mapping_ext_in_x[table]) :
                      10000.0;
      return first[1] + curve_mapping_ext_in_y[table] * fac;
    }
  }
  else if (x >= last[0]) {
    if (curve_mapping_use_extend_extrapolate == 0) {
      /* horizontal extrapolation */
      return last[1];
    }
    else {
      float fac = (curve_mapping_ext_out_x[table] != 0.0) ?
                      ((x - last[0]) / curve_mapping_ext_out_x[table]) :
                      -10000.0;
      return last[1] + curve_mapping_ext_out_y[table] * fac;
    }
  }
  return 0.0;
}

float curvemap_evaluateF(int table, float value)
{
  float mintable_ = curve_mapping_mintable[table];
  float range = curve_mapping_range[table];
  float mintable = 0.0;
  int CM_TABLE = curve_mapping_lut_size - 1;

  float fi;
  int i;

  /* index in table */
  fi = (value - mintable) * range;
  i = int(fi);

  /* fi is table float index and should check against table range i.e. [0.0 CM_TABLE] */
  if (fi < 0.0 || fi > float(CM_TABLE)) {
    return curvemap_calc_extend(table,
                                value,
                                vec2(curve_mapping_first_x[table], curve_mapping_first_y[table]),
                                vec2(curve_mapping_last_x[table], curve_mapping_last_y[table]));
  }
  else {
    if (i < 0) {
      return read_curve_mapping(table, 0);
    }
    if (i >= CM_TABLE) {
      return read_curve_mapping(table, CM_TABLE);
    }
    fi = fi - float(i);
    float cm1 = read_curve_mapping(table, i);
    float cm2 = read_curve_mapping(table, i + 1);
    return mix(cm1, cm2, fi);
  }
}

vec4 curvemapping_evaluate_premulRGBF(vec4 col)
{
  col.rgb = (col.rgb - curve_mapping_black.rgb) * curve_mapping_bwmul.rgb;

  vec4 result;
  result.r = curvemap_evaluateF(0, col.r);
  result.g = curvemap_evaluateF(1, col.g);
  result.b = curvemap_evaluateF(2, col.b);
  result.a = col.a;
  return result;
}

float dither_random_value(vec2 co)
{
  return fract(sin(dot(co.xy, vec2(12.9898, 78.233))) * 43758.5453) * 0.005 * dither;
}

vec2 round_to_pixel(sampler2D tex, vec2 uv)
{
  vec2 size = textureSize(tex, 0);
  return vec2(ivec2(uv * size)) / size;
}

vec4 apply_dither(vec4 col, vec2 uv)
{
  col.rgb += dither_random_value(uv);
  return col;
}

vec4 OCIO_ProcessColor(vec4 col, vec2 noise_uv)
{
  if (curve_mapping) {
    col = curvemapping_evaluate_premulRGBF(col);
  }

  if (predivide) {
    if (col.a > 0.0 && col.a < 1.0) {
      col.rgb *= 1.0 / col.a;
    }
  }

  /* NOTE: This is true we only do de-premul here and NO premul
   *       and the reason is simple -- opengl is always configured
   *       for straight alpha at this moment
   */

  col = OCIODisplay(col, lut3d_texture);

  if (dither > 0.0) {
    col = apply_dither(col, noise_uv);
  }

  return col;
}

/*  ------------------------------------------------------------------------  */

in vec2 texCoord_interp;
out vec4 fragColor;

/* Standard Implementation. */
void main()
{
  vec4 col = texture(image_texture, texCoord_interp.st);
  vec2 noise_uv = round_to_pixel(image_texture, texCoord_interp.st);

  fragColor = OCIO_ProcessColor(col, noise_uv);
}

#if 0 /* In-place transformation. No extra buffer required. */
void main()
{
  /* TODO(fclem) use imageLoad/Store to do in-place color-management. */
}
#endif
