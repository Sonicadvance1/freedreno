/*
 * Copyright (c) 2012 Rob Clark <robdclark@gmail.com>
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include "redump.h"
#include "disasm.h"

struct pgm_header {
	uint32_t size;
	uint32_t unknown1;
	uint32_t unknown2;
	uint32_t revision;
	uint32_t unknown4;
	uint32_t unknown5;
	uint32_t unknown6;
	uint32_t unknown7;
	uint32_t unknown8;
	uint32_t num_attribs;
	uint32_t num_uniforms;
	uint32_t num_samplers;
	uint32_t num_varyings;
};

struct vs_header {
	uint32_t unknown1;  /* seems to be # of sections up to and including shader */
	uint32_t unknown2;  /* seems to be low byte or so of SQ_PROGRAM_CNTL */
	uint32_t unknown3;
	uint32_t unknown4;
	uint32_t unknown5;
	uint32_t unknown6;
	uint32_t unknown7;
	uint32_t unknown8;
	uint32_t unknown9;  /* seems to be # of sections following shader */
};

struct fs_header {
	uint32_t unknown1;
};

struct attribute {
	uint32_t type_info;
	uint32_t reg;       /* seems to be the register the fetch instruction loads to */
	uint32_t const_idx; /* the CONST() indx value for sampler */
	uint32_t unknown2;
	uint32_t unknown3;
	uint32_t unknown4;
	uint32_t unknown5;
	char name[];
};

struct uniform {
	uint32_t type_info;
	uint32_t unknown2;
	uint32_t unknown3;
	uint32_t unknown4;
	uint32_t const_base; /* const base register (for uniforms that take more than one const reg, ie. matrices) */
	uint32_t unknown6;
	uint32_t const_reg; /* the const register holding the value */
	uint32_t unknown7;
	uint32_t unknown8;
	uint32_t unknown9;
	union {
		struct {
			char name[1];
		} v1;
		struct {
			uint32_t unknown10;
			uint32_t unknown11;
			uint32_t unknown12;
			char name[];
		} v2;
	};
};

struct sampler {
	uint32_t type_info;
	uint32_t unknown2;
	uint32_t unknown3;
	uint32_t unknown4;
	uint32_t unknown5;
	uint32_t unknown6;
	uint32_t const_idx; /* the CONST() indx value for the sampler */
	uint32_t unknown7;
	char name[];
};

struct varying {
	uint32_t type_info;
	uint32_t unknown2;
	uint32_t unknown3;
	uint32_t reg;       /* the register holding the value (on entry to the shader) */
	char name[];
};

struct output {
	uint32_t type_info;
	uint32_t unknown2;
	uint32_t unknown3;
	uint32_t unknown4;
	uint32_t unknown5;
	uint32_t unknown6;
	uint32_t unknown7;
	uint32_t unknown8;
	char name[];
};

struct constant {
	uint32_t unknown1;
	uint32_t unknown2;
	uint32_t unknown3;
	uint32_t const_idx;
	float val[];
};

struct state {
	char *buf;
	int sz;
	struct pgm_header *hdr;
	struct attribute *attribs[32];  /* don't really know the upper limit.. */
	struct uniform *uniforms[32];
	struct sampler *samplers[32];
	struct varying *varyings[32];
	struct output  *outputs[0];  /* I guess only one?? */
};

const char *infile;
static int full_dump = 1;
static int dump_shaders = 0;
static int gpu_id;

char *find_sect_end(char *buf, int sz)
{
	uint8_t *ptr = (uint8_t *)buf;
	uint8_t *end = ptr + sz - 3;

	while (ptr < end) {
		uint32_t d = 0;

		d |= ptr[0] <<  0;
		d |= ptr[1] <<  8;
		d |= ptr[2] << 16;
		d |= ptr[3] << 24;

		/* someone at QC likes baseball */
		if (d == 0xba5eba11)
			return ptr;

		ptr++;
	}
	return NULL;
}

/* convert float to dword */
static inline float d2f(uint32_t d)
{
	union {
		float f;
		uint32_t d;
	} u = {
		.d = d,
	};
	return u.f;
}

static void dump_hex(char *buf, int sz)
{
	uint8_t *ptr = (uint8_t *)buf;
	uint8_t *end = ptr + sz;
	int i = 0;

	while (ptr < end) {
		uint32_t d = 0;

		printf((i % 8) ? " " : "\t");

		d |= *(ptr++) <<  0;
		d |= *(ptr++) <<  8;
		d |= *(ptr++) << 16;
		d |= *(ptr++) << 24;

		printf("%08x", d);

		if ((i % 8) == 7) {
			printf("\n");
		}

		i++;
	}

	if (i % 8) {
		printf("\n");
	}
}

static void dump_float(char *buf, int sz)
{
	uint8_t *ptr = (uint8_t *)buf;
	uint8_t *end = ptr + sz - 3;
	int i = 0;

	while (ptr < end) {
		uint32_t d = 0;

		printf((i % 8) ? " " : "\t");

		d |= *(ptr++) <<  0;
		d |= *(ptr++) <<  8;
		d |= *(ptr++) << 16;
		d |= *(ptr++) << 24;

		printf("%8f", d2f(d));

		if ((i % 8) == 7) {
			printf("\n");
		}

		i++;
	}

	if (i % 8) {
		printf("\n");
	}
}

#define is_ok_ascii(c) \
	(isascii(c) && ((c == '\t') || !iscntrl(c)))

static void clean_ascii(char *buf, int sz)
{
	uint8_t *ptr = (uint8_t *)buf;
	uint8_t *end = ptr + sz;
	while (ptr < end) {
		*(ptr++) ^= 0xff;
	}
}

static void dump_ascii(char *buf, int sz)
{
	uint8_t *ptr = (uint8_t *)buf;
	uint8_t *end = ptr + sz;
	printf("\t");
	while (ptr < end) {
		uint8_t c = *(ptr++) ^ 0xff;
		if (c == '\n') {
			printf("\n\t");
		} else if (c == '\0') {
			printf("\n\t-----------------------------------\n\t");
		} else if (is_ok_ascii(c)) {
			printf("%c", c);
		} else {
			printf("?");
		}
	}
	printf("\n");
}

static void dump_hex_ascii(char *buf, int sz)
{
	uint8_t *ptr = (uint8_t *)buf;
	uint8_t *end = ptr + sz;
	uint8_t *ascii = ptr;
	int i = 0;

	printf("-----------------------------------------------\n");
	printf("%d (0x%x) bytes\n", sz, sz);

	while (ptr < end) {
		uint32_t d = 0;

		printf((i % 4) ? " " : "\t");

		d |= *(ptr++) <<  0;
		d |= *(ptr++) <<  8;
		d |= *(ptr++) << 16;
		d |= *(ptr++) << 24;

		printf("%08x", d);

		if ((i % 4) == 3) {
			int j;
			printf("\t|");
			for (j = 0; j < 16; j++) {
				uint8_t c = *(ascii++);
				c ^= 0xff;
				printf("%c", (isascii(c) && !iscntrl(c)) ? c : '.');
			}
			printf("|\n");
		}

		i++;
	}

	if (i % 8) {
		int j;
		printf("\t|");
		while (ascii < end) {
			uint8_t c = *(ascii++);
			c ^= 0xff;
			printf("%c", (isascii(c) && !iscntrl(c)) ? c : '.');
		}
		printf("|\n");
	}
}

void *next_sect(struct state *state, int *sect_size)
{
	char *end = find_sect_end(state->buf, state->sz);
	void *sect;

	*sect_size = end - state->buf;

	/* copy the section to keep things nicely 32b aligned: */
	sect = malloc(ALIGN(*sect_size, 4));
	memcpy(sect, state->buf, *sect_size);

	state->sz -= *sect_size + 4;
	state->buf = end + 4;

	return sect;
}

static int valid_type(uint32_t type_info)
{
	switch ((type_info >> 8) & 0xff) {
	case 0x8b:     /* vector */
	case 0x14:     /* float */
		return 1;
	default:
		return 0;
	}
}

static void dump_attribute(struct attribute *attrib)
{
	printf("\tR%d, CONST(%d): %s\n", attrib->reg,
			attrib->const_idx, attrib->name);
}

static inline int is_uniform_v2(struct uniform *uniform)
{
	/* TODO maybe this should be based on revision #? */
	if (uniform->v2.unknown10 == 0)
		return 1;
	return 0;
}

static void dump_uniform(struct uniform *uniform)
{
	char *name = is_uniform_v2(uniform) ? uniform->v2.name : uniform->v1.name;
	if (uniform->const_reg == -1) {
		printf("\tC%d+: %s\n", uniform->const_base, name);
	} else {
		printf("\tC%d: %s\n", uniform->const_reg, name);
	}
}

static void dump_sampler(struct sampler *sampler)
{
	printf("\tCONST(%d): %s\n", sampler->const_idx, sampler->name);
}

static void dump_varying(struct varying *varying)
{
	printf("\tR%d: %s\n", varying->reg, varying->name);
}

static void dump_output(struct output *output)
{
	printf("\tR?: %s\n", output->name);
}

static void dump_constant(struct constant *constant)
{
	printf("\tC%d: %f, %f, %f, %f\n", constant->const_idx,
			constant->val[0], constant->val[1],
			constant->val[2], constant->val[3]);
}

/* dump attr/uniform/sampler/varying/const summary: */
static void dump_short_summary(struct state *state, int nconsts,
		struct constant **constants)
{
	int i;

	/* dump attr/uniform/sampler/varying/const summary: */
	for (i = 0; i < state->hdr->num_varyings; i++) {
		dump_varying(state->varyings[i]);
	}
	for (i = 0; i < state->hdr->num_attribs; i++) {
		dump_attribute(state->attribs[i]);
	}
	for (i = 0; i < state->hdr->num_uniforms; i++) {
		dump_uniform(state->uniforms[i]);
	}
	for (i = 0; i < state->hdr->num_samplers; i++) {
		dump_sampler(state->samplers[i]);
	}
	for (i = 0; i < nconsts - 1; i++) {
		if (constants[i]->unknown2 == 0) {
			dump_constant(constants[i]);
		}
	}
	printf("\n");
}

static void dump_raw_shader(uint32_t *dwords, uint32_t sizedwords, int n, char *ext)
{
	static char filename[256];
	int fd;

	if (!dump_shaders)
		return;

	sprintf(filename, "%.*s-%d.%s", strlen(infile)-3, infile, n, ext);
	fd = open(filename, O_WRONLY | O_TRUNC | O_CREAT, 0644);
	write(fd, dwords, sizedwords * 4);
}

static void dump_shaders_a2xx(struct state *state)
{
	int i, sect_size;
	uint8_t *ptr;

	/* dump vertex shaders: */
	for (i = 0; i < 3; i++) {
		struct vs_header *vs_hdr = next_sect(state, &sect_size);
		struct constant *constants[32];
		int j, level = 0;

		printf("\n");

		if (full_dump) {
			printf("#######################################################\n");
			printf("######## VS%d HEADER: (size %d)\n", i, sect_size);
			dump_hex((void *)vs_hdr, sect_size);
		}

		for (j = 0; j < (int)vs_hdr->unknown1 - 1; j++) {
			constants[j] = next_sect(state, &sect_size);
			if (full_dump) {
				printf("######## VS%d CONST: (size=%d)\n", i, sect_size);
				dump_constant(constants[j]);
				dump_hex((char *)constants[j], sect_size);
			}
		}

		ptr = next_sect(state, &sect_size);
		printf("######## VS%d SHADER: (size=%d)\n", i, sect_size);
		if (full_dump) {
			dump_hex(ptr, sect_size);
			level = 1;
		} else {
			dump_short_summary(state, vs_hdr->unknown1 - 1, constants);
		}
		disasm_a2xx((uint32_t *)(ptr + 32), (sect_size - 32) / 4, level+1, SHADER_VERTEX);
		dump_raw_shader((uint32_t *)(ptr + 32), (sect_size - 32) / 4, i, "vo");
		free(ptr);

		for (j = 0; j < vs_hdr->unknown9; j++) {
			ptr = next_sect(state, &sect_size);
			if (full_dump) {
				printf("######## VS%d CONST?: (size=%d)\n", i, sect_size);
				dump_hex(ptr, sect_size);
			}
			free(ptr);
		}

		for (j = 0; j < vs_hdr->unknown1 - 1; j++) {
			free(constants[j]);
		}

		free(vs_hdr);
	}

	/* dump fragment shaders: */
	for (i = 0; i < 1; i++) {
		struct fs_header *fs_hdr = next_sect(state, &sect_size);
		struct constant *constants[32];
		int j, level = 0;

		printf("\n");

		if (full_dump) {
			printf("#######################################################\n");
			printf("######## FS%d HEADER: (size %d)\n", i, sect_size);
			dump_hex((void *)fs_hdr, sect_size);
		}

		for (j = 0; j < fs_hdr->unknown1 - 1; j++) {
			constants[j] = next_sect(state, &sect_size);
			if (full_dump) {
				printf("######## FS%d CONST: (size=%d)\n", i, sect_size);
				dump_constant(constants[j]);
				dump_hex((char *)constants[j], sect_size);
			}
		}

		ptr = next_sect(state, &sect_size);
		printf("######## FS%d SHADER: (size=%d)\n", i, sect_size);
		if (full_dump) {
			dump_hex(ptr, sect_size);
			level = 1;
		} else {
			dump_short_summary(state, fs_hdr->unknown1 - 1, constants);
		}
		disasm_a2xx((uint32_t *)(ptr + 32), (sect_size - 32) / 4, level+1, SHADER_FRAGMENT);
		dump_raw_shader((uint32_t *)(ptr + 32), (sect_size - 32) / 4, i, "fo");
		free(ptr);

		for (j = 0; j < fs_hdr->unknown1 - 1; j++) {
			free(constants[j]);
		}

		free(fs_hdr);
	}
}

static void dump_shaders_a3xx(struct state *state)
{
	int i, j;

	/* dump vertex shaders: */
	for (i = 0; i < 2; i++) {
		int instrs_size, hdr_size, sect_size, nconsts = 0, level = 0, compact = 0;
		uint8_t *vs_hdr;
		struct constant *constants[32];
		uint8_t *instrs = NULL;

		vs_hdr = next_sect(state, &hdr_size);
printf("hdr_size=%d\n", hdr_size);

		/* seems like there are two cases, either:
		 *  1) 152 byte header,
		 *  2) zero or more 32 byte compiler const sections
		 *  3) followed by shader instructions
		 * or, if there are no compiler consts, this can be
		 * all smashed in one large section
		 */
		int n;
		if (state->hdr->revision >= 7)
			n = 156;
		else
			n = 152;
		if (hdr_size > n) {
			instrs = &vs_hdr[n];
			instrs_size = hdr_size - n;
			hdr_size = n;
			compact = 1;
		} else {
			while (1) {
				void *ptr = next_sect(state, &sect_size);

dump_hex_ascii(ptr, sect_size);
				if ((sect_size != 32) && (sect_size != 44)) {
					/* end of constants: */
					instrs = ptr;
					instrs_size = sect_size;
					break;
				}
				constants[nconsts++] = ptr;
			}
		}

		printf("\n");

		if (full_dump) {
			printf("#######################################################\n");
			printf("######## VS%d HEADER: (size %d)\n", i, hdr_size);
			dump_hex((void *)vs_hdr, hdr_size);
			for (j = 0; j < nconsts; j++) {
				printf("######## VS%d CONST: (size=%d)\n", i, sizeof(constants[i]));
				dump_constant(constants[j]);
				dump_hex((char *)constants[j], sizeof(constants[j]));
			}
		}

		printf("######## VS%d SHADER: (size=%d)\n", i, instrs_size);
		if (full_dump) {
			dump_hex(instrs, instrs_size);
			level = 1;
		} else {
			dump_short_summary(state, nconsts, constants);
		}

		if (!compact) {
			if (state->hdr->revision >= 7) {
				instrs += ALIGN(instrs_size, 8) - instrs_size;
				instrs_size = ALIGN(instrs_size, 8);
			}
			instrs += 32;
			instrs_size -= 32;
		}

		disasm_a3xx((uint32_t *)instrs, instrs_size / 4, level+1, SHADER_VERTEX);
		dump_raw_shader((uint32_t *)instrs, instrs_size / 4, i, "vo3");
		free(vs_hdr);
	}

	/* dump fragment shaders: */
	for (i = 0; i < 1; i++) {
		int instrs_size, hdr_size, sect_size, nconsts = 0, level = 0, compact = 0;
		uint8_t *fs_hdr;
		struct constant *constants[32];
		uint8_t *instrs = NULL;

		fs_hdr = next_sect(state, &hdr_size);

printf("hdr_size=%d\n", hdr_size);
		/* two cases, similar to vertex shader, but magic # is 200
		 * (or 208 for newer?)..
		 */
		int n;
		if (state->hdr->revision >= 8)
			n = 208;
		else if (state->hdr->revision == 7)
			n = 204;
		else
			n = 200;

		if (hdr_size > n) {
			instrs = &fs_hdr[n];
			instrs_size = hdr_size - n;
			hdr_size = n;
			compact = 1;
		} else {
			while (1) {
				void *ptr = next_sect(state, &sect_size);

dump_hex_ascii(ptr, sect_size);
				if (sect_size != 32) {
					/* end of constants: */
					instrs = ptr;
					instrs_size = sect_size;
					break;
				}
				constants[nconsts++] = ptr;
			}
		}

		printf("\n");

		if (full_dump) {
			printf("#######################################################\n");
			printf("######## FS%d HEADER: (size %d)\n", i, hdr_size);
			dump_hex((void *)fs_hdr, hdr_size);
			for (j = 0; j < nconsts; j++) {
				printf("######## FS%d CONST: (size=%d)\n", i, sizeof(constants[i]));
				dump_constant(constants[j]);
				dump_hex((char *)constants[j], sizeof(constants[j]));
			}
		}

		printf("######## FS%d SHADER: (size=%d)\n", i, instrs_size);
		if (full_dump) {
			dump_hex(instrs, instrs_size);
			level = 1;
		} else {
			dump_short_summary(state, nconsts, constants);
		}

		if (!compact) {
			if (state->hdr->revision >= 7) {
				instrs += 44;
				instrs_size -= 44;
			} else {
				instrs += 32;
				instrs_size -= 32;
			}
		}
		disasm_a3xx((uint32_t *)instrs, instrs_size / 4, level+1, SHADER_FRAGMENT);
		dump_raw_shader((uint32_t *)instrs, instrs_size / 4, i, "fo3");
		free(fs_hdr);
	}
}

void dump_program(struct state *state)
{
	int i, sect_size;
	uint8_t *ptr;

	state->hdr = next_sect(state, &sect_size);

	printf("######## HEADER: (size %d)\n", sect_size);
	printf("\tsize:       %d\n", state->hdr->size);
	printf("\trevision:   %d\n", state->hdr->revision);
	printf("\tattributes: %d\n", state->hdr->num_attribs);
	printf("\tuniforms:   %d\n", state->hdr->num_uniforms);
	printf("\tsamplers:   %d\n", state->hdr->num_samplers);
	printf("\tvaryings:   %d\n", state->hdr->num_varyings);
	if (full_dump)
		dump_hex((void *)state->hdr, sect_size);
	printf("\n");

	/* there seems to be two 0xba5eba11's at the end of the header, possibly
	 * with some other stuff between them:
	 */
	ptr = next_sect(state, &sect_size);
	if (full_dump) {
		dump_hex_ascii(ptr, sect_size);
	}

	for (i = 0; (i < state->hdr->num_attribs) && (state->sz > 0); i++) {
		state->attribs[i] = next_sect(state, &sect_size);

		/* hmm, for a3xx (or maybe just newer driver version), we have some
		 * extra sections that don't seem useful, so skip these:
		 */
		while (!valid_type(state->attribs[i]->type_info)) {
			dump_hex_ascii(state->attribs[i], sect_size);
			state->attribs[i] = next_sect(state, &sect_size);
		}

		clean_ascii(state->attribs[i]->name, sect_size - 28);
		if (full_dump) {
			printf("######## ATTRIBUTE: (size %d)\n", sect_size);
			dump_attribute(state->attribs[i]);
			dump_hex((char *)state->attribs[i], sect_size);
		}
	}

	for (i = 0; (i < state->hdr->num_uniforms) && (state->sz > 0); i++) {
		state->uniforms[i] = next_sect(state, &sect_size);

		/* hmm, for a3xx (or maybe just newer driver version), we have some
		 * extra sections that don't seem useful, so skip these:
		 */
		while (!valid_type(state->uniforms[i]->type_info)) {
			dump_hex_ascii(state->uniforms[i], sect_size);
			state->uniforms[i] = next_sect(state, &sect_size);
		}

		if (is_uniform_v2(state->uniforms[i])) {
			clean_ascii(state->uniforms[i]->v2.name, sect_size - 53);
		} else {
			clean_ascii(state->uniforms[i]->v1.name, sect_size - 41);
		}

		if (full_dump) {
			printf("######## UNIFORM: (size %d)\n", sect_size);
			dump_uniform(state->uniforms[i]);
			dump_hex((char *)state->uniforms[i], sect_size);
		}
	}

	for (i = 0; (i < state->hdr->num_samplers) && (state->sz > 0); i++) {
		state->samplers[i] = next_sect(state, &sect_size);

		/* hmm, for a3xx (or maybe just newer driver version), we have some
		 * extra sections that don't seem useful, so skip these:
		 */
		while (!valid_type(state->samplers[i]->type_info)) {
			dump_hex_ascii(state->samplers[i], sect_size);
			state->samplers[i] = next_sect(state, &sect_size);
		}

		clean_ascii(state->samplers[i]->name, sect_size - 33);
		if (full_dump) {
			printf("######## SAMPLER: (size %d)\n", sect_size);
			dump_sampler(state->samplers[i]);
			dump_hex((char *)state->samplers[i], sect_size);
		}

		// need to test with multiple samplers to see if we get one
		// of these extra sections for each sampler:
		if (state->hdr->revision >= 7) {
			ptr = next_sect(state, &sect_size);
			dump_hex_ascii(ptr, sect_size);
		}
	}

	for (i = 0; (i < state->hdr->num_varyings) && (state->sz > 0); i++) {
		state->varyings[i] = next_sect(state, &sect_size);

		/* hmm, for a3xx (or maybe just newer driver version), we have some
		 * extra sections that don't seem useful, so skip these:
		 */
		while (!valid_type(state->varyings[i]->type_info)) {
			dump_hex_ascii(state->varyings[i], sect_size);
			state->varyings[i] = next_sect(state, &sect_size);
		}

		clean_ascii(state->varyings[i]->name, sect_size - 16);
		if (full_dump) {
			printf("######## VARYING: (size %d)\n", sect_size);
			dump_varying(state->varyings[i]);
			dump_hex((char *)state->varyings[i], sect_size);
		}
	}

	/* not sure exactly which revision started this, but seems at least
	 * rev7 and rev8 implicitly include a new section for gl_FragColor:
	 */
	if (state->hdr->revision >= 7) {
		/* I guess only one? */
		state->outputs[0] = next_sect(state, &sect_size);

		clean_ascii(state->outputs[0]->name, sect_size - 32);
		if (full_dump) {
			printf("######## OUTPUT: (size %d)\n", sect_size);
			dump_output(state->outputs[0]);
			dump_hex((char *)state->outputs[0], sect_size);
		}
	}

	if (gpu_id >= 300) {
		dump_shaders_a3xx(state);
	} else {
		dump_shaders_a2xx(state);
	}

	if (!full_dump)
		return;

	/* dump ascii version of shader program: */
	ptr = next_sect(state, &sect_size);
	printf("\n#######################################################\n");
	printf("######## SHADER SRC: (size=%d)\n", sect_size);
	dump_ascii(ptr, sect_size);
	free(ptr);

	/* dump remaining sections (there shouldn't be any): */
	while (state->sz > 0) {
		ptr = next_sect(state, &sect_size);
		printf("######## section (size=%d)\n", sect_size);
		printf("as hex:\n");
		dump_hex(ptr, sect_size);
		printf("as float:\n");
		dump_float(ptr, sect_size);
		printf("as ascii:\n");
		dump_ascii(ptr, sect_size);
		free(ptr);
	}
}

static int check_extension(const char *path, const char *ext)
{
	return strcmp(path + strlen(path) - strlen(ext), ext) == 0;
}

int main(int argc, char **argv)
{
	enum rd_sect_type type = RD_NONE;
	enum debug_t debug = 0;
	void *buf = NULL;
	int fd, sz, i;

	/* lame argument parsing: */

	while (1) {
		if ((argc > 1) && !strcmp(argv[1], "--verbose")) {
			debug |= PRINT_RAW | PRINT_VERBOSE;
			argv++;
			argc--;
			continue;
		}
		if ((argc > 1) && !strcmp(argv[1], "--expand")) {
			debug |= EXPAND_REPEAT;
			argv++;
			argc--;
			continue;
		}
		if ((argc > 1) && !strcmp(argv[1], "--short")) {
			/* only short dump, original shader, symbol table, and disassembly */
			full_dump = 0;
			argv++;
			argc--;
			continue;
		}
		if ((argc > 1) && !strcmp(argv[1], "--dump-shaders")) {
			dump_shaders = 1;
			argv++;
			argc--;
			continue;
		}

		break;
	}

	if (argc != 2) {
		fprintf(stderr, "usage: pgmdump [--verbose] [--short] [--dump-shaders] testlog.rd\n");
		return -1;
	}

	disasm_set_debug(debug);

	infile = argv[1];

	fd = open(infile, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "could not open: %s\n", infile);
		return -1;
	}

	/* figure out what sort of input we are dealing with: */
	if (!check_extension(infile, ".rd")) {
		int (*disasm)(uint32_t *dwords, int sizedwords, int level, enum shader_t type);
		enum shader_t shader = 0;
		int ret;

		if (check_extension(infile, ".vo")) {
			disasm = disasm_a2xx;
			shader = SHADER_VERTEX;
		} else if (check_extension(infile, ".fo")) {
			disasm = disasm_a2xx;
			shader = SHADER_FRAGMENT;
		} else if (check_extension(infile, ".vo3")) {
			disasm = disasm_a3xx;
			shader = SHADER_VERTEX;
		} else if (check_extension(infile, ".fo3")) {
			disasm = disasm_a3xx;
			shader = SHADER_FRAGMENT;
		} else if (check_extension(infile, ".co3")) {
			disasm = disasm_a3xx;
			shader = SHADER_COMPUTE;
		} else {
			fprintf(stderr, "invalid input file: %s\n", infile);
			return -1;
		}
		buf = calloc(1, 100 * 1024);
		ret = read(fd, buf, 100 * 1024);
		if (ret < 0) {
			fprintf(stderr, "error: %m");
			return -1;
		}
		return disasm(buf, ret/4, 0, shader);
	}

	while ((read(fd, &type, sizeof(type)) > 0) && (read(fd, &sz, 4) > 0)) {
		free(buf);

		/* note: allow hex dumps to go a bit past the end of the buffer..
		 * might see some garbage, but better than missing the last few bytes..
		 */
		buf = calloc(1, sz + 3);
		read(fd, buf, sz);

		switch(type) {
		case RD_TEST:
			if (full_dump)
				printf("test: %s\n", (char *)buf);
			break;
		case RD_VERT_SHADER:
			printf("vertex shader:\n%s\n", (char *)buf);
			break;
		case RD_FRAG_SHADER:
			printf("fragment shader:\n%s\n", (char *)buf);
			break;
		case RD_PROGRAM: {
			struct state state = {
					.buf = buf,
					.sz = sz,
			};
			printf("############################################################\n");
			printf("program:\n");
			dump_program(&state);
			printf("############################################################\n");
			break;
		}
		case RD_GPU_ID:
			gpu_id = *((unsigned int *)buf);
			printf("gpu_id: %d\n", gpu_id);
			break;
		}
	}

	return 0;
}

