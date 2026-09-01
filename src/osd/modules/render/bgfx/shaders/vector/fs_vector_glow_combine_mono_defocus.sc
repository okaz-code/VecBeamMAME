$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Monochrome/vector composite. Legacy CRT lens geometry affects ambient and bezel reflection only;
// vector emission and ordinary glow remain geometrically straight.

#include "common.sh"
SAMPLER2D(s_base, 0);
SAMPLER2D(s_bloom, 1);
SAMPLER2D(s_optical, 2);
SAMPLER2D(s_bezel_source, 3);
SAMPLER2D(s_bezel_length, 4);

// Scales the post-pool aux buffers (analytic glow, halation, no-persist dots, rays) by the beam
// window's deposited fraction. The renderer used to bake this into their vertices, but their
// content only changes when a new source pass arrives, so it is built once per pass now and the
// per-present ramp arrives here instead. It has to be applied at the sample, ahead of any tonal
// reshape - scaling after a power curve is not the same scaling. 1 outside the window path.
uniform vec4 u_aux_ramp;
#define AUX_TEX2D(_sampler, _uv) (texture2D(_sampler, _uv) * u_aux_ramp.x)
// Bezel reflection reads the glow buffer WITHOUT u_aux_ramp - deliberately, and unlike every other
// consumer of that texture. The ramp scales whole-pass aux content down to the fraction of the sweep
// the body has deposited so far, so a halo never outshines the line it belongs to. The bezel is a
// different quantity: it is diffuse room-side light from the whole tube face, and the face is
// showing the whole pass, because the phosphor pool retains it between windowed presents. Ramping it
// made the bezel collapse at the start of every pass and recover over it - a pulse with no physical
// counterpart, and a severe one, because the ramp lands ahead of two expansive power curves
// (0.32 -> 0.16 after shape_wide_source -> 0.07 after shape_glow).
//
// s_bezel_length must be sampled the same way. long_light is min()'d against the source, so a ramped
// classification against an unramped source would clamp the long term and report ordinary long
// strokes as short.
#define BEZEL_TEX2D(_sampler, _uv) texture2D(_sampler, _uv)
uniform vec4 u_defocus;
uniform vec4 u_vec_pincushion_x_quad;
uniform vec4 u_glow_enable;
uniform vec4 u_phosphor_color;
uniform vec4 u_target_dims;
uniform vec4 u_quad_dims;
uniform vec4 u_ambient_color;
uniform vec4 u_ambient_level;
uniform vec4 u_room_ambient;
uniform vec4 u_ambient_output_scale;
uniform vec4 u_hdr_glow_compensation;
uniform vec4 u_convergence_global;
uniform vec4 u_convergence_global_color;
uniform vec4 u_tube_distortion;
uniform vec4 u_tube_round_corner;
uniform vec4 u_tube_vignetting;
uniform vec4 u_tube_face_scale;
uniform vec4 u_vector_image_scale;
uniform vec4 u_bezel_glow_strength;
uniform vec4 u_bezel_glow_width;
uniform vec4 u_vector_render_scale;
uniform vec4 u_bezel_glow_curve;
uniform vec4 u_bezel_long_reflection;
uniform vec4 u_bezel_short_reflection;
uniform vec4 u_bezel_long_threshold;
uniform vec4 u_glow_tail_curve;
uniform vec4 u_glow_black_toe;
#define PINCUSHION_GAIN (5.0 / 30.0)
#define GLOW_BRIGHTNESS_GAIN 2.67

float tube_active_amount()
{
	float geometry=abs(u_tube_distortion.x)+u_tube_round_corner.x;
	return step(0.0001,u_ambient_level.x)*step(0.0001,geometry+abs(1.0-u_tube_face_scale.x));
}
// Vector Image Scale is applied to beam coordinates before rasterisation, preserving overscan and spot width.
vec2 emission_uv(vec2 uv){return uv;}
vec2 tube_quad_dims(){return max(u_quad_dims.xy,vec2_splat(1.0));}
vec2 tube_view_scale(){return max(u_target_dims.xy,vec2_splat(1.0))/tube_quad_dims();}
vec2 tube_face_coord(vec2 uv,float active){float s=mix(1.0,clamp(u_tube_face_scale.x,0.75,1.0),active);return (uv-vec2_splat(0.5))*tube_view_scale()/s;}
vec2 tube_aspect(){vec2 dims=tube_quad_dims();return dims/max(min(dims.x,dims.y),1.0);}
float safe_distortion_divisor(float v){return abs(v)<1e-4?(v<0.0?-1e-4:1e-4):v;}
vec2 distort_centered(vec2 p,float amount)
{
	vec2 aspect=tube_aspect(),physical=p*aspect;
	float r2=dot(physical,physical),f=1.0+r2*amount;vec2 half_extent=vec2_splat(0.5)*aspect;
	float rx2=half_extent.x*half_extent.x,ry2=half_extent.y*half_extent.y;
	float fit_x=1.0+rx2*amount,fit_y=1.0+ry2*amount;
	physical*=vec2(f/safe_distortion_divisor(fit_x),f/safe_distortion_divisor(fit_y));return physical/aspect;
}
vec2 tube_quad_coord(vec2 uv,float active)
{
	return distort_centered(tube_face_coord(uv,active),u_tube_distortion.x*active);
}
float round_box(vec2 p,vec2 b,float r){vec2 q=abs(p)-b+r;return min(max(q.x,q.y),0.0)+length(max(q,vec2_splat(0.0)))-r;}
// These take the distorted tube coordinate and the aspect rather than deriving them, because
// main() needs the same pair for the face, the vignette and the bezel band. Deriving it inside
// each ran distort_centered three times per pixel - and tube_aspect five - for one answer.
float tube_signed_distance_at(vec2 q,vec2 aspect,float active)
{
	if(active<0.5)return -1.0;float radius=clamp(u_tube_round_corner.x*0.25,0.0,0.45);
	return round_box(q*aspect,vec2_splat(0.5)*aspect,radius);
}
float tube_face_factor_at(float sd,float active)
{
	if(active<0.5)return 1.0;vec2 dims=tube_quad_dims();
	float aa=max(fwidth(sd),1.0/max(min(dims.x,dims.y),1.0));return 1.0-smoothstep(-aa,aa,sd);
}
float tube_vignette_at(vec2 q,vec2 aspect,float active)
{
	if(active<0.5)return 1.0;float amount=max(u_tube_vignetting.x,0.0);
	float len=length(q*aspect)*(1.41421356/length(aspect));float blur=amount*0.75+0.25,radius=1.0-amount*0.25;
	return saturate(smoothstep(radius,radius-blur,len));
}
float bezel_glow_width_px(){return max(u_bezel_glow_width.x,1.0)*clamp(u_vector_render_scale.x,0.1,1.0);}

// The bezel band was deriving its own signed distance with a comment saying it had to match
// tube_signed_distance exactly - so it was the same round_box on the same coordinate, computed
// twice. It takes that distance now.
float bezel_band_at(float sd,float active)
{
	if(active<0.5||u_ambient_level.x<=0.0||u_bezel_glow_strength.x<=0.0)return 0.0;
	vec2 dims=tube_quad_dims();float signed_px=sd*min(dims.x,dims.y);
	float width_px=bezel_glow_width_px(),curve=max(u_bezel_glow_curve.x,0.25);
	return exp(-pow(max(signed_px,0.0)/width_px,curve))*step(0.0,signed_px);
}
vec3 shape_glow(vec3 c)
{
	c=max(c,vec3_splat(0.0));float peak=max(c.r,max(c.g,c.b));if(peak<=1e-7)return vec3_splat(0.0);
	float pivot=0.25,curve=max(u_glow_tail_curve.x,0.1);float shaped=peak<pivot?pivot*pow(max(peak/pivot,1e-6),curve):peak;
	float toe=max(u_glow_black_toe.x,0.0);if(toe>0.0)shaped*=smoothstep(0.0,toe,peak);return c*(shaped/peak);
}
vec2 bezel_source_min()
{
	vec2 content_scale=min(tube_quad_dims()/max(u_target_dims.xy,vec2_splat(1.0)),vec2_splat(1.0));
	return (vec2_splat(1.0)-content_scale)*0.5;
}
vec2 bezel_source_max(){return vec2_splat(1.0)-bezel_source_min();}
vec2 bezel_source_axis(vec2 source_uv)
{
	vec2 content_min=bezel_source_min(),content_max=bezel_source_max();
	float nearest=abs(source_uv.x-content_min.x);vec2 axis=vec2(0.0,1.0);
	float candidate=abs(content_max.x-source_uv.x);if(candidate<nearest){nearest=candidate;axis=vec2(1.0,-1.0);}
	candidate=abs(source_uv.y-content_min.y);if(candidate<nearest){nearest=candidate;axis=vec2(2.0,1.0);}
	candidate=abs(content_max.y-source_uv.y);if(candidate<nearest)axis=vec2(3.0,-1.0);
	return axis;
}
vec2 bezel_source_edge(vec2 source_uv,vec2 axis)
{
	vec2 content_min=bezel_source_min(),content_max=bezel_source_max();
	vec2 reach=vec2_splat(bezel_glow_width_px())/max(u_target_dims.xy,vec2_splat(1.0));
	vec2 tangent_min=min(content_min+reach,content_max),tangent_max=max(content_max-reach,content_min);
	if(axis.x<0.5)return vec2(content_min.x,clamp(source_uv.y,tangent_min.y,tangent_max.y));
	if(axis.x<1.5)return vec2(content_max.x,clamp(source_uv.y,tangent_min.y,tangent_max.y));
	if(axis.x<2.5)return vec2(clamp(source_uv.x,tangent_min.x,tangent_max.x),content_min.y);
	return vec2(clamp(source_uv.x,tangent_min.x,tangent_max.x),content_max.y);
}
vec2 bezel_source_step(vec2 axis)
{
	vec2 inward=axis.x<1.5?vec2(axis.y,0.0):vec2(0.0,axis.y);
	return inward*bezel_glow_width_px()/max(u_target_dims.xy,vec2_splat(1.0));
}
vec3 apply_bezel_length_gain(vec2 uv,vec3 light)
{
	vec3 long_light=min(max(BEZEL_TEX2D(s_bezel_length,uv).rgb,vec3_splat(0.0)),light);
	vec3 short_light=max(light-long_light,vec3_splat(0.0));
	return short_light*max(u_bezel_short_reflection.x,0.0)+long_light*max(u_bezel_long_reflection.x,0.0);
}
vec3 bezel_bloom_source(vec2 edge_uv,vec2 reach_uv)
{
	// Sample raw analytic light so bezel reflection strength and reach do not
	// inherit the visible Glow Wide field.
	vec2 best_uv=edge_uv;vec3 source=apply_bezel_length_gain(best_uv,BEZEL_TEX2D(s_bezel_source,best_uv).rgb);float best_peak=max(source.r,max(source.g,source.b));
	vec2 candidate_uv=edge_uv+reach_uv*0.125;vec3 candidate=apply_bezel_length_gain(candidate_uv,BEZEL_TEX2D(s_bezel_source,candidate_uv).rgb);float peak=max(candidate.r,max(candidate.g,candidate.b));
	if(peak>best_peak){best_uv=candidate_uv;source=candidate;best_peak=peak;}
	candidate_uv=edge_uv+reach_uv*0.25;candidate=apply_bezel_length_gain(candidate_uv,BEZEL_TEX2D(s_bezel_source,candidate_uv).rgb);peak=max(candidate.r,max(candidate.g,candidate.b));
	if(peak>best_peak){best_uv=candidate_uv;source=candidate;best_peak=peak;}
	candidate_uv=edge_uv+reach_uv*0.5;candidate=apply_bezel_length_gain(candidate_uv,BEZEL_TEX2D(s_bezel_source,candidate_uv).rgb);peak=max(candidate.r,max(candidate.g,candidate.b));
	if(peak>best_peak){best_uv=candidate_uv;source=candidate;best_peak=peak;}
	candidate_uv=edge_uv+reach_uv;candidate=apply_bezel_length_gain(candidate_uv,BEZEL_TEX2D(s_bezel_source,candidate_uv).rgb);peak=max(candidate.r,max(candidate.g,candidate.b));
	if(peak>best_peak){best_uv=candidate_uv;source=candidate;best_peak=peak;}
	return source*GLOW_BRIGHTNESS_GAIN;
}
vec3 bezel_optical_source(vec2 edge_uv,vec2 reach_uv)
{
	vec3 source=AUX_TEX2D(s_optical,edge_uv).rgb;
	source=max(source,AUX_TEX2D(s_optical,edge_uv+reach_uv*0.125).rgb);
	source=max(source,AUX_TEX2D(s_optical,edge_uv+reach_uv*0.25).rgb);
	source=max(source,AUX_TEX2D(s_optical,edge_uv+reach_uv*0.5).rgb);
	return max(source,AUX_TEX2D(s_optical,edge_uv+reach_uv).rgb)*GLOW_BRIGHTNESS_GAIN;
}
vec3 bezel_axis_light(vec2 source_uv,vec2 axis)
{
	vec2 edge_uv=bezel_source_edge(source_uv,axis),reach_uv=bezel_source_step(axis);
	vec3 edge_glow=u_glow_enable.x>0.0?shape_glow(bezel_bloom_source(edge_uv,reach_uv)):vec3_splat(0.0);
	return edge_glow+bezel_optical_source(edge_uv,reach_uv);
}
vec3 bezel_corner_light(vec2 source_uv)
{
	bool left=source_uv.x<0.5,top=source_uv.y<0.5;
	vec2 vertical_axis=left?vec2(0.0,1.0):vec2(1.0,-1.0);
	vec2 horizontal_axis=top?vec2(2.0,1.0):vec2(3.0,-1.0);
	vec2 content_min=bezel_source_min(),content_max=bezel_source_max();
	float vertical_distance=min(abs(source_uv.x-content_min.x),abs(content_max.x-source_uv.x));
	float horizontal_distance=min(abs(source_uv.y-content_min.y),abs(content_max.y-source_uv.y));
	float corner_blend=bezel_glow_width_px()/max(min(u_target_dims.x,u_target_dims.y),1.0);
	float distance_delta=vertical_distance-horizontal_distance;
	if(distance_delta<=-corner_blend)return bezel_axis_light(source_uv,vertical_axis);
	if(distance_delta>=corner_blend)return bezel_axis_light(source_uv,horizontal_axis);
	vec3 vertical_light=bezel_axis_light(source_uv,vertical_axis);
	vec3 horizontal_light=bezel_axis_light(source_uv,horizontal_axis);
	return mix(vertical_light,horizontal_light,smoothstep(-corner_blend,corner_blend,distance_delta));
}
vec2 vector_pincushion_uv(vec2 texcoord)
{
	// Zero is the identity and the transform costs a few multiplies per pixel, so skip it.
	if (u_vec_pincushion_x_quad.x == 0.0) return texcoord;
	vec2 uv=texcoord*2.0-1.0;float x=uv.x,y=uv.y,y2=y*y;
	x*=1.0+u_vec_pincushion_x_quad.x*PINCUSHION_GAIN*y2;return (vec2(x,y)+1.0)*0.5;
}
vec3 sample_defocused(vec2 uv)
{
	if(u_defocus.x==0.0&&u_defocus.y==0.0)return texture2D(s_base,uv).rgb;
	const vec2 C1=vec2(-1.60,0.25),C2=vec2(-1.00,-0.55),C3=vec2(-0.55,1.00),C4=vec2(-0.25,-1.60);
	const vec2 C5=vec2(0.25,1.60),C6=vec2(0.55,-1.00),C7=vec2(1.00,0.55),C8=vec2(1.60,-0.25);
	vec2 d=u_defocus.xy*vec2_splat(1.0/1024.0);vec3 b=texture2D(s_base,uv).rgb+texture2D(s_base,uv+C1*d).rgb+texture2D(s_base,uv+C2*d).rgb
		+texture2D(s_base,uv+C3*d).rgb+texture2D(s_base,uv+C4*d).rgb+texture2D(s_base,uv+C5*d).rgb+texture2D(s_base,uv+C6*d).rgb+texture2D(s_base,uv+C7*d).rgb+texture2D(s_base,uv+C8*d).rgb;
	return b*(1.0/9.0);
}
void main()
{
	float tube_active=tube_active_amount();vec2 emit_uv=emission_uv(v_texcoord0);vec2 base_uv=vector_pincushion_uv(emit_uv);
	bool outside=base_uv.x<0.0||base_uv.x>1.0||base_uv.y<0.0||base_uv.y>1.0;vec3 base=outside?vec3_splat(0.0):sample_defocused(base_uv);
	vec2 tube_aspect_v=tube_aspect();vec2 tube_q=tube_quad_coord(v_texcoord0,tube_active);
	float tube_sd=tube_signed_distance_at(tube_q,tube_aspect_v,tube_active);
	float face=tube_face_factor_at(tube_sd,tube_active),vignette=tube_vignette_at(tube_q,tube_aspect_v,tube_active);
	vec3 phosphor_tint=max(u_phosphor_color.rgb,vec3_splat(0.0));
	vec3 ambient=u_ambient_level.x*max(u_room_ambient.x,0.0)*0.001*u_ambient_color.rgb*u_ambient_output_scale.x*face*vignette;
	vec3 glow=vec3_splat(0.0),optical=vec3_splat(0.0);bool emit_outside=emit_uv.x<0.0||emit_uv.x>1.0||emit_uv.y<0.0||emit_uv.y>1.0;
	if(!emit_outside){if(u_glow_enable.x>0.0)glow=shape_glow(texture2D(s_bloom,emit_uv).rgb*GLOW_BRIGHTNESS_GAIN);optical=AUX_TEX2D(s_optical,emit_uv).rgb*GLOW_BRIGHTNESS_GAIN;}
	// The gain is a uniform, so this branch is coherent across the whole draw. At zero - which is
	// the Vectrex chain's default - the exp and the sqrt inside length() were pure waste.
	float global_weight=0.0;
	if(u_convergence_global.z>0.0)
	{
		vec2 global_delta=(v_texcoord0-u_convergence_global.xy)*u_target_dims.xy;
		float global_sigma=max(u_convergence_global.w*0.5*length(u_target_dims.xy),1.0);
		global_weight=u_convergence_global.z*exp(-0.5*dot(global_delta,global_delta)/(global_sigma*global_sigma));
	}
	glow*=phosphor_tint;optical*=phosphor_tint;
	vec3 global_out=max(u_convergence_global_color.rgb,vec3_splat(0.0))*phosphor_tint*global_weight*face;
	vec3 bezel=vec3_splat(0.0);float band=bezel_band_at(tube_sd,tube_active);
	if(band>0.0)bezel=bezel_corner_light(emit_uv)*phosphor_tint*(band*max(u_bezel_glow_strength.x,0.0));
	// Apply after shape_glow so changing HDR Beam Peak does not reveal more of the nonlinear tail.
	float glow_compensation=max(u_hdr_glow_compensation.x,0.0);
	vec3 composite=(base+(glow+optical)*glow_compensation)*face+ambient+(global_out+bezel)*glow_compensation;
	gl_FragColor=vec4(composite,1.0)*v_color0;
}
