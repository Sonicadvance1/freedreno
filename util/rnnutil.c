/* -*- mode: C; c-file-style: "k&r"; tab-width 4; indent-tabs-mode: t; -*- */

/*
 * Copyright (C) 2014 Rob Clark <robclark@freedesktop.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "rnnutil.h"

static struct rnndomain *finddom(struct rnn *rnn, uint32_t regbase)
{
	if (rnndec_checkaddr(rnn->vc, rnn->dom[0], regbase, 0))
		return rnn->dom[0];
	return rnn->dom[1];
}

struct rnn *rnn_new(int nocolor)
{
	struct rnn *rnn = calloc(sizeof(*rnn), 1);

	if (!rnn)
		return NULL;

	rnn_init();

	rnn->db = rnn_newdb();
	rnn->vc_nocolor = rnndec_newcontext(rnn->db);
	rnn->vc_nocolor->colors = &envy_null_colors;
	if (nocolor) {
		rnn->vc = rnn->vc_nocolor;
	} else {
		rnn->vc = rnndec_newcontext(rnn->db);
		rnn->vc->colors = &envy_def_colors;
	}

	return rnn;
}

static void init(struct rnn *rnn, char *file, char *domain)
{
	/* prepare rnn stuff for lookup */
	rnn_parsefile(rnn->db, file);
	rnn_prepdb(rnn->db);
	rnn->dom[0] = rnn_finddomain(rnn->db, domain);
	if (!strcmp(domain, "A4XX")) {
		/* I think even the common registers move around in A4XX.. */
		rnn->dom[1] = rnn->dom[0];
	} else {
		rnn->dom[1] = rnn_finddomain(rnn->db, "AXXX");
	}
	if (!rnn->dom[0] && rnn->dom[1]) {
		fprintf(stderr, "Could not find domain %s in %s\n", domain, file);
	}
}

void rnn_load(struct rnn *rnn, const char *gpuname)
{
	if (strstr(gpuname, "a2")) {
		init(rnn, "adreno/a2xx.xml", "A2XX");
	} else if (strstr(gpuname, "a3")) {
		init(rnn, "adreno/a3xx.xml", "A3XX");
	} else if (strstr(gpuname, "a4")) {
		init(rnn, "adreno/a4xx.xml", "A4XX");
	}
}

const char *rnn_regname(struct rnn *rnn, uint32_t regbase, int color)
{
	static char buf[128];
	struct rnndecaddrinfo *info;

	info = rnndec_decodeaddr(color ? rnn->vc : rnn->vc_nocolor,
			finddom(rnn, regbase), regbase, 0);
	if (info) {
		strcpy(buf, info->name);
		free(info->name);
		free(info);
		return buf;
	}
	return NULL;
}

struct rnndecaddrinfo *rnn_reginfo(struct rnn *rnn, uint32_t regbase)
{
	return rnndec_decodeaddr(rnn->vc, finddom(rnn, regbase), regbase, 0);
}

const char *rnn_enumname(struct rnn *rnn, const char *name, uint32_t val)
{
	return rnndec_decode_enum(rnn->vc, name, val);
}
