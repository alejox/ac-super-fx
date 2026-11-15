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

#define S_STYLE "style"
#define S_COLORS "colors"
#define S_COLOR_SOURCE "color_source"
#define S_COLOR1 "color1"
#define S_COLOR2 "color2"
#define S_COLOR3 "color3"
#define S_SPEED "speed"
#define S_SCALE "scale"
#define S_OPACITY "opacity"
#define S_TRANSPARENT "transparent"
#define S_WIDTH "width"
#define S_HEIGHT "height"

#define STYLE_SMOKE 0
#define STYLE_GRADIENT 1

#define COLORS_FIXED 0
#define COLORS_SOURCE 1
#define COLORS_MUSIC 2

#define PROBE_SIZE 32
#define HUE_BINS 12

/* Cada cuanto se refrescan los colores en vivo (segundos). */
#define SOURCE_INTERVAL 0.2f
#define MUSIC_INTERVAL 0.25f

/* Contrato con Music Now Playing: procedimiento global de OBS.
 * Colores como enteros 0xRRGGBB. */
#define MUSIC_PROC "music_now_playing_get_palette"

struct flow_source {
	obs_source_t *context;

	gs_effect_t *effect;
	gs_eparam_t *param_time;
	gs_eparam_t *param_color1;
	gs_eparam_t *param_color2;
	gs_eparam_t *param_color3;
	gs_eparam_t *param_scale;
	gs_eparam_t *param_aspect;
	gs_eparam_t *param_opacity;
	gs_eparam_t *param_transparent;

	gs_texrender_t *probe;
	gs_stagesurf_t *stage;

	uint32_t width;
	uint32_t height;
	int style;
	int colors;
	float speed;
	float scale;
	float opacity;
	bool transparent;

	obs_weak_source_t *color_source;

	struct vec4 fixed[3];   /* colores elegidos por el usuario */
	struct vec4 target[3];  /* hacia donde va la paleta */
	struct vec4 current[3]; /* lo que se dibuja (suavizado) */

	float time;
	float since_refresh;
	bool want_probe;
	bool probing;
};

static const char *flow_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ACFX.Flow");
}

static void color_from_setting(struct vec4 *out, obs_data_t *settings, const char *name)
{
	/* OBS guarda los colores como 0xAABBGGRR. */
	vec4_from_rgba(out, (uint32_t)obs_data_get_int(settings, name));
	out->w = 1.0f;
}

static void color_from_rgb24(struct vec4 *out, long long rgb)
{
	vec4_set(out, (float)((rgb >> 16) & 0xFF) / 255.0f, (float)((rgb >> 8) & 0xFF) / 255.0f,
		 (float)(rgb & 0xFF) / 255.0f, 1.0f);
}

static void use_fixed_colors(struct flow_source *s)
{
	for (int i = 0; i < 3; i++)
		vec4_copy(&s->target[i], &s->fixed[i]);
}

static void flow_update(void *data, obs_data_t *settings)
{
	struct flow_source *s = data;

	s->width = (uint32_t)obs_data_get_int(settings, S_WIDTH);
	s->height = (uint32_t)obs_data_get_int(settings, S_HEIGHT);
	s->style = (int)obs_data_get_int(settings, S_STYLE);
	s->colors = (int)obs_data_get_int(settings, S_COLORS);
	s->speed = (float)obs_data_get_int(settings, S_SPEED) / 100.0f;
	s->scale = (float)obs_data_get_double(settings, S_SCALE);
	s->opacity = (float)obs_data_get_int(settings, S_OPACITY) / 100.0f;
	s->transparent = obs_data_get_bool(settings, S_TRANSPARENT);

	color_from_setting(&s->fixed[0], settings, S_COLOR1);
	color_from_setting(&s->fixed[1], settings, S_COLOR2);
	color_from_setting(&s->fixed[2], settings, S_COLOR3);

	/* La fuente de color se usa desde el hilo grafico: se cambia con el bloqueo tomado. */
	obs_weak_source_t *next = NULL;
	const char *name = obs_data_get_string(settings, S_COLOR_SOURCE);
	if (s->colors == COLORS_SOURCE && name && *name) {
		obs_source_t *src = obs_get_source_by_name(name);
		if (src && src != s->context)
			next = obs_source_get_weak_source(src);
		obs_source_release(src);
	}

	obs_enter_graphics();
	obs_weak_source_t *previous = s->color_source;
	s->color_source = next;
	obs_leave_graphics();
	obs_weak_source_release(previous);

	/* Hasta que llegue una paleta en vivo se usan los colores fijos. */
	use_fixed_colors(s);
	s->since_refresh = 1000.0f;
}

static void *flow_create(obs_data_t *settings, obs_source_t *context)
{
	struct flow_source *s = bzalloc(sizeof(*s));
	s->context = context;

	char *path = obs_module_file("effects/flow.effect");
	char *error = NULL;

	obs_enter_graphics();
	s->effect = path ? gs_effect_create_from_file(path, &error) : NULL;
	if (s->effect) {
		s->param_time = gs_effect_get_param_by_name(s->effect, "time");
		s->param_color1 = gs_effect_get_param_by_name(s->effect, "color1");
		s->param_color2 = gs_effect_get_param_by_name(s->effect, "color2");
		s->param_color3 = gs_effect_get_param_by_name(s->effect, "color3");
		s->param_scale = gs_effect_get_param_by_name(s->effect, "scale");
		s->param_aspect = gs_effect_get_param_by_name(s->effect, "aspect");
		s->param_opacity = gs_effect_get_param_by_name(s->effect, "opacity");
		s->param_transparent = gs_effect_get_param_by_name(s->effect, "transparent");
	}
	s->probe = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	s->stage = gs_stagesurface_create(PROBE_SIZE, PROBE_SIZE, GS_RGBA);
	obs_leave_graphics();

	if (!s->effect)
		obs_log(LOG_ERROR, "could not load flow.effect: %s", error ? error : "(unknown error)");

	bfree(error);
	bfree(path);

	flow_update(s, settings);
	for (int i = 0; i < 3; i++)
		vec4_copy(&s->current[i], &s->target[i]);

	return s;
}

static void flow_destroy(void *data)
{
	struct flow_source *s = data;

	obs_enter_graphics();
	gs_effect_destroy(s->effect);
	gs_texrender_destroy(s->probe);
	gs_stagesurface_destroy(s->stage);
	obs_leave_graphics();

	obs_weak_source_release(s->color_source);
	bfree(s);
}

static uint32_t flow_get_width(void *data)
{
	struct flow_source *s = data;
	return s->width;
}

static uint32_t flow_get_height(void *data)
{
	struct flow_source *s = data;
	return s->height;
}

/* Pregunta la paleta a Music Now Playing. Si el plugin no esta o no
 * suena nada, se queda con los colores fijos. */
static void poll_music_palette(struct flow_source *s)
{
	calldata_t cd;
	calldata_init(&cd);

	bool ok = proc_handler_call(obs_get_proc_handler(), MUSIC_PROC, &cd);
	if (ok && calldata_bool(&cd, "playing")) {
		color_from_rgb24(&s->target[0], calldata_int(&cd, "accent"));
		color_from_rgb24(&s->target[1], calldata_int(&cd, "tint"));
		color_from_rgb24(&s->target[2], calldata_int(&cd, "background"));
	} else {
		use_fixed_colors(s);
	}

	calldata_free(&cd);
}

static void flow_tick(void *data, float seconds)
{
	struct flow_source *s = data;

	s->time += seconds * s->speed;
	if (s->time > 3600.0f)
		s->time -= 3600.0f;

	s->since_refresh += seconds;

	if (s->colors == COLORS_MUSIC && s->since_refresh >= MUSIC_INTERVAL) {
		s->since_refresh = 0.0f;
		poll_music_palette(s);
	} else if (s->colors == COLORS_SOURCE && s->since_refresh >= SOURCE_INTERVAL) {
		s->since_refresh = 0.0f;
		s->want_probe = true;
	}

	/* Transicion suave hacia la paleta nueva (~1 s). */
	float k = seconds * 3.0f;
	if (k > 1.0f)
		k = 1.0f;
	for (int i = 0; i < 3; i++) {
		s->current[i].x += (s->target[i].x - s->current[i].x) * k;
		s->current[i].y += (s->target[i].y - s->current[i].y) * k;
		s->current[i].z += (s->target[i].z - s->current[i].z) * k;
		s->current[i].w = 1.0f;
	}
}

/* Saca tres colores de una imagen pequena RGBA:
 *   0 = el tono mas vivo, 1 = el segundo tono vivo, 2 = el promedio oscurecido. */
static void pick_colors(struct flow_source *s, const uint8_t *data, uint32_t linesize)
{
	float bin_weight[HUE_BINS] = {0};
	float bin_rgb[HUE_BINS][3] = {{0}};
	float avg[3] = {0};
	float count = 0.0f;

	for (uint32_t y = 0; y < PROBE_SIZE; y++) {
		const uint8_t *row = data + (size_t)y * linesize;
		for (uint32_t x = 0; x < PROBE_SIZE; x++) {
			const uint8_t *px = row + x * 4;
			if (px[3] < 128)
				continue;

			float r = px[0] / 255.0f, g = px[1] / 255.0f, b = px[2] / 255.0f;
			float hi = fmaxf(r, fmaxf(g, b));
			float lo = fminf(r, fminf(g, b));
			float delta = hi - lo;

			avg[0] += r;
			avg[1] += g;
			avg[2] += b;
			count += 1.0f;

			float sat = hi > 0.0f ? delta / hi : 0.0f;
			if (sat < 0.25f || hi < 0.2f)
				continue;

			float hue;
			if (hi == r)
				hue = fmodf((g - b) / delta, 6.0f);
			else if (hi == g)
				hue = (b - r) / delta + 2.0f;
			else
				hue = (r - g) / delta + 4.0f;
			if (hue < 0.0f)
				hue += 6.0f;

			int bin = (int)(hue / 6.0f * HUE_BINS) % HUE_BINS;
			float weight = sat * hi;
			bin_weight[bin] += weight;
			bin_rgb[bin][0] += r * weight;
			bin_rgb[bin][1] += g * weight;
			bin_rgb[bin][2] += b * weight;
		}
	}

	if (count < 1.0f) {
		use_fixed_colors(s);
		return;
	}

	avg[0] /= count;
	avg[1] /= count;
	avg[2] /= count;

	int first = -1, second = -1;
	for (int i = 0; i < HUE_BINS; i++) {
		if (bin_weight[i] > 0.0f && (first < 0 || bin_weight[i] > bin_weight[first]))
			first = i;
	}
	for (int i = 0; first >= 0 && i < HUE_BINS; i++) {
		int dist = abs(i - first);
		if (dist > HUE_BINS / 2)
			dist = HUE_BINS - dist;
		if (dist < 2 || bin_weight[i] <= 0.0f)
			continue;
		if (second < 0 || bin_weight[i] > bin_weight[second])
			second = i;
	}

	/* Imagen sin color (gris): el promedio hace de color principal. */
	if (first < 0 || bin_weight[first] < count * 0.01f) {
		vec4_set(&s->target[0], avg[0], avg[1], avg[2], 1.0f);
		vec4_set(&s->target[1], avg[0] * 0.7f, avg[1] * 0.7f, avg[2] * 0.7f, 1.0f);
	} else {
		float w = bin_weight[first];
		vec4_set(&s->target[0], bin_rgb[first][0] / w, bin_rgb[first][1] / w, bin_rgb[first][2] / w, 1.0f);

		if (second >= 0 && bin_weight[second] > bin_weight[first] * 0.15f) {
			w = bin_weight[second];
			vec4_set(&s->target[1], bin_rgb[second][0] / w, bin_rgb[second][1] / w, bin_rgb[second][2] / w,
				 1.0f);
		} else {
			/* Un solo tono: el segundo color es el mismo mezclado con el promedio. */
			vec4_set(&s->target[1], (s->target[0].x + avg[0]) * 0.5f, (s->target[0].y + avg[1]) * 0.5f,
				 (s->target[0].z + avg[2]) * 0.5f, 1.0f);
		}
	}

	vec4_set(&s->target[2], avg[0] * 0.3f, avg[1] * 0.3f, avg[2] * 0.3f, 1.0f);
}

/* Dibuja la fuente elegida en una textura diminuta y lee sus colores. */
static void probe_color_source(struct flow_source *s)
{
	obs_source_t *src = obs_weak_source_get_source(s->color_source);
	if (!src) {
		use_fixed_colors(s);
		return;
	}

	uint32_t width = obs_source_get_width(src);
	uint32_t height = obs_source_get_height(src);

	if (width && height && s->probe && s->stage) {
		s->probing = true;

		gs_texrender_reset(s->probe);
		if (gs_texrender_begin(s->probe, PROBE_SIZE, PROBE_SIZE)) {
			struct vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			obs_source_video_render(src);
			gs_blend_state_pop();

			gs_texrender_end(s->probe);

			gs_texture_t *tex = gs_texrender_get_texture(s->probe);
			uint8_t *pixels = NULL;
			uint32_t linesize = 0;
			if (tex) {
				gs_stage_texture(s->stage, tex);
				if (gs_stagesurface_map(s->stage, &pixels, &linesize)) {
					pick_colors(s, pixels, linesize);
					gs_stagesurface_unmap(s->stage);
				}
			}
		}

		s->probing = false;
	} else {
		use_fixed_colors(s);
	}

	obs_source_release(src);
}

static void flow_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct flow_source *s = data;

	if (!s->effect || !s->width || !s->height)
		return;

	/* s->probing evita un bucle si la fuente de color nos contiene. */
	if (s->want_probe && !s->probing) {
		s->want_probe = false;
		probe_color_source(s);
	}

	gs_effect_set_float(s->param_time, s->time);
	gs_effect_set_vec4(s->param_color1, &s->current[0]);
	gs_effect_set_vec4(s->param_color2, &s->current[1]);
	gs_effect_set_vec4(s->param_color3, &s->current[2]);
	gs_effect_set_float(s->param_scale, s->scale);
	gs_effect_set_float(s->param_aspect, (float)s->width / (float)s->height);
	gs_effect_set_float(s->param_opacity, s->opacity);
	gs_effect_set_float(s->param_transparent, s->transparent ? 1.0f : 0.0f);

	/* El shader entrega alfa premultiplicado. */
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	while (gs_effect_loop(s->effect, s->style == STYLE_GRADIENT ? "Gradient" : "Smoke"))
		gs_draw_sprite(NULL, 0, s->width, s->height);

	gs_blend_state_pop();
}

static bool colors_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	bool from_source = obs_data_get_int(settings, S_COLORS) == COLORS_SOURCE;
	obs_property_set_visible(obs_properties_get(props, S_COLOR_SOURCE), from_source);
	return true;
}

struct source_list {
	obs_property_t *list;
	obs_source_t *self;
};

static bool add_color_source(void *param, obs_source_t *source)
{
	struct source_list *ctx = param;

	if (source == ctx->self || !(obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO))
		return true;

	const char *name = obs_source_get_name(source);
	if (name && *name)
		obs_property_list_add_string(ctx->list, name, name);
	return true;
}

static obs_properties_t *flow_properties(void *data)
{
	struct flow_source *s = data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *style = obs_properties_add_list(props, S_STYLE, obs_module_text("ACFX.Flow.Style"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(style, obs_module_text("ACFX.Flow.Style.Smoke"), STYLE_SMOKE);
	obs_property_list_add_int(style, obs_module_text("ACFX.Flow.Style.Gradient"), STYLE_GRADIENT);

	obs_property_t *colors = obs_properties_add_list(props, S_COLORS, obs_module_text("ACFX.Flow.Colors"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(colors, obs_module_text("ACFX.Flow.Colors.Fixed"), COLORS_FIXED);
	obs_property_list_add_int(colors, obs_module_text("ACFX.Flow.Colors.Source"), COLORS_SOURCE);
	obs_property_list_add_int(colors, obs_module_text("ACFX.Flow.Colors.Music"), COLORS_MUSIC);
	obs_property_set_long_description(colors, obs_module_text("ACFX.Flow.Colors.Tip"));
	obs_property_set_modified_callback(colors, colors_modified);

	obs_property_t *list = obs_properties_add_list(props, S_COLOR_SOURCE, obs_module_text("ACFX.Flow.ColorSource"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	struct source_list ctx = {list, s ? s->context : NULL};
	obs_enum_sources(add_color_source, &ctx);
	obs_enum_scenes(add_color_source, &ctx);

	obs_properties_add_color(props, S_COLOR1, obs_module_text("ACFX.Flow.Color1"));
	obs_properties_add_color(props, S_COLOR2, obs_module_text("ACFX.Flow.Color2"));
	obs_properties_add_color(props, S_COLOR3, obs_module_text("ACFX.Flow.Color3"));

	obs_property_t *speed =
		obs_properties_add_int_slider(props, S_SPEED, obs_module_text("ACFX.Flow.Speed"), 0, 300, 5);
	obs_property_int_set_suffix(speed, " %");

	obs_properties_add_float_slider(props, S_SCALE, obs_module_text("ACFX.Flow.Scale"), 0.5, 8.0, 0.1);

	obs_property_t *opacity =
		obs_properties_add_int_slider(props, S_OPACITY, obs_module_text("ACFX.Flow.Opacity"), 0, 100, 1);
	obs_property_int_set_suffix(opacity, " %");

	obs_properties_add_bool(props, S_TRANSPARENT, obs_module_text("ACFX.Flow.Transparent"));

	obs_properties_add_int(props, S_WIDTH, obs_module_text("ACFX.Flow.Width"), 16, 8192, 1);
	obs_properties_add_int(props, S_HEIGHT, obs_module_text("ACFX.Flow.Height"), 16, 8192, 1);

	return props;
}

static void flow_defaults(obs_data_t *settings)
{
	struct obs_video_info ovi;
	bool have_video = obs_get_video_info(&ovi);

	obs_data_set_default_int(settings, S_WIDTH, have_video ? ovi.base_width : 1920);
	obs_data_set_default_int(settings, S_HEIGHT, have_video ? ovi.base_height : 1080);
	obs_data_set_default_int(settings, S_STYLE, STYLE_SMOKE);
	obs_data_set_default_int(settings, S_COLORS, COLORS_FIXED);
	/* 0xAABBGGRR: cian #4DE8FF, ambar #FFB454, casi negro #0B0E18 */
	obs_data_set_default_int(settings, S_COLOR1, 0xFFFFE84D);
	obs_data_set_default_int(settings, S_COLOR2, 0xFF54B4FF);
	obs_data_set_default_int(settings, S_COLOR3, 0xFF180E0B);
	obs_data_set_default_int(settings, S_SPEED, 100);
	obs_data_set_default_double(settings, S_SCALE, 2.5);
	obs_data_set_default_int(settings, S_OPACITY, 100);
	obs_data_set_default_bool(settings, S_TRANSPARENT, false);
}

struct obs_source_info ac_super_fx_flow_source = {
	.id = "ac_super_fx_flow_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.icon_type = OBS_ICON_TYPE_COLOR,
	.get_name = flow_get_name,
	.create = flow_create,
	.destroy = flow_destroy,
	.update = flow_update,
	.get_width = flow_get_width,
	.get_height = flow_get_height,
	.video_tick = flow_tick,
	.video_render = flow_render,
	.get_properties = flow_properties,
	.get_defaults = flow_defaults,
};
