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

#define S_PRESET "preset"
#define S_TYPE "type"
#define S_RADIUS "radius"
#define S_ANGLE "angle"
#define S_AREA "area"
#define S_REGION "region_group"
#define S_REGION_X "region_x"
#define S_REGION_Y "region_y"
#define S_REGION_W "region_width"
#define S_REGION_H "region_height"
#define S_FEATHER "region_feather"
#define S_INVERT "region_invert"

#define TYPE_GAUSSIAN 0
#define TYPE_BOX 1
#define TYPE_PIXELATE 2
#define TYPE_MOTION 3
#define TYPE_ZOOM 4

#define AREA_FULL 0
#define AREA_REGION 1

#define PRESET_CUSTOM 0
#define PRESET_SOFT 1
#define PRESET_MEDIUM 2
#define PRESET_STRONG 3
#define PRESET_CENSOR 4

#define MAX_RADIUS 96

static const int preset_radius[] = {0, 8, 20, 40, 64};

struct blur_filter {
	obs_source_t *context;

	gs_effect_t *effect;
	gs_eparam_t *param_image;
	gs_eparam_t *param_texel_step;
	gs_eparam_t *param_radius;
	gs_eparam_t *param_image_size;
	gs_eparam_t *param_original;
	gs_eparam_t *param_region;
	gs_eparam_t *param_feather;
	gs_eparam_t *param_invert;

	gs_texrender_t *input;  /* fuente original */
	gs_texrender_t *pass_h; /* tras la pasada horizontal */
	gs_texrender_t *pass_v; /* tras la pasada vertical (resultado) */

	int type;
	float radius;
	float angle; /* radianes, para el estilo Movimiento */

	bool use_region;
	float region_x; /* 0..1 */
	float region_y;
	float region_w;
	float region_h;
	float feather; /* pixeles */
	bool invert;

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
	f->radius = (float)obs_data_get_int(settings, S_RADIUS);
	f->angle = (float)obs_data_get_int(settings, S_ANGLE) * 0.01745329252f;

	f->use_region = obs_data_get_int(settings, S_AREA) == AREA_REGION;
	f->region_x = (float)obs_data_get_double(settings, S_REGION_X) / 100.0f;
	f->region_y = (float)obs_data_get_double(settings, S_REGION_Y) / 100.0f;
	f->region_w = (float)obs_data_get_double(settings, S_REGION_W) / 100.0f;
	f->region_h = (float)obs_data_get_double(settings, S_REGION_H) / 100.0f;
	f->feather = (float)obs_data_get_int(settings, S_FEATHER);
	f->invert = obs_data_get_bool(settings, S_INVERT);
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
		f->param_image_size = gs_effect_get_param_by_name(f->effect, "image_size");
		f->param_original = gs_effect_get_param_by_name(f->effect, "original");
		f->param_region = gs_effect_get_param_by_name(f->effect, "region");
		f->param_feather = gs_effect_get_param_by_name(f->effect, "feather_uv");
		f->param_invert = gs_effect_get_param_by_name(f->effect, "invert");
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

/* Una pasada del efecto: src -> dst. */
static bool blur_pass(struct blur_filter *f, const char *technique, gs_texture_t *src, gs_texrender_t *dst,
		      uint32_t width, uint32_t height, float step_x, float step_y)
{
	struct vec2 step, size;
	vec2_set(&step, step_x, step_y);
	vec2_set(&size, (float)width, (float)height);

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
	gs_effect_set_vec2(f->param_image_size, &size);

	while (gs_effect_loop(f->effect, technique))
		gs_draw_sprite(src, 0, width, height);

	gs_texrender_end(dst);
	return true;
}

/* Dibuja el resultado final en el destino actual de OBS. */
static void draw_result(struct blur_filter *f, bool blurred, uint32_t width, uint32_t height)
{
	gs_texture_t *sharp = gs_texrender_get_texture(f->input);
	gs_texture_t *result = blurred ? gs_texrender_get_texture(f->pass_v) : sharp;
	if (!result)
		return;

	/* Las texturas llevan alfa premultiplicado. */
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	if (blurred && f->use_region && sharp) {
		struct vec4 region;
		struct vec2 feather;
		vec4_set(&region, f->region_x, f->region_y, f->region_x + f->region_w, f->region_y + f->region_h);
		vec2_set(&feather, f->feather / (float)width, f->feather / (float)height);

		gs_effect_set_texture(f->param_image, result);
		gs_effect_set_texture(f->param_original, sharp);
		gs_effect_set_vec4(f->param_region, &region);
		gs_effect_set_vec2(f->param_feather, &feather);
		gs_effect_set_float(f->param_invert, f->invert ? 1.0f : 0.0f);

		while (gs_effect_loop(f->effect, "Region"))
			gs_draw_sprite(result, 0, width, height);
	} else {
		gs_effect_t *draw = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = gs_effect_get_param_by_name(draw, "image");
		gs_effect_set_texture(image, result);

		while (gs_effect_loop(draw, "Draw"))
			gs_draw_sprite(result, 0, width, height);
	}

	gs_blend_state_pop();
}

static void blur_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct blur_filter *f = data;

	obs_source_t *target = obs_filter_get_target(f->context);
	uint32_t width = target ? obs_source_get_base_width(target) : 0;
	uint32_t height = target ? obs_source_get_base_height(target) : 0;

	if (!f->effect || !width || !height || f->radius < 1.0f || f->rendering) {
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

	gs_texture_t *input = gs_texrender_get_texture(f->input);
	float tx = 1.0f / (float)width;
	float ty = 1.0f / (float)height;
	bool ok;

	switch (f->type) {
	case TYPE_PIXELATE:
		ok = blur_pass(f, "Pixelate", input, f->pass_v, width, height, 0.0f, 0.0f);
		break;
	case TYPE_ZOOM:
		ok = blur_pass(f, "Zoom", input, f->pass_v, width, height, 0.0f, 0.0f);
		break;
	case TYPE_MOTION:
		/* Una sola pasada gaussiana a lo largo del angulo elegido. */
		ok = blur_pass(f, "Gaussian", input, f->pass_v, width, height, cosf(f->angle) * tx,
			       sinf(f->angle) * ty);
		break;
	default: {
		const char *technique = f->type == TYPE_BOX ? "Box" : "Gaussian";
		ok = blur_pass(f, technique, input, f->pass_h, width, height, tx, 0.0f) &&
		     blur_pass(f, technique, gs_texrender_get_texture(f->pass_h), f->pass_v, width, height, 0.0f, ty);
		break;
	}
	}

	gs_blend_state_pop();

	/* Si algo fallo, se muestra la fuente sin desenfocar. */
	draw_result(f, ok, width, height);

	f->rendering = false;
}

/* Elegir un preajuste mueve la intensidad. */
static bool preset_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	long long preset = obs_data_get_int(settings, S_PRESET);
	if (preset <= PRESET_CUSTOM || preset > PRESET_CENSOR)
		return false;

	if (obs_data_get_int(settings, S_RADIUS) == preset_radius[preset])
		return false;

	obs_data_set_int(settings, S_RADIUS, preset_radius[preset]);
	return true;
}

/* Mover la intensidad a mano pasa el preajuste a "Personalizado". */
static bool radius_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	long long preset = obs_data_get_int(settings, S_PRESET);
	if (preset <= PRESET_CUSTOM || preset > PRESET_CENSOR)
		return false;

	if (obs_data_get_int(settings, S_RADIUS) == preset_radius[preset])
		return false;

	obs_data_set_int(settings, S_PRESET, PRESET_CUSTOM);
	return true;
}

/* El angulo solo aplica al estilo Movimiento. */
static bool type_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	bool motion = obs_data_get_int(settings, S_TYPE) == TYPE_MOTION;
	obs_property_set_visible(obs_properties_get(props, S_ANGLE), motion);
	return true;
}

/* Los controles del rectangulo solo aparecen cuando se usan. */
static bool area_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	bool region = obs_data_get_int(settings, S_AREA) == AREA_REGION;
	obs_property_set_visible(obs_properties_get(props, S_REGION), region);
	return true;
}

static obs_property_t *add_percent(obs_properties_t *props, const char *name, const char *text)
{
	obs_property_t *p = obs_properties_add_float_slider(props, name, obs_module_text(text), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(p, " %");
	return p;
}

static obs_properties_t *blur_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *preset = obs_properties_add_list(props, S_PRESET, obs_module_text("ACFX.Blur.Preset"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(preset, obs_module_text("ACFX.Blur.Preset.Soft"), PRESET_SOFT);
	obs_property_list_add_int(preset, obs_module_text("ACFX.Blur.Preset.Medium"), PRESET_MEDIUM);
	obs_property_list_add_int(preset, obs_module_text("ACFX.Blur.Preset.Strong"), PRESET_STRONG);
	obs_property_list_add_int(preset, obs_module_text("ACFX.Blur.Preset.Censor"), PRESET_CENSOR);
	obs_property_list_add_int(preset, obs_module_text("ACFX.Blur.Preset.Custom"), PRESET_CUSTOM);
	obs_property_set_long_description(preset, obs_module_text("ACFX.Blur.Preset.Tip"));
	obs_property_set_modified_callback(preset, preset_modified);

	obs_property_t *radius =
		obs_properties_add_int_slider(props, S_RADIUS, obs_module_text("ACFX.Blur.Radius"), 0, MAX_RADIUS, 1);
	obs_property_int_set_suffix(radius, " px");
	obs_property_set_long_description(radius, obs_module_text("ACFX.Blur.Radius.Tip"));
	obs_property_set_modified_callback(radius, radius_modified);

	obs_property_t *type = obs_properties_add_list(props, S_TYPE, obs_module_text("ACFX.Blur.Type"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(type, obs_module_text("ACFX.Blur.Type.Gaussian"), TYPE_GAUSSIAN);
	obs_property_list_add_int(type, obs_module_text("ACFX.Blur.Type.Box"), TYPE_BOX);
	obs_property_list_add_int(type, obs_module_text("ACFX.Blur.Type.Pixelate"), TYPE_PIXELATE);
	obs_property_list_add_int(type, obs_module_text("ACFX.Blur.Type.Motion"), TYPE_MOTION);
	obs_property_list_add_int(type, obs_module_text("ACFX.Blur.Type.Zoom"), TYPE_ZOOM);
	obs_property_set_modified_callback(type, type_modified);

	obs_property_t *angle =
		obs_properties_add_int_slider(props, S_ANGLE, obs_module_text("ACFX.Blur.Angle"), 0, 360, 1);
	obs_property_int_set_suffix(angle, "\xC2\xB0");
	obs_property_set_long_description(type, obs_module_text("ACFX.Blur.Type.Tip"));

	obs_property_t *area = obs_properties_add_list(props, S_AREA, obs_module_text("ACFX.Blur.Area"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(area, obs_module_text("ACFX.Blur.Area.Full"), AREA_FULL);
	obs_property_list_add_int(area, obs_module_text("ACFX.Blur.Area.Region"), AREA_REGION);
	obs_property_set_long_description(area, obs_module_text("ACFX.Blur.Area.Tip"));
	obs_property_set_modified_callback(area, area_modified);

	obs_properties_t *region = obs_properties_create();
	add_percent(region, S_REGION_X, "ACFX.Blur.Region.X");
	add_percent(region, S_REGION_Y, "ACFX.Blur.Region.Y");
	add_percent(region, S_REGION_W, "ACFX.Blur.Region.Width");
	add_percent(region, S_REGION_H, "ACFX.Blur.Region.Height");
	obs_property_t *feather = obs_properties_add_int_slider(region, S_FEATHER,
								obs_module_text("ACFX.Blur.Region.Feather"), 0, 200, 1);
	obs_property_int_set_suffix(feather, " px");
	obs_properties_add_bool(region, S_INVERT, obs_module_text("ACFX.Blur.Region.Invert"));

	obs_properties_add_group(props, S_REGION, obs_module_text("ACFX.Blur.Region"), OBS_GROUP_NORMAL, region);

	return props;
}

static void blur_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_PRESET, PRESET_MEDIUM);
	obs_data_set_default_int(settings, S_RADIUS, 20);
	obs_data_set_default_int(settings, S_TYPE, TYPE_GAUSSIAN);
	obs_data_set_default_int(settings, S_ANGLE, 0);
	obs_data_set_default_int(settings, S_AREA, AREA_FULL);
	obs_data_set_default_double(settings, S_REGION_X, 25.0);
	obs_data_set_default_double(settings, S_REGION_Y, 25.0);
	obs_data_set_default_double(settings, S_REGION_W, 50.0);
	obs_data_set_default_double(settings, S_REGION_H, 50.0);
	obs_data_set_default_int(settings, S_FEATHER, 0);
	obs_data_set_default_bool(settings, S_INVERT, false);
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
