/*
 * Copyright (c) 2017 Rob Clark <rclark@redhat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "drm-common.h"

static struct drm drm;

static int offscreen_run(const struct gbm *gbm, const struct egl *egl, const struct cube *cube)
{
	struct gbm_bo *bo = NULL;
	uint32_t i = 0;

	start_fpscntrs();

	while (i < drm.count) {
		unsigned frame = i;
		struct gbm_bo *next_bo;

		if (!gbm->surface) {
			glBindFramebuffer(GL_FRAMEBUFFER, egl->fbs[frame % NUM_BUFFERS].fb);
		}

		cube->draw(i++);

		if (gbm->surface) {
			eglSwapBuffers(egl->display, egl->surface);
			next_bo = gbm_surface_lock_front_buffer(gbm->surface);
		} else {
			glFinish();
			next_bo = gbm->bos[frame % NUM_BUFFERS];
		}
		if (!next_bo) {
			printf("Failed to lock frontbuffer\n");
			return -1;
		}

		end_fpscntrs();

		/* release last buffer to render on again: */
		if (bo && gbm->surface)
			gbm_surface_release_buffer(gbm->surface, bo);
		bo = next_bo;
	}

	finish_fpscntrs();

	return 0;
}

const struct drm * init_drm_offscreen(const char *device, const char *mode_str, unsigned int count)
{
	int ret;

	ret = init_drm_render(&drm, device, mode_str, count);
	if (ret)
		return NULL;

	drm.run = offscreen_run;

	return &drm;
}
