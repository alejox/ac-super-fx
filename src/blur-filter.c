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

#define S_TYPE "type"
#define S_RADIUS "radius"

#define TYPE_GAUSSIAN 0
#define TYPE_BOX 1

#define MAX_RADIUS 96.0

struct blur_filter {
	obs_source_t *context;

	gs_effect_t *effect;
	gs_eparam_t *param_image;
	gs_eparam_t *param_texel_step;
	gs_eparam_t *param_radius;

	gs_texrender_t *input;  /* fuente original */
	gs_texrender_t *pass_h; /* tras la pasada horizontal */
	gs_texrender_t *pass_v; /* tras la pasada vertical (resultado) */

	int type;
	float radius;
	bool rendering;
};

static const char *blur_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ACFX.Blur");
}

static void blur_update(void *data, obs_data_t *settings)
{
	struct blur_filter *f = data;

	f->type = (int)obs_data_get_int(settings, S_TYPE);
	f->radius = (float)obs_data_get_double(settings, S_RADIUS);
}

static void *blur_create(obs_data_t *settings, obs_source_t *context)
{
	struct blur_filter *f = bzalloc(sizeof(*f));
	f->context = context;

	char *path = obs_module_file("effects/blur.effect");
	char *error = NULL;

	obs_enter_graphics();
	f->effect = path ? gs_effect_create_from_file(path, &error) : NULL;
	if (f->effect) {
		f->param_image = gs_effect_get_param_by_name(f->effect, "image");
		f->param_texel_step = gs_effect_get_param_by_name(f->effect, "texel_step");
		f->param_radius = gs_effect_get_param_by_name(f->effect, "radius");
	}
	f->input = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->pass_h = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->pass_v = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();

	if (!f->effect)
		obs_log(LOG_ERROR, "could not load blur.effect: %s", error ? error : "(unknown error)");

	bfree(error);
	bfree(path);

	blur_update(f, settings);
	return f;
}

static void blur_destroy(void *data)
{
	struct blur_filter *f = data;

	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	gs_texrender_destroy(f->input);
	gs_texrender_destroy(f->pass_h);
	gs_texrender_destroy(f->pass_v);
	obs_leave_graphics();

	bfree(f);
}

/* Dibuja la fuente (con los filtros anteriores) dentro de f->input. */
static bool capture_input(struct blur_filter *f, uint32_t width, uint32_t height)
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

/* Una pasada de blur en una direccion: src -> dst. */
static bool blur_pass(struct blur_filter *f, gs_texture_t *src, gs_texrender_t *dst, uint32_t width, uint32_t height,
		      float step_x, float step_y)
{
	const char *technique = f->type == TYPE_BOX ? "Box" : "Gaussian";
	struct vec2 step;
	vec2_set(&step, step_x, step_y);

	gs_texrender_reset(dst);
	if (!gs_texrender_begin(dst, width, height))
		return false;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

	gs_effect_set_texture(f->param_image, src);
	gs_effect_set_vec2(f->param_texel_step, &step);
	gs_effect_set_float(f->param_radius, f->radius);

	while (gs_effect_loop(f->effect, technique))
		gs_draw_sprite(src, 0, width, height);

	gs_texrender_end(dst);
	return true;
}

static void blur_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct blur_filter *f = data;

	obs_source_t *target = obs_filter_get_target(f->context);
	uint32_t width = target ? obs_source_get_base_width(target) : 0;
	uint32_t height = target ? obs_source_get_base_height(target) : 0;

	if (!f->effect || !width || !height || f->radius < 0.01f || f->rendering) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	f->rendering = true;

	if (!capture_input(f, width, height)) {
		f->rendering = false;
		return;
	}

	gs_blend_state_push();
	gs_reset_blend_state();
	gs_enable_blending(false);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	bool ok =
		blur_pass(f, gs_texrender_get_texture(f->input), f->pass_h, width, height, 1.0f / (float)width, 0.0f) &&
		blur_pass(f, gs_texrender_get_texture(f->pass_h), f->pass_v, width, height, 0.0f, 1.0f / (float)height);

	gs_blend_state_pop();

	/* Si algo fallo, se muestra la fuente sin desenfocar. */
	gs_texture_t *result = gs_texrender_get_texture(ok ? f->pass_v : f->input);
	if (result) {
		gs_effect_t *draw = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = gs_effect_get_param_by_name(draw, "image");
		gs_effect_set_texture(image, result);

		/* Las texturas llevan alfa premultiplicado. */
		gs_blend_state_push();
		gs_enable_blending(true);
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

		while (gs_effect_loop(draw, "Draw"))
			gs_draw_sprite(result, 0, width, height);

		gs_blend_state_pop();
	}

	f->rendering = false;
}

static obs_properties_t *blur_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *type = obs_properties_add_list(props, S_TYPE, obs_module_text("ACFX.Blur.Type"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(type, obs_module_text("ACFX.Blur.Type.Gaussian"), TYPE_GAUSSIAN);
	obs_property_list_add_int(type, obs_module_text("ACFX.Blur.Type.Box"), TYPE_BOX);

	obs_property_t *radius = obs_properties_add_float_slider(props, S_RADIUS, obs_module_text("ACFX.Blur.Radius"),
								 0.0, MAX_RADIUS, 0.5);
	obs_property_float_set_suffix(radius, " px");

	return props;
}

static void blur_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_TYPE, TYPE_GAUSSIAN);
	obs_data_set_default_double(settings, S_RADIUS, 12.0);
}

struct obs_source_info ac_super_fx_blur_filter = {
	.id = "ac_super_fx_blur_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = blur_get_name,
	.create = blur_create,
	.destroy = blur_destroy,
	.update = blur_update,
	.video_render = blur_render,
	.get_properties = blur_properties,
	.get_defaults = blur_defaults,
};
