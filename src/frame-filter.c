/*
AC Super FX
Copyright (C) 2026 alejoxcode

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>
#include <math.h>

#define S_RADIUS "radius"
#define S_BORDER "border_group"
#define S_BORDER_WIDTH "border_width"
#define S_BORDER_COLOR "border_color"
#define S_SHADOW "shadow"
#define S_SHADOW_COLOR "shadow_color"
#define S_SHADOW_OPACITY "shadow_opacity"
#define S_SHADOW_BLUR "shadow_blur"
#define S_SHADOW_X "shadow_x"
#define S_SHADOW_Y "shadow_y"

struct frame_filter {
	obs_source_t *context;

	gs_effect_t *effect;
	gs_eparam_t *param_image;
	gs_eparam_t *param_source_size;
	gs_eparam_t *param_output_size;
	gs_eparam_t *param_pad;
	gs_eparam_t *param_radius;
	gs_eparam_t *param_border_width;
	gs_eparam_t *param_border_color;
	gs_eparam_t *param_shadow_color;
	gs_eparam_t *param_shadow_offset;
	gs_eparam_t *param_shadow_blur;
	gs_eparam_t *param_shadow_opacity;

	gs_texrender_t *input;

	float radius;
	float border_width;
	struct vec4 border_color;

	bool shadow;
	struct vec4 shadow_color;
	float shadow_opacity;
	float shadow_blur;
	float shadow_x;
	float shadow_y;

	uint32_t pad; /* margen para la sombra, en pixeles por lado */
	bool rendering;
};

static const char *frame_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ACFX.Frame");
}

static void frame_update(void *data, obs_data_t *settings)
{
	struct frame_filter *f = data;

	f->radius = (float)obs_data_get_int(settings, S_RADIUS);
	f->border_width = (float)obs_data_get_int(settings, S_BORDER_WIDTH);
	vec4_from_rgba(&f->border_color, (uint32_t)obs_data_get_int(settings, S_BORDER_COLOR));
	f->border_color.w = 1.0f;

	f->shadow = obs_data_get_bool(settings, S_SHADOW);
	vec4_from_rgba(&f->shadow_color, (uint32_t)obs_data_get_int(settings, S_SHADOW_COLOR));
	f->shadow_color.w = 1.0f;
	f->shadow_opacity = (float)obs_data_get_int(settings, S_SHADOW_OPACITY) / 100.0f;
	f->shadow_blur = (float)obs_data_get_int(settings, S_SHADOW_BLUR);
	f->shadow_x = (float)obs_data_get_int(settings, S_SHADOW_X);
	f->shadow_y = (float)obs_data_get_int(settings, S_SHADOW_Y);

	if (f->shadow && f->shadow_opacity > 0.0f) {
		float reach = f->shadow_blur + fmaxf(fabsf(f->shadow_x), fabsf(f->shadow_y));
		f->pad = (uint32_t)ceilf(reach) + 1;
	} else {
		f->pad = 0;
	}
}

static void *frame_create(obs_data_t *settings, obs_source_t *context)
{
	struct frame_filter *f = bzalloc(sizeof(*f));
	f->context = context;

	char *path = obs_module_file("effects/frame.effect");
	char *error = NULL;

	obs_enter_graphics();
	f->effect = path ? gs_effect_create_from_file(path, &error) : NULL;
	if (f->effect) {
		f->param_image = gs_effect_get_param_by_name(f->effect, "image");
		f->param_source_size = gs_effect_get_param_by_name(f->effect, "source_size");
		f->param_output_size = gs_effect_get_param_by_name(f->effect, "output_size");
		f->param_pad = gs_effect_get_param_by_name(f->effect, "pad");
		f->param_radius = gs_effect_get_param_by_name(f->effect, "radius");
		f->param_border_width = gs_effect_get_param_by_name(f->effect, "border_width");
		f->param_border_color = gs_effect_get_param_by_name(f->effect, "border_color");
		f->param_shadow_color = gs_effect_get_param_by_name(f->effect, "shadow_color");
		f->param_shadow_offset = gs_effect_get_param_by_name(f->effect, "shadow_offset");
		f->param_shadow_blur = gs_effect_get_param_by_name(f->effect, "shadow_blur");
		f->param_shadow_opacity = gs_effect_get_param_by_name(f->effect, "shadow_opacity");
	}
	f->input = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();

	if (!f->effect)
		obs_log(LOG_ERROR, "could not load frame.effect: %s", error ? error : "(unknown error)");

	bfree(error);
	bfree(path);

	frame_update(f, settings);
	return f;
}

static void frame_destroy(void *data)
{
	struct frame_filter *f = data;

	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	gs_texrender_destroy(f->input);
	obs_leave_graphics();

	bfree(f);
}

/* La fuente crece el margen de la sombra por cada lado. */
static uint32_t frame_get_width(void *data)
{
	struct frame_filter *f = data;
	obs_source_t *target = obs_filter_get_target(f->context);
	uint32_t width = target ? obs_source_get_base_width(target) : 0;
	return width ? width + f->pad * 2 : 0;
}

static uint32_t frame_get_height(void *data)
{
	struct frame_filter *f = data;
	obs_source_t *target = obs_filter_get_target(f->context);
	uint32_t height = target ? obs_source_get_base_height(target) : 0;
	return height ? height + f->pad * 2 : 0;
}

/* Dibuja la fuente (con los filtros anteriores) dentro de f->input. */
static bool capture_input(struct frame_filter *f, uint32_t width, uint32_t height)
{
	gs_effect_t *pass_through = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	bool ok = false;

	gs_texrender_reset(f->input);

	if (!obs_source_process_filter_begin(f->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
		return false;

	gs_blend_state_push();
	gs_reset_blend_state();
	gs_enable_blending(false);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(f->input, width, height)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

		obs_source_process_filter_end(f->context, pass_through, width, height);
		gs_texrender_end(f->input);
		ok = true;
	}

	gs_blend_state_pop();
	return ok;
}

static void frame_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct frame_filter *f = data;

	obs_source_t *target = obs_filter_get_target(f->context);
	uint32_t width = target ? obs_source_get_base_width(target) : 0;
	uint32_t height = target ? obs_source_get_base_height(target) : 0;

	if (!f->effect || !width || !height || f->rendering) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	f->rendering = true;
	bool captured = capture_input(f, width, height);
	f->rendering = false;

	gs_texture_t *tex = captured ? gs_texrender_get_texture(f->input) : NULL;
	if (!tex)
		return;

	uint32_t out_width = width + f->pad * 2;
	uint32_t out_height = height + f->pad * 2;

	struct vec2 source_size, output_size, offset;
	vec2_set(&source_size, (float)width, (float)height);
	vec2_set(&output_size, (float)out_width, (float)out_height);
	vec2_set(&offset, f->shadow_x, f->shadow_y);

	gs_effect_set_texture(f->param_image, tex);
	gs_effect_set_vec2(f->param_source_size, &source_size);
	gs_effect_set_vec2(f->param_output_size, &output_size);
	gs_effect_set_float(f->param_pad, (float)f->pad);
	gs_effect_set_float(f->param_radius, f->radius);
	gs_effect_set_float(f->param_border_width, f->border_width);
	gs_effect_set_vec4(f->param_border_color, &f->border_color);
	gs_effect_set_vec4(f->param_shadow_color, &f->shadow_color);
	gs_effect_set_vec2(f->param_shadow_offset, &offset);
	gs_effect_set_float(f->param_shadow_blur, f->shadow_blur);
	gs_effect_set_float(f->param_shadow_opacity, f->pad ? f->shadow_opacity : 0.0f);

	/* El shader entrega alfa premultiplicado. */
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	while (gs_effect_loop(f->effect, "Draw"))
		gs_draw_sprite(tex, 0, out_width, out_height);

	gs_blend_state_pop();
}

static obs_properties_t *frame_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *radius =
		obs_properties_add_int_slider(props, S_RADIUS, obs_module_text("ACFX.Frame.Radius"), 0, 500, 1);
	obs_property_int_set_suffix(radius, " px");

	obs_properties_t *border = obs_properties_create();
	obs_property_t *border_width = obs_properties_add_int_slider(
		border, S_BORDER_WIDTH, obs_module_text("ACFX.Frame.Border.Width"), 0, 50, 1);
	obs_property_int_set_suffix(border_width, " px");
	obs_properties_add_color(border, S_BORDER_COLOR, obs_module_text("ACFX.Frame.Border.Color"));
	obs_properties_add_group(props, S_BORDER, obs_module_text("ACFX.Frame.Border"), OBS_GROUP_NORMAL, border);

	obs_properties_t *shadow = obs_properties_create();
	obs_properties_add_color(shadow, S_SHADOW_COLOR, obs_module_text("ACFX.Frame.Shadow.Color"));
	obs_property_t *opacity = obs_properties_add_int_slider(
		shadow, S_SHADOW_OPACITY, obs_module_text("ACFX.Frame.Shadow.Opacity"), 0, 100, 1);
	obs_property_int_set_suffix(opacity, " %");
	obs_property_t *blur = obs_properties_add_int_slider(shadow, S_SHADOW_BLUR,
							     obs_module_text("ACFX.Frame.Shadow.Blur"), 0, 200, 1);
	obs_property_int_set_suffix(blur, " px");
	obs_property_t *x = obs_properties_add_int_slider(shadow, S_SHADOW_X,
							  obs_module_text("ACFX.Frame.Shadow.OffsetX"), -200, 200, 1);
	obs_property_int_set_suffix(x, " px");
	obs_property_t *y = obs_properties_add_int_slider(shadow, S_SHADOW_Y,
							  obs_module_text("ACFX.Frame.Shadow.OffsetY"), -200, 200, 1);
	obs_property_int_set_suffix(y, " px");

	obs_property_t *group = obs_properties_add_group(props, S_SHADOW, obs_module_text("ACFX.Frame.Shadow"),
							 OBS_GROUP_CHECKABLE, shadow);
	obs_property_set_long_description(group, obs_module_text("ACFX.Frame.Shadow.Tip"));

	return props;
}

static void frame_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_RADIUS, 24);
	obs_data_set_default_int(settings, S_BORDER_WIDTH, 0);
	obs_data_set_default_int(settings, S_BORDER_COLOR, 0xFFFFE84D); /* cian #4DE8FF */
	obs_data_set_default_bool(settings, S_SHADOW, true);
	obs_data_set_default_int(settings, S_SHADOW_COLOR, 0xFF000000);
	obs_data_set_default_int(settings, S_SHADOW_OPACITY, 55);
	obs_data_set_default_int(settings, S_SHADOW_BLUR, 30);
	obs_data_set_default_int(settings, S_SHADOW_X, 0);
	obs_data_set_default_int(settings, S_SHADOW_Y, 12);
}

struct obs_source_info ac_super_fx_frame_filter = {
	.id = "ac_super_fx_frame_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = frame_get_name,
	.create = frame_create,
	.destroy = frame_destroy,
	.update = frame_update,
	.get_width = frame_get_width,
	.get_height = frame_get_height,
	.video_render = frame_render,
	.get_properties = frame_properties,
	.get_defaults = frame_defaults,
};
