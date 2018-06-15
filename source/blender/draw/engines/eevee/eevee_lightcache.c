/*
 * Copyright 2016, Blender Foundation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Blender Institute
 *
 */

/** \file eevee_lightcache.c
 *  \ingroup draw_engine
 *
 * Eevee's indirect lighting cache.
 */

#include "DRW_render.h"

#include "BKE_global.h"

#include "DEG_depsgraph_build.h"
#include "DEG_depsgraph_query.h"

#include "DNA_lightprobe_types.h"
#include "DNA_group_types.h"

#include "PIL_time.h"

#include "eevee_lightcache.h"
#include "eevee_private.h"

#include "../../../intern/gawain/gawain/gwn_context.h"

#include "WM_api.h"

/* Rounded to nearest PowerOfTwo */
#if defined(IRRADIANCE_SH_L2)
#define IRRADIANCE_SAMPLE_SIZE_X 4 /* 3 in reality */
#define IRRADIANCE_SAMPLE_SIZE_Y 4 /* 3 in reality */
#elif defined(IRRADIANCE_CUBEMAP)
#define IRRADIANCE_SAMPLE_SIZE_X 8
#define IRRADIANCE_SAMPLE_SIZE_Y 8
#elif defined(IRRADIANCE_HL2)
#define IRRADIANCE_SAMPLE_SIZE_X 4 /* 3 in reality */
#define IRRADIANCE_SAMPLE_SIZE_Y 2
#endif

#ifdef IRRADIANCE_SH_L2
/* we need a signed format for Spherical Harmonics */
#  define IRRADIANCE_FORMAT GPU_RGBA16F
#else
#  define IRRADIANCE_FORMAT GPU_RGBA8
#endif

#define IRRADIANCE_MAX_POOL_LAYER 256 /* OpenGL 3.3 core requirement, can be extended but it's already very big */
#define IRRADIANCE_MAX_POOL_SIZE 1024
#define MAX_IRRADIANCE_SAMPLES \
        (IRRADIANCE_MAX_POOL_SIZE / IRRADIANCE_SAMPLE_SIZE_X) * \
        (IRRADIANCE_MAX_POOL_SIZE / IRRADIANCE_SAMPLE_SIZE_Y)

/* TODO should be replace by a more elegant alternative. */
extern void DRW_opengl_context_enable(void);
extern void DRW_opengl_context_disable(void);

extern void DRW_opengl_render_context_enable(void *re_gl_context);
extern void DRW_opengl_render_context_disable(void *re_gl_context);
extern void DRW_gawain_render_context_enable(void *re_gwn_context);
extern void DRW_gawain_render_context_disable(void *re_gwn_context);

typedef struct EEVEE_LightBake {
	Depsgraph *depsgraph;
	ViewLayer *view_layer;
	Scene *scene;
	struct Main *bmain;

	Object *probe;                   /* Current probe being rendered. */
	GPUTexture *rt_color;            /* Target cube color texture. */
	GPUTexture *rt_depth;            /* Target cube depth texture. */
	GPUFrameBuffer *rt_fb[6];        /* Target cube framebuffers. */
	GPUFrameBuffer *store_fb;        /* Storage framebuffer. */
	int rt_res;                      /* Cube render target resolution. */

	/* Shared */
	int layer;                       /* Target layer to store the data to. */
	float samples_ct, invsamples_ct; /* Sample count for the convolution. */
	float lod_factor;                /* Sampling bias during convolution step. */
	float lod_max;                   /* Max cubemap LOD to sample when convolving. */
	int cube_count, grid_count;      /* Number of probes to render + world probe. */

	/* Irradiance grid */
	int irr_cube_res;                /* Target cubemap at MIP 0. */
	int total_irr_samples;           /* Total for all grids */
	int bounce_curr, bounce_count;   /* The current light bounce being evaluated. */
	float vis_range, vis_blur;       /* Sample Visibility compression and bluring. */
	float vis_res;                   /* Resolution of the Visibility shadowmap. */
	GPUTexture *grid_prev;           /* Result of previous light bounce. */
	LightProbe **grid_prb;           /* Pointer to the id.data of the probe object. */

	/* Reflection probe */
	int ref_cube_res;                /* Target cubemap at MIP 0. */
	float probemat[6][4][4];         /* ViewProjection matrix for each cube face. */
	float texel_size, padding_size;  /* Texel and padding size for the final octahedral map. */
	float roughness;                 /* Roughness level of the current mipmap. */
	LightProbe **cube_prb;           /* Pointer to the id.data of the probe object. */

	/* Dummy Textures */
	struct GPUTexture *dummy_color, *dummy_depth;
	struct GPUTexture *dummy_layer_color;

	void *gl_context, *gwn_context;  /* If running in parallel (in a separate thread), use this context. */
} EEVEE_LightBake;

/* -------------------------------------------------------------------- */

/** \name Light Cache
 * \{ */

static bool EEVEE_lightcache_validate(
        const EEVEE_LightCache *light_cache,
        const SceneEEVEE *UNUSED(eevee),
        const int UNUSED(cube_count),
        const int UNUSED(irr_samples))
{
	if (light_cache) {
		/* TODO if settings and probe count matches... */
		return true;
	}
	return false;
}

EEVEE_LightCache *EEVEE_lightcache_create(
        const SceneEEVEE *UNUSED(eevee),
        const int UNUSED(cube_count),
        const int UNUSED(irr_samples))
{
	EEVEE_LightCache *light_cache = MEM_callocN(sizeof(EEVEE_LightCache), "EEVEE_LightCache");

	float rgba[4] = {1.0f, 0.0f, 0.0f, 1.0f};
	light_cache->grid_tx = DRW_texture_create_2D_array(1, 1, 1, IRRADIANCE_FORMAT, DRW_TEX_FILTER, rgba);
	light_cache->cube_tx = DRW_texture_create_2D_array(1, 1, 1, GPU_RGBA8, DRW_TEX_FILTER, rgba);

	// int irr_size[3];
	// irradiance_pool_size_get(lbake->vis_res, lbake->total_irr_samples, irr_size);

	// light_cache->grid_tx = DRW_texture_create_2D_array(irr_size[0], irr_size[1], irr_size[2], IRRADIANCE_FORMAT, DRW_TEX_FILTER, NULL);
	// light_cache->cube_tx = DRW_texture_create_2D_array(lbake->ref_cube_res, lbake->ref_cube_res, lcache->cube_count, GPU_RGBA8, DRW_TEX_FILTER, NULL);

	return light_cache;
}

void EEVEE_lightcache_free(EEVEE_LightCache *lcache)
{
	DRW_TEXTURE_FREE_SAFE(lcache->cube_tx);
	DRW_TEXTURE_FREE_SAFE(lcache->grid_tx);

	MEM_SAFE_FREE(lcache->cube_data);
	MEM_SAFE_FREE(lcache->grid_data);
	MEM_freeN(lcache);
}

/** \} */


/* -------------------------------------------------------------------- */

/** \name Light Bake Job
 * \{ */

void *EEVEE_lightbake_job_data_alloc(struct Main *bmain, struct ViewLayer *view_layer, struct Scene *scene, bool run_as_job)
{
	EEVEE_LightBake *lbake = MEM_callocN(sizeof(EEVEE_LightBake), "EEVEE_LightBake");

	lbake->depsgraph = DEG_graph_new(scene, view_layer, DAG_EVAL_RENDER);
	lbake->scene = scene;
	lbake->bmain = bmain;

	if (run_as_job) {
		lbake->gl_context = WM_opengl_context_create();
	}

    DEG_graph_relations_update(lbake->depsgraph, bmain, scene, view_layer);

	return lbake;
}

void EEVEE_lightbake_job_data_free(void *custom_data)
{
	EEVEE_LightBake *lbake = (EEVEE_LightBake *)custom_data;

	DEG_graph_free(lbake->depsgraph);

	MEM_SAFE_FREE(lbake->cube_prb);
	MEM_SAFE_FREE(lbake->grid_prb);

	MEM_freeN(lbake);
}

static void irradiance_pool_size_get(int visibility_size, int total_samples, int r_size[3])
{
	/* Compute how many irradiance samples we can store per visibility sample. */
	int irr_per_vis = (visibility_size / IRRADIANCE_SAMPLE_SIZE_X) *
	                  (visibility_size / IRRADIANCE_SAMPLE_SIZE_Y);

	/* The irradiance itself take one layer, hence the +1 */
	int layer_ct = MIN2(irr_per_vis + 1, IRRADIANCE_MAX_POOL_LAYER);

	int texel_ct = (int)ceilf((float)total_samples / (float)(layer_ct - 1));
	r_size[0] = visibility_size * max_ii(1, min_ii(texel_ct, (IRRADIANCE_MAX_POOL_SIZE / visibility_size)));
	r_size[1] = visibility_size * max_ii(1, (texel_ct / (IRRADIANCE_MAX_POOL_SIZE / visibility_size)));
	r_size[2] = layer_ct;
}

static void eevee_lightbake_create_resources(EEVEE_LightBake *lbake)
{
	Scene *scene_eval = DEG_get_evaluated_scene(lbake->depsgraph);
	Scene *scene_orig = lbake->scene;
	const SceneEEVEE *eevee = &scene_eval->eevee;

	lbake->bounce_count = eevee->gi_diffuse_bounces;
	lbake->vis_res      = eevee->gi_visibility_resolution;
	lbake->rt_res       = eevee->gi_cubemap_resolution;

	//lbake->ref_cube_res = octahedral_from_cubemap();
	lbake->ref_cube_res = lbake->rt_res;

	lbake->cube_prb = MEM_callocN(sizeof(LightProbe *) * lbake->cube_count, "EEVEE Cube visgroup ptr");
	lbake->grid_prb = MEM_callocN(sizeof(LightProbe *) * lbake->grid_count, "EEVEE Grid visgroup ptr");

	/* Only one render target for now. */
	lbake->rt_depth = DRW_texture_create_cube(lbake->rt_res, GPU_DEPTH_COMPONENT24, 0, NULL);
	lbake->rt_color = DRW_texture_create_cube(lbake->rt_res, GPU_RGBA16F, DRW_TEX_FILTER | DRW_TEX_MIPMAP, NULL);

	for (int i = 0; i < 6; ++i) {
		GPU_framebuffer_ensure_config(&lbake->rt_fb[i], {
			GPU_ATTACHMENT_TEXTURE_CUBEFACE(lbake->rt_depth, i),
			GPU_ATTACHMENT_TEXTURE_CUBEFACE(lbake->rt_color, i)
		});
	}

	int irr_size[3];
	irradiance_pool_size_get(lbake->vis_res, lbake->total_irr_samples, irr_size);

	lbake->grid_prev = DRW_texture_create_2D_array(irr_size[0], irr_size[1], irr_size[2], IRRADIANCE_FORMAT, DRW_TEX_FILTER, NULL);

	/* Ensure Light Cache is ready to accept new data. If not recreate one.
	 * WARNING: All the following must be threadsafe. It's currently protected
	 * by the DRW mutex. */
	EEVEE_LightCache *lcache = scene_orig->eevee.light_cache;
	SceneEEVEE *sce_eevee = &scene_orig->eevee;

	if (lcache != NULL && !EEVEE_lightcache_validate(lcache, sce_eevee, lbake->cube_count, lbake->total_irr_samples)) {
		EEVEE_lightcache_free(lcache);
		scene_orig->eevee.light_cache = lcache = NULL;
	}

	if (lcache == NULL) {
		lcache = EEVEE_lightcache_create(sce_eevee, lbake->cube_count, lbake->total_irr_samples);
		scene_orig->eevee.light_cache = lcache;
	}

	/* Share light cache with the evaluated (baking) layer and the original layer.
	 * This avoid full scene re-evaluation by depsgraph. */
	scene_eval->eevee.light_cache = lcache;
}

static void eevee_lightbake_delete_resources(EEVEE_LightBake *lbake)
{
	if (lbake->gl_context) {
		DRW_opengl_render_context_enable(lbake->gl_context);
		DRW_gawain_render_context_enable(lbake->gwn_context);
	}
	else {
		DRW_opengl_context_enable();
	}

	DRW_TEXTURE_FREE_SAFE(lbake->rt_depth);
	DRW_TEXTURE_FREE_SAFE(lbake->rt_color);
	DRW_TEXTURE_FREE_SAFE(lbake->grid_prev);
	for (int i = 0; i < 6; ++i) {
		GPU_FRAMEBUFFER_FREE_SAFE(lbake->rt_fb[i]);
	}

	if (lbake->gl_context) {
		/* Delete the baking context. */
		DRW_gawain_render_context_disable(lbake->gwn_context);
		DRW_gawain_render_context_enable(lbake->gwn_context);
		GWN_context_discard(lbake->gwn_context);
		DRW_opengl_render_context_disable(lbake->gl_context);
		WM_opengl_context_dispose(lbake->gl_context);
		lbake->gwn_context = NULL;
		lbake->gl_context = NULL;
	}
	else {
		DRW_opengl_context_disable();
	}
}

static void eevee_lightbake_context_enable(EEVEE_LightBake *lbake)
{
	if (lbake->gl_context) {
		DRW_opengl_render_context_enable(lbake->gl_context);
		if (lbake->gwn_context == NULL) {
			lbake->gwn_context = GWN_context_create();
		}
		DRW_gawain_render_context_enable(lbake->gwn_context);
	}
	else {
		DRW_opengl_context_enable();
	}
}

static void eevee_lightbake_context_disable(EEVEE_LightBake *lbake)
{
	if (lbake->gl_context) {
		DRW_opengl_render_context_disable(lbake->gl_context);
		DRW_gawain_render_context_disable(lbake->gwn_context);
	}
	else {
		DRW_opengl_context_disable();
	}
}

static void eevee_lightbake_render_world(void *ved, void *UNUSED(user_data))
{
	EEVEE_Data *vedata = (EEVEE_Data *)ved;
	EEVEE_StorageList *stl = vedata->stl;
	EEVEE_FramebufferList *fbl = vedata->fbl;
	// EEVEE_LightBake *lbake = user_data;
	EEVEE_ViewLayerData *sldata = EEVEE_view_layer_data_ensure();

	EEVEE_materials_init(sldata, stl, fbl);
	EEVEE_lights_init(sldata);
	EEVEE_lightprobes_init(sldata, vedata);

	EEVEE_lightprobes_cache_init(sldata, vedata);
	EEVEE_lightprobes_refresh_world(sldata, vedata);
}

static void eevee_lightbake_render_probe(void *ved, void *UNUSED(user_data))
{
	EEVEE_Data *vedata = (EEVEE_Data *)ved;
	EEVEE_StorageList *stl = vedata->stl;
	EEVEE_FramebufferList *fbl = vedata->fbl;
	// EEVEE_LightBake *lbake = user_data;
	EEVEE_ViewLayerData *sldata = EEVEE_view_layer_data_ensure();

	EEVEE_materials_init(sldata, stl, fbl);
	EEVEE_lights_init(sldata);
	EEVEE_lightprobes_init(sldata, vedata);

	EEVEE_lightprobes_cache_init(sldata, vedata);
	EEVEE_lights_cache_init(sldata, vedata);
	EEVEE_materials_cache_init(sldata, vedata);

	/* Disable specular lighting when rendering probes to avoid feedback loops (looks bad).
	 * Disable AO until we find a way to hide really bad discontinuities between cubefaces. */
	// common_data->spec_toggle = false;
	// common_data->ssr_toggle = false;
	// common_data->sss_toggle = false;
	// common_data->ao_settings = 0.0f;
	// common_data->ao_dist = 0.0f;

	EEVEE_lightprobes_refresh_world(sldata, vedata);
}

static void eevee_lightbake_count_probes(EEVEE_LightBake *lbake)
{
	Depsgraph *depsgraph = lbake->depsgraph;

	/* At least one of each for the world */
	lbake->grid_count = lbake->cube_count = lbake->total_irr_samples = 1;

	DEG_OBJECT_ITER_FOR_RENDER_ENGINE_BEGIN(depsgraph, ob)
	{
		if (ob->type == OB_LIGHTPROBE) {
			LightProbe *prb = (LightProbe *)ob->data;

			if (prb->type == LIGHTPROBE_TYPE_GRID) {
				lbake->total_irr_samples += prb->grid_resolution_x * prb->grid_resolution_y * prb->grid_resolution_z;
				lbake->grid_count++;
			}
			else if (prb->type == LIGHTPROBE_TYPE_CUBE) {
				lbake->cube_count++;
			}
		}
	}
	DEG_OBJECT_ITER_FOR_RENDER_ENGINE_END;
}

static void eevee_lightbake_gather_probes(EEVEE_LightBake *lbake)
{
	Depsgraph *depsgraph = lbake->depsgraph;
	EEVEE_LightCache *lcache = lbake->scene->eevee.light_cache;

	/* At least one for the world */
	int grid_count = 1;
	int cube_count = 1;
	int total_irr_samples = 1;

	/* Convert all lightprobes to tight UBO data from all lightprobes in the scene.
	 * This allows a large number of probe to be precomputed. */
	DEG_OBJECT_ITER_FOR_RENDER_ENGINE_BEGIN(depsgraph, ob)
	{
		if (ob->type == OB_LIGHTPROBE) {
			LightProbe *prb = (LightProbe *)ob->data;

			if (prb->type == LIGHTPROBE_TYPE_GRID) {
				lbake->grid_prb[grid_count] = prb;
				EEVEE_LightGrid *egrid = &lcache->grid_data[grid_count++];
				EEVEE_lightprobes_grid_data_from_object(ob, egrid, &total_irr_samples);
			}
			else if (prb->type == LIGHTPROBE_TYPE_CUBE) {
				lbake->grid_prb[cube_count] = prb;
				EEVEE_LightProbe *eprobe = &lcache->cube_data[cube_count++];
				EEVEE_lightprobes_cube_data_from_object(ob, eprobe);
			}
		}
	}
	DEG_OBJECT_ITER_FOR_RENDER_ENGINE_END;
}

void EEVEE_lightbake_update(void *custom_data)
{
	EEVEE_LightBake *lbake = (EEVEE_LightBake *)custom_data;
	Scene *scene = lbake->scene;

	DEG_id_tag_update(&scene->id, DEG_TAG_COPY_ON_WRITE);
}

void EEVEE_lightbake_job(void *custom_data, short *UNUSED(stop), short *do_update, float *UNUSED(progress))
{
	EEVEE_LightBake *lbake = (EEVEE_LightBake *)custom_data;
	Depsgraph *depsgraph = lbake->depsgraph;

	int frame = 0; /* TODO make it user param. */
	DEG_evaluate_on_framechange(lbake->bmain, depsgraph, frame);

	lbake->view_layer = DEG_get_evaluated_view_layer(depsgraph);

	/* Count lightprobes */
	eevee_lightbake_count_probes(lbake);

	/* TODO: Remove when multiple drawmanager/context are supported.
	 * Currently this locks the viewport without any reason
	 * (resource creation can be done from another context). */
	eevee_lightbake_context_enable(lbake);
	eevee_lightbake_create_resources(lbake);
	eevee_lightbake_context_disable(lbake);

	/* Gather all probes data */
	eevee_lightbake_gather_probes(lbake);

	EEVEE_LightCache *lcache = lbake->scene->eevee.light_cache;

	/* Render world irradiance and reflection first */
	if (lcache->flag & LIGHTCACHE_UPDATE_WORLD) {
		eevee_lightbake_context_enable(lbake);
		DRW_custom_pipeline(&draw_engine_eevee_type, depsgraph, eevee_lightbake_render_world, lbake);
		*do_update = 1;
		eevee_lightbake_context_disable(lbake);
	}

#if 0
	/* Render irradiance grids */
	lbake->bounce_curr = 0;
	while (lbake->bounce_curr < lbake->bounce_count) {
		/* Bypass world, start at 1. */
		for (int p = 1; p < lbake->grid_count; ++p) {
			/* TODO: make DRW manager instanciable (and only lock on drawing) */
			DRW_opengl_context_enable();

			/* Create passes */
			/* Iter through objects */
			DRW_opengl_context_disable();

			/* Render one cubemap/irradiance sample. */
			if (*stop != 0) {
				return;
			}
		}
		lbake->bounce_curr += 1;
	}

	/* Render reflections */
	for (prb in cube_probes) {
		/* Ask for lower importance draw manager lock. */

		/* Create passes */
		/* Iter through objects */
		/* Render one cubemap/irradiance sample. */
		if (*stop != 0) {
			return;
		}
	}
#endif

	eevee_lightbake_delete_resources(lbake);
}

/** \} */

