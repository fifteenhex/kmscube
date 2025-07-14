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

#define _GNU_SOURCE

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "esUtil.h"

static struct cube cube;

static struct {
	const struct egl *egl;

	ESMatrix projection;
	const struct gbm *gbm;

	GLuint program, blit_program;
	/* uniform handles: */
	GLint modelviewmatrix, modelviewprojectionmatrix;
	GLint texture, blit_texture;
	GLuint vbo, blit_vbo;
	GLint position_attrib, texcoord_attrib, normal_attrib;
	GLint blit_position_attrib, blit_texcoord_attrib;
	GLuint tex;

	/* video decoder: */
	struct decoder *decoder;
	int filenames_count, idx;
	const char *filenames[32];

	EGLSyncKHR last_fence;
} gl;

static const struct blit_vertex {
        GLfloat position[2];
        GLfloat texCoord[2];
} blit_vertices[] = {
	{{-1.0f, -1.0f}, {0.0f, 1.0f}},
	{{ 1.0f, -1.0f}, {1.0f, 1.0f}},
	{{-1.0f,  1.0f}, {0.0f, 0.0f}},
	{{ 1.0f,  1.0f}, {1.0f, 0.0f}},
};

static const char *blit_vs =
		"attribute vec4 in_position;        \n"
		"attribute vec2 in_TexCoord;        \n"
		"                                   \n"
		"varying vec2 vTexCoord;            \n"
		"                                   \n"
		"void main()                        \n"
		"{                                  \n"
		"    gl_Position = in_position;     \n"
		"    vTexCoord = in_TexCoord;       \n"
		"}                                  \n";

static const char *blit_fs =
		"#extension GL_OES_EGL_image_external : enable\n"
		"precision mediump float;           \n"
		"                                   \n"
		"uniform samplerExternalOES uTex;   \n"
		"                                   \n"
		"varying vec2 vTexCoord;            \n"
		"                                   \n"
		"void main()                        \n"
		"{                                  \n"
		"    gl_FragColor = texture2D(uTex, vTexCoord);\n"
		"}                                  \n";

static const char *vertex_shader_source =
		"uniform mat4 modelviewMatrix;      \n"
		"uniform mat4 modelviewprojectionMatrix;\n"
		"                                   \n"
		"attribute vec4 in_position;        \n"
		"attribute vec2 in_TexCoord;        \n"
		"attribute vec3 in_normal;          \n"
		"                                   \n"
		"vec4 lightSource = vec4(2.0, 2.0, 20.0, 0.0);\n"
		"                                   \n"
		"varying vec4 vVaryingColor;        \n"
		"varying vec2 vTexCoord;            \n"
		"                                   \n"
		"void main()                        \n"
		"{                                  \n"
		"    gl_Position = modelviewprojectionMatrix * in_position;\n"
		"    mat3 normalMatrix = mat3(modelviewMatrix);\n"
		"    vec3 vEyeNormal = normalMatrix * in_normal;\n"
		"    vec4 vPosition4 = modelviewMatrix * in_position;\n"
		"    vec3 vPosition3 = vPosition4.xyz / vPosition4.w;\n"
		"    vec3 vLightDir = normalize(lightSource.xyz - vPosition3);\n"
		"    float diff = max(0.0, dot(vEyeNormal, vLightDir));\n"
		"    vVaryingColor = vec4(diff * vec3(1.0, 1.0, 1.0), 1.0);\n"
		"    vTexCoord = in_TexCoord; \n"
		"}                            \n";

static const char *fragment_shader_source =
		"#extension GL_OES_EGL_image_external : enable\n"
		"precision mediump float;           \n"
		"                                   \n"
		"uniform samplerExternalOES uTex;   \n"
		"                                   \n"
		"varying vec4 vVaryingColor;        \n"
		"varying vec2 vTexCoord;            \n"
		"                                   \n"
		"void main()                        \n"
		"{                                  \n"
		"    gl_FragColor = vVaryingColor * texture2D(uTex, vTexCoord);\n"
		"}                                  \n";


static void draw_cube_video(unsigned i)
{
	ESMatrix modelview;
	EGLImage frame;

	if (gl.last_fence) {
		gl.egl->eglClientWaitSyncKHR(gl.egl->display, gl.last_fence, 0, EGL_FOREVER_KHR);
		gl.egl->eglDestroySyncKHR(gl.egl->display, gl.last_fence);
		gl.last_fence = NULL;
	}

	frame = video_frame(gl.decoder);
	if (!frame) {
		/* end of stream */
		video_deinit(gl.decoder);
		gl.idx = (gl.idx + 1) % gl.filenames_count;
		gl.decoder = video_init(gl.egl, gl.gbm, gl.filenames[gl.idx]);
		if (!gl.decoder) {
			printf("cannot create new video decoder\n");
			return;
		}
		frame = video_frame(gl.decoder);
		if (!frame) {
			printf("cannot get frames from new decoder\n");
			return;
		}
	}

	glUseProgram(gl.blit_program);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, gl.tex);
	gl.egl->glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, frame);

	/* clear the color buffer */
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(gl.blit_program);
	glUniform1i(gl.blit_texture, 0); /* '0' refers to texture unit 0. */
	glBindBuffer(GL_ARRAY_BUFFER, gl.blit_vbo);
	glVertexAttribPointer(gl.blit_position_attrib, 2, GL_FLOAT, GL_FALSE, sizeof(blit_vertices[0]), (const GLvoid *)offsetof(struct blit_vertex, position));
	glVertexAttribPointer(gl.blit_texcoord_attrib, 2, GL_FLOAT, GL_FALSE, sizeof(blit_vertices[0]), (const GLvoid *)offsetof(struct blit_vertex, texCoord));
	glEnableVertexAttribArray(gl.blit_position_attrib);
	glEnableVertexAttribArray(gl.blit_texcoord_attrib);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glEnableVertexAttribArray(gl.blit_texcoord_attrib);
	glEnableVertexAttribArray(gl.blit_position_attrib);

	glUseProgram(gl.program);

	esMatrixLoadIdentity(&modelview);
	esTranslate(&modelview, 0.0f, 0.0f, -8.0f);
	esRotate(&modelview, 45.0f + (0.25f * i), 1.0f, 0.0f, 0.0f);
	esRotate(&modelview, 45.0f - (0.5f * i), 0.0f, 1.0f, 0.0f);
	esRotate(&modelview, 10.0f + (0.15f * i), 0.0f, 0.0f, 1.0f);

	ESMatrix modelviewprojection;
	esMatrixLoadIdentity(&modelviewprojection);
	esMatrixMultiply(&modelviewprojection, &modelview, &gl.projection);

	glUniformMatrix4fv(gl.modelviewmatrix, 1, GL_FALSE, &modelview.m[0][0]);
	glUniformMatrix4fv(gl.modelviewprojectionmatrix, 1, GL_FALSE, &modelviewprojection.m[0][0]);
	glUniform1i(gl.texture, 0); /* '0' refers to texture unit 0. */
	glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);
	glVertexAttribPointer(gl.position_attrib, 3, GL_FLOAT, GL_FALSE, sizeof(vertices[0]), (const GLvoid *)offsetof(struct vertex, position));
	glVertexAttribPointer(gl.texcoord_attrib, 2, GL_FLOAT, GL_FALSE, sizeof(vertices[0]), (const GLvoid *)offsetof(struct vertex, texCoord));
	glVertexAttribPointer(gl.normal_attrib, 3, GL_FLOAT, GL_FALSE, sizeof(vertices[0]), (const GLvoid *)offsetof(struct vertex, normal));
	glEnableVertexAttribArray(gl.position_attrib);
	glEnableVertexAttribArray(gl.texcoord_attrib);
	glEnableVertexAttribArray(gl.normal_attrib);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 8, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 12, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 16, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 20, 4);

	glEnableVertexAttribArray(gl.normal_attrib);
	glEnableVertexAttribArray(gl.texcoord_attrib);
	glEnableVertexAttribArray(gl.position_attrib);

	gl.last_fence = gl.egl->eglCreateSyncKHR(gl.egl->display, EGL_SYNC_FENCE_KHR, NULL);
}

const struct cube * init_cube_video(const struct egl *egl, const struct gbm *gbm, const char *filenames)
{
	char *fnames, *s;
	int ret, i = 0;

	gl.egl = egl;

	if (egl_check(gl.egl, glEGLImageTargetTexture2DOES) ||
	    egl_check(gl.egl, eglCreateSyncKHR) ||
	    egl_check(gl.egl, eglDestroySyncKHR) ||
	    egl_check(gl.egl, eglClientWaitSyncKHR))
		return NULL;

	fnames = strdup(filenames);
	while ((s = strstr(fnames, ","))) {
		gl.filenames[i] = fnames;
		s[0] = '\0';
		fnames = &s[1];
		i++;
	}
	gl.filenames[i] = fnames;
	gl.filenames_count = ++i;

	gl.decoder = video_init(gl.egl, gbm, gl.filenames[gl.idx]);
	if (!gl.decoder) {
		printf("cannot create video decoder\n");
		return NULL;
	}

	GLfloat aspect = (GLfloat)(gbm->height) / (GLfloat)(gbm->width);
	esMatrixLoadIdentity(&gl.projection);
	esFrustum(&gl.projection, -2.1f, +2.1f, -2.1f * aspect, +2.1f * aspect, 6.0f, 10.0f);
	gl.gbm = gbm;

	ret = create_program(blit_vs, blit_fs, &gl.blit_program);
	if (ret < 0)
		return NULL;

	gl.blit_texture = glGetUniformLocation(gl.blit_program, "uTex");
	gl.blit_position_attrib = glGetAttribLocation(gl.blit_program, "in_position");
	gl.blit_texcoord_attrib = glGetAttribLocation(gl.blit_program, "in_TexCoord");

	glGenBuffers(1, &gl.blit_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, gl.blit_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(blit_vertices), &blit_vertices[0], GL_STATIC_DRAW);

	ret = create_program(vertex_shader_source, fragment_shader_source, &gl.program);
	if (ret < 0)
		return NULL;

	gl.modelviewmatrix = glGetUniformLocation(gl.program, "modelviewMatrix");
	gl.modelviewprojectionmatrix = glGetUniformLocation(gl.program, "modelviewprojectionMatrix");
	gl.texture   = glGetUniformLocation(gl.program, "uTex");
	gl.position_attrib = glGetAttribLocation(gl.program, "in_position");
	gl.texcoord_attrib = glGetAttribLocation(gl.program, "in_TexCoord");
	gl.normal_attrib = glGetAttribLocation(gl.program, "in_normal");

	glViewport(0, 0, gbm->width, gbm->height);
	glEnable(GL_CULL_FACE);

	glGenBuffers(1, &gl.vbo);
	glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), &vertices[0], GL_STATIC_DRAW);

	glGenTextures(1, &gl.tex);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, gl.tex);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glClearColor(0.5, 0.5, 0.5, 1.0);

	cube.draw = draw_cube_video;

	return &cube;
}
