/*
 * ***** BEGIN GPL LICENSE BLOCK *****
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
 * The Original Code is Copyright (C) 2005 Blender Foundation.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Clement Foucault.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gpu_uniformbuffer.c
 *  \ingroup gpu
 */

#include <string.h>
#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"

#include "gpu_codegen.h"

#include "GPU_extensions.h"
#include "GPU_glew.h"
#include "GPU_material.h"
#include "GPU_uniformbuffer.h"

typedef enum GPUUniformBufferFlag {
	GPU_UBO_FLAG_INITIALIZED = (1 << 0),
	GPU_UBO_FLAG_DIRTY = (1 << 1),
} GPUUniformBufferFlag;

typedef enum GPUUniformBufferType {
	GPU_UBO_STATIC = 0,
	GPU_UBO_DYNAMIC = 1,
} GPUUniformBufferType;

struct GPUUniformBuffer {
	int size;           /* in bytes */
	GLuint bindcode;    /* opengl identifier for UBO */
	int bindpoint;      /* current binding point */
	GPUUniformBufferType type;
};

#define GPUUniformBufferStatic GPUUniformBuffer

typedef struct GPUUniformBufferDynamic {
	GPUUniformBuffer buffer;
	ListBase items;				 /* GPUUniformBufferDynamicItem */
	void *data;                  /* Continuous memory block to copy to GPU. */
	int *id_lookup; /* Lookup table for the offset of each individual GPUInput. */
	char flag;
} GPUUniformBufferDynamic;

struct GPUUniformBufferDynamicItem {
	struct GPUUniformBufferDynamicItem *next, *prev;
	GPUType gputype;
	int offset;
	int size;
};


/* Prototypes */
static GPUType get_padded_gpu_type(struct LinkData *link);
static void gpu_uniformbuffer_inputs_sort(struct ListBase *inputs);

static GPUUniformBufferDynamicItem *gpu_uniformbuffer_populate(
        GPUUniformBufferDynamic *ubo, const GPUType gputype, const int offset);

/* Only support up to this type, if you want to extend it, make sure the
 * padding logic is correct for the new types. */
#define MAX_UBO_GPU_TYPE GPU_VEC4

static void gpu_uniformbuffer_initialize(GPUUniformBuffer *ubo, const void *data)
{
	glBindBuffer(GL_UNIFORM_BUFFER, ubo->bindcode);
	glBufferData(GL_UNIFORM_BUFFER, ubo->size, data, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

GPUUniformBuffer *GPU_uniformbuffer_create(int size, const void *data, char err_out[256])
{
	GPUUniformBuffer *ubo = MEM_callocN(sizeof(GPUUniformBufferStatic), "GPUUniformBufferStatic");
	ubo->size = size;
	ubo->bindpoint = -1;

	/* Generate Buffer object */
	glGenBuffers(1, &ubo->bindcode);

	if (!ubo->bindcode) {
		if (err_out)
			BLI_snprintf(err_out, 256, "GPUUniformBuffer: UBO create failed");
		GPU_uniformbuffer_free(ubo);
		return NULL;
	}

	if (ubo->size > GPU_max_ubo_size()) {
		if (err_out)
			BLI_snprintf(err_out, 256, "GPUUniformBuffer: UBO too big");
		GPU_uniformbuffer_free(ubo);
		return NULL;
	}

	gpu_uniformbuffer_initialize(ubo, data);
	return ubo;
}

static bool gpu_input_is_dynamic_uniform(GPUInput *input) {
	return ((input->source == GPU_SOURCE_VEC_UNIFORM) &&
	        (input->link == NULL));
}

/**
 * Create dynamic UBO from parameters
 * Return NULL if failed to create or if \param inputs is empty.
 *
 * \param inputs ListBase of GPUInput
 * \param r_inputs ListBase with sorted LinkData->data(GPUInputs) of UBO uniforms
 */
GPUUniformBuffer *GPU_uniformbuffer_dynamic_sort_and_create(
        ListBase *inputs, ListBase *r_inputs_sorted, char err_out[256])
{
	/* There is no point on creating an UBO if there is no valid input. */
	int num_inputs = 0;
	BLI_assert(BLI_listbase_is_empty(r_inputs_sorted));
	for (GPUInput *input = inputs->first; input; input = input->next) {
		if (gpu_input_is_dynamic_uniform(input)) {
			BLI_addtail(r_inputs_sorted, BLI_genericNodeN(input));
			num_inputs++;
		}
	}
	if (num_inputs == 0) {
		return NULL;
	}

	GPUUniformBufferDynamic *ubo = MEM_callocN(sizeof(GPUUniformBufferDynamic), "GPUUniformBufferDynamic");
	ubo->buffer.type = GPU_UBO_DYNAMIC;
	ubo->buffer.bindpoint = -1;
	ubo->flag = GPU_UBO_FLAG_DIRTY;

	/* Generate Buffer object. */
	glGenBuffers(1, &ubo->buffer.bindcode);

	if (!ubo->buffer.bindcode) {
		if (err_out)
			BLI_snprintf(err_out, 256, "GPUUniformBuffer: UBO create failed");
		GPU_uniformbuffer_free(&ubo->buffer);
		return NULL;
	}

	if (ubo->buffer.size > GPU_max_ubo_size()) {
		if (err_out)
			BLI_snprintf(err_out, 256, "GPUUniformBuffer: UBO too big");
		GPU_uniformbuffer_free(&ubo->buffer);
		return NULL;
	}

	ubo->id_lookup = MEM_mallocN(num_inputs * sizeof(*ubo->id_lookup), __func__);

	/* Make sure we comply to the ubo alignment requirements, yet keep a lookup table for their original order. */
	gpu_uniformbuffer_inputs_sort(r_inputs_sorted);

	int *id_lookup = ubo->id_lookup;
	int offset = 0;
	for (LinkData *link = r_inputs_sorted->first; link; link = link->next) {
		if (gpu_input_is_dynamic_uniform(link->data)) {
			GPUType gputype = get_padded_gpu_type(link);
			gpu_uniformbuffer_populate(ubo, gputype, offset);
			offset += gputype;

			const int id = BLI_findindex(inputs, link->data);
			BLI_assert(id != -1);
			*id_lookup++ = id;
		}
	}

	ubo->data = MEM_mallocN(ubo->buffer.size, __func__);

	/* Initialize buffer data. */
	GPU_uniformbuffer_dynamic_eval(&ubo->buffer, inputs);
	GPU_uniformbuffer_dynamic_update(&ubo->buffer);

	return &ubo->buffer;
}

/**
 * Free the data, and clean the items list.
 */
static void gpu_uniformbuffer_dynamic_reset(GPUUniformBufferDynamic *ubo)
{
	ubo->buffer.size = 0;
	if (ubo->data) {
		MEM_freeN(ubo->data);
	}
	if (ubo->id_lookup) {
		MEM_freeN(ubo->id_lookup);
	}
	BLI_freelistN(&ubo->items);
}

void GPU_uniformbuffer_free(GPUUniformBuffer *ubo)
{
	if (ubo->type == GPU_UBO_DYNAMIC) {
		gpu_uniformbuffer_dynamic_reset((GPUUniformBufferDynamic *)ubo);
	}

	glDeleteBuffers(1, &ubo->bindcode);
	MEM_freeN(ubo);
}

static void gpu_uniformbuffer_update(GPUUniformBuffer *ubo, const void *data)
{
	glBindBuffer(GL_UNIFORM_BUFFER, ubo->bindcode);
	glBufferSubData(GL_UNIFORM_BUFFER, 0, ubo->size, data);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void GPU_uniformbuffer_update(GPUUniformBuffer *ubo, const void *data)
{
	BLI_assert(ubo->type == GPU_UBO_STATIC);
	gpu_uniformbuffer_update(ubo, data);
}

/**
 * Update the data based on unsorted GPUInput nodes.
 * They may either be the complete list of GPUMaterial inputs, or
 * already a sub-selection of only the UBO ones.
 */
void GPU_uniformbuffer_dynamic_eval(GPUUniformBuffer *ubo_, ListBase *inputs)
{
	BLI_assert(ubo_->type == GPU_UBO_DYNAMIC);
	GPUUniformBufferDynamic *ubo = (GPUUniformBufferDynamic *)ubo_;

	int *sorted_id = ubo->id_lookup;
	for (GPUInput *input = inputs->first; input; input = input->next) {

		printf("%s: %p > %p\n", __func__, input, input->dynamicvec);
		printf("%s %p\n", __func__, input->link);

		if (gpu_input_is_dynamic_uniform(input)) {
			BLI_assert(input != NULL);
			BLI_assert(input->dynamicvec != NULL);
			const int id = *sorted_id++;
			GPUUniformBufferDynamicItem *item = BLI_findlink(&ubo->items, id);
			printf("%s: %d\n", __func__, (int)(item->size / sizeof(float)));
			for (int i = 0; i < (item->size / sizeof(float)); i++) {
				printf("%s: [%d] %4.2f\n", __func__, i, input->dynamicvec[i]);
			}
			memcpy((float *)ubo->data + item->offset, input->dynamicvec, item->size);
		}
	}
}

/**
 * We need to recalculate the internal data, and re-generate it
 * from its populated items.
 */
void GPU_uniformbuffer_dynamic_update(GPUUniformBuffer *ubo_)
{
	BLI_assert(ubo_->type == GPU_UBO_DYNAMIC);
	GPUUniformBufferDynamic *ubo = (GPUUniformBufferDynamic *)ubo_;

	if (ubo->flag & GPU_UBO_FLAG_INITIALIZED) {
		gpu_uniformbuffer_update(ubo_, ubo->data);
	}
	else {
		ubo->flag |= GPU_UBO_FLAG_INITIALIZED;
		gpu_uniformbuffer_initialize(ubo_, ubo->data);
	}

	ubo->flag &= ~GPU_UBO_FLAG_DIRTY;
}

/**
 * We need to pad some data types (vec3) on the C side
 * To match the GPU expected memory block alignment.
 */
static GPUType get_padded_gpu_type(LinkData *link)
{
	GPUInput *input = link->data;
	GPUType gputype = input->type;

	/* Unless the vec3 is followed by a float we need to treat it as a vec4. */
	if (gputype == GPU_VEC3 &&
	    (link->next != NULL) &&
	    (((GPUInput *)link->next->data)->type != GPU_FLOAT))
	{
		gputype = GPU_VEC4;
	}

	return gputype;
}

/**
 * Returns 1 if the first item shold be after second item.
 * We make sure the vec4 uniforms come first.
 */
static int inputs_cmp(const void *a, const void *b)
{
	const LinkData *link_a = a, *link_b = b;
	const GPUInput *input_a = link_a->data, *input_b = link_b->data;
	return input_a->type < input_b->type ? 1 : 0;
}

/**
 * Make sure we respect the expected alignment of UBOs.
 * vec4, pad vec3 as vec4, then vec2, then floats.
 */
static void gpu_uniformbuffer_inputs_sort(ListBase *inputs)
{
	/* Order them as vec4, vec3, vec2, float. */
	BLI_listbase_sort(inputs, inputs_cmp);

	/* Creates a lookup table for the different types; */
	LinkData *inputs_lookup[MAX_UBO_GPU_TYPE + 1] = {NULL};
	GPUType cur_type = MAX_UBO_GPU_TYPE + 1;

	for (LinkData *link = inputs->first; link; link = link->next) {
		GPUInput *input = link->data;
		if (input->type == cur_type) {
			continue;
		}
		else {
			inputs_lookup[input->type] = link;
			cur_type = input->type;
		}
	}

	/* If there is no GPU_VEC3 there is no need for alignment. */
	if (inputs_lookup[GPU_VEC3] == NULL) {
		return;
	}

	LinkData *link = inputs_lookup[GPU_VEC3];
	while (link != NULL && ((GPUInput *)link->data)->type == GPU_VEC3) {
		LinkData *link_next = link->next;

		/* If GPU_VEC3 is followed by nothing or a GPU_FLOAT, no need for aligment. */
		if ((link_next == NULL) ||
		    ((GPUInput *)link_next->data)->type == GPU_FLOAT)
		{
			break;
		}

		/* If there is a float, move it next to current vec3. */
		if (inputs_lookup[GPU_FLOAT] != NULL) {
			LinkData *float_input = inputs_lookup[GPU_FLOAT];
			inputs_lookup[GPU_FLOAT] = float_input->next;

			BLI_remlink(inputs, float_input);
			BLI_insertlinkafter(inputs, link, float_input);
		}

		link = link_next;
	}
}

/**
 * This may now happen from the main thread, so we can't update the UBO
 * We simply flag it as dirty
 */
static GPUUniformBufferDynamicItem *gpu_uniformbuffer_populate(
        GPUUniformBufferDynamic *ubo, const GPUType gputype, const int offset)
{
	BLI_assert(gputype <= MAX_UBO_GPU_TYPE);
	GPUUniformBufferDynamicItem *item = MEM_callocN(sizeof(GPUUniformBufferDynamicItem), __func__);

	item->gputype = gputype;
	item->size = gputype * sizeof(float);
	item->offset = offset;

	ubo->buffer.size += item->size;
	ubo->flag |= GPU_UBO_FLAG_DIRTY;
	BLI_addtail(&ubo->items, item);

	return item;
}

void GPU_uniformbuffer_bind(GPUUniformBuffer *ubo, int number)
{
	if (number >= GPU_max_ubo_binds()) {
		fprintf(stderr, "Not enough UBO slots.\n");
		return;
	}

	if (ubo->type == GPU_UBO_DYNAMIC) {
		GPUUniformBufferDynamic *ubo_dynamic = (GPUUniformBufferDynamic *)ubo;
		if (ubo_dynamic->flag & GPU_UBO_FLAG_DIRTY) {
			GPU_uniformbuffer_dynamic_update(ubo);
		}
	}

	if (ubo->bindcode != 0) {
		glBindBufferBase(GL_UNIFORM_BUFFER, number, ubo->bindcode);
	}

	ubo->bindpoint = number;
}

void GPU_uniformbuffer_unbind(GPUUniformBuffer *ubo)
{
	ubo->bindpoint = -1;
}

int GPU_uniformbuffer_bindpoint(GPUUniformBuffer *ubo)
{
	return ubo->bindpoint;
}

void GPU_uniformbuffer_tag_dirty(GPUUniformBuffer *ubo_)
{
	BLI_assert(ubo_->type == GPU_UBO_DYNAMIC);
	GPUUniformBufferDynamic *ubo = (GPUUniformBufferDynamic *)ubo_;
	ubo->flag |= GPU_UBO_FLAG_DIRTY;
}

#undef MAX_UBO_GPU_TYPE
