/*
   Graph Plotter for numerical data analysis.
   Copyright (C) 2022 Roman Belov <romblv@gmail.com>

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "lz4/lz4.h"

#include "plot.h"
#include "draw.h"
#include "lse.h"
#include "scheme.h"

extern SDL_RWops *TTF_RW_roboto_mono_normal();
extern SDL_RWops *TTF_RW_roboto_mono_thin();

double fp_nan()
{
	union {
		unsigned long long	l;
		double			f;
	}
	u = { 0xFFF8000000000000ULL };

	return u.f;
}

int fp_isfinite(double x)
{
	union {
		double			f;
		unsigned long long	l;
	}
	u = { x };

	return ((0x7FFUL & (unsigned long) (u.l >> 52)) != 0x7FFUL) ? 1 : 0;
}

plot_t *plotAlloc(draw_t *dw, scheme_t *sch)
{
	plot_t		*pl;
	int		N;

	pl = calloc(1, sizeof(plot_t));

	pl->dw = dw;
	pl->sch = sch;

	for (N = 0; N < PLOT_SKETCH_MAX - 1; ++N)
		pl->sketch[N].linked = N + 1;

	pl->sketch[PLOT_SKETCH_MAX - 1].linked = -1;

	pl->sketch_list_garbage = 0;
	pl->sketch_list_todraw = -1;
	pl->sketch_list_current = -1;
	pl->sketch_list_current_end = -1;

	for (N = 0; N < PLOT_FIGURE_MAX; ++N)
		pl->draw[N].list_self = -1;

	pl->layout_font_long = 11;
	pl->layout_border = 5;
	pl->layout_tick_tooth = 5;
	pl->layout_grid_dash = 2;
	pl->layout_grid_space = 8;
	pl->layout_drawing_dash = 8;
	pl->layout_drawing_space = 12;
	pl->layout_fence_dash = 10;
	pl->layout_fence_space = 10;
	pl->layout_fence_point = 10;

	pl->default_drawing = FIGURE_DRAWING_LINE;
	pl->default_width = 2;
	pl->transparency_mode = 1;
	pl->fprecision = 9;
	pl->lz4_compress = 0;

	return pl;
}

static void
plotSketchFree(plot_t *pl)
{
	int		N;

	plotSketchClean(pl);

	for (N = 0; N < PLOT_SKETCH_MAX; ++N) {

		if (pl->sketch[N].chunk != NULL) {

			free(pl->sketch[N].chunk);

			pl->sketch[N].chunk = NULL;
		}
	}
}

void plotClean(plot_t *pl)
{
	int		dN;

	drawPixmapClean(pl->dw);
	plotSketchFree(pl);

	for (dN = 0; dN < PLOT_DATASET_MAX; ++dN) {

		if (pl->data[dN].column_N != 0)
			plotDataClean(pl, dN);
	}

	free(pl);
}

static void
plotFontLayout(plot_t *pl)
{
	TTF_SizeUTF8(pl->font, "M", &pl->layout_font_long, &pl->layout_font_height);

	pl->layout_font_height = TTF_FontHeight(pl->font);

	pl->layout_axis_box = pl->layout_tick_tooth + pl->layout_font_height;
	pl->layout_label_box = pl->layout_font_height;
	pl->layout_mark = pl->layout_font_height / 4;
}

void plotFontDefault(plot_t *pl, int ttfnum, int ptsize, int style)
{
	if (pl->font != NULL) {

		TTF_CloseFont(pl->font);

		pl->font = NULL;
	}

	switch (ttfnum) {

		default:
			ttfnum = TTF_ID_ROBOTO_MONO_NORMAL;

		case TTF_ID_ROBOTO_MONO_NORMAL:
			pl->font = TTF_OpenFontRW(TTF_RW_roboto_mono_normal(), 1, ptsize);
			break;

		case TTF_ID_ROBOTO_MONO_THIN:
			pl->font = TTF_OpenFontRW(TTF_RW_roboto_mono_thin(), 1, ptsize);
			break;
	}

	TTF_SetFontStyle(pl->font, style);

	pl->layout_font_ttf = ttfnum;
	pl->layout_font_pt = ptsize;

	plotFontLayout(pl);
}

void plotFontOpen(plot_t *pl, const char *file, int ptsize, int style)
{
	if (pl->font != NULL) {

		TTF_CloseFont(pl->font);

		pl->font = NULL;
	}

	pl->font = TTF_OpenFont(file, ptsize);

	if (pl->font == NULL) {

		ERROR("TTF_OpenFont: \"%s\"\n", TTF_GetError());
		return ;
	}

	TTF_SetFontStyle(pl->font, style);

	pl->layout_font_ttf = 0;
	pl->layout_font_pt = ptsize;

	plotFontLayout(pl);
}

static void
plotDataChunkAlloc(plot_t *pl, int dN, int lN)
{
	int		N, kN, lSHIFT;

	lSHIFT = pl->data[dN].chunk_SHIFT;

	kN = (lN & pl->data[dN].chunk_MASK) ? 1 : 0;
	kN += lN >> lSHIFT;

	if (kN > PLOT_CHUNK_MAX) {

		kN = PLOT_CHUNK_MAX;
		lN = kN * (1UL << lSHIFT);
	}

	if (pl->lz4_compress != 0) {

		for (N = kN; N < PLOT_CHUNK_MAX; ++N) {

			if (pl->data[dN].compress[N].raw != NULL) {

				free(pl->data[dN].compress[N].raw);

				pl->data[dN].compress[N].raw = NULL;
			}
		}
	}
	else {
		for (N = 0; N < kN; ++N) {

			if (pl->data[dN].raw[N] == NULL) {

				pl->data[dN].raw[N] = (fval_t *) malloc(pl->data[dN].chunk_bSIZE);

				if (pl->data[dN].raw[N] == NULL) {

					lN = N * (1UL << lSHIFT);

					ERROR("Unable to allocate memory of %i dataset\n", dN);
					break;
				}
			}
		}

		for (N = kN; N < PLOT_CHUNK_MAX; ++N) {

			if (pl->data[dN].raw[N] != NULL) {

				free(pl->data[dN].raw[N]);

				pl->data[dN].raw[N] = NULL;
			}
		}
	}

	pl->data[dN].length_N = lN;
}

unsigned long long plotDataMemoryUsage(plot_t *pl, int dN)
{
	int			N;
	unsigned long long	bUSAGE;

	if (dN < 0 || dN >= PLOT_DATASET_MAX) {

		ERROR("Dataset number is out of range\n");
		return 0;
	}

	bUSAGE = 0;

	for (N = 0; N < PLOT_CHUNK_MAX; ++N) {

		if (pl->data[dN].raw[N] != NULL) {

			bUSAGE += pl->data[dN].chunk_bSIZE;
		}

		if (pl->data[dN].compress[N].raw != NULL) {

			bUSAGE += pl->data[dN].compress[N].length;
		}
	}

	return bUSAGE;
}

unsigned long long plotDataMemoryUncompressed(plot_t *pl, int dN)
{
	int			N;
	unsigned long long	bUSAGE;

	if (dN < 0 || dN >= PLOT_DATASET_MAX) {

		ERROR("Dataset number is out of range\n");
		return 0;
	}

	bUSAGE = 0;

	for (N = 0; N < PLOT_CHUNK_MAX; ++N) {

		if (		pl->data[dN].raw[N] != NULL
				|| pl->data[dN].compress[N].raw != NULL) {

			bUSAGE += pl->data[dN].chunk_bSIZE;
		}
	}

	return bUSAGE;
}

static int
plotDataCacheGetNode(plot_t *pl, int dN, int kN)
{
	int		N, kNOT, xN = -1;

	for (N = 0; N < PLOT_CHUNK_CACHE; ++N) {

		if (pl->data[dN].cache[N].raw == NULL) {

			xN = N;
			break;
		}
	}

	if (xN < 0) {

		kNOT = pl->data[dN].tail_N >> pl->data[dN].chunk_SHIFT;

		N = (pl->data[dN].cache_ID < PLOT_CHUNK_CACHE - 1)
			? pl->data[dN].cache_ID + 1 : 0;

		if (pl->data[dN].cache[N].chunk_N == kNOT) {

			N = (N < PLOT_CHUNK_CACHE - 1) ? N + 1 : 0;
		}

		xN = N;

		pl->data[dN].cache_ID = N;
	}

	return xN;
}

static void
plotDataCacheFetch(plot_t *pl, int dN, int kN)
{
	int		xN, kNZ, lzLEN;

	xN = plotDataCacheGetNode(pl, dN, kN);

	if (pl->data[dN].cache[xN].raw != NULL) {

		kNZ = pl->data[dN].cache[xN].chunk_N;

		if (pl->data[dN].cache[xN].dirty != 0) {

			lzLEN = LZ4_compressBound(pl->data[dN].chunk_bSIZE);

			if (pl->data[dN].compress[kNZ].raw != NULL) {

				free(pl->data[dN].compress[kNZ].raw);
			}

			pl->data[dN].compress[kNZ].raw = (void *) malloc(lzLEN);

			if (pl->data[dN].compress[kNZ].raw == NULL) {

				ERROR("Unable to allocate LZ4 memory of %i dataset\n", dN);
			}

			lzLEN = LZ4_compress_default(
					(const char *) pl->data[dN].cache[xN].raw,
					(char *) pl->data[dN].compress[kNZ].raw,
					pl->data[dN].chunk_bSIZE, lzLEN);

			if (lzLEN > 0) {

				pl->data[dN].compress[kNZ].raw =
					realloc(pl->data[dN].compress[kNZ].raw, lzLEN);
				pl->data[dN].compress[kNZ].length = lzLEN;
			}
			else {
				ERROR("Unable to compress the chunk of %i dataset\n", dN);

				free(pl->data[dN].compress[kNZ].raw);

				pl->data[dN].compress[kNZ].raw = NULL;
				pl->data[dN].compress[kNZ].length = 0;
			}
		}

		pl->data[dN].raw[kNZ] = NULL;
	}
	else {
		pl->data[dN].cache[xN].raw = (fval_t *) malloc(pl->data[dN].chunk_bSIZE);

		if (pl->data[dN].cache[xN].raw == NULL) {

			ERROR("Unable to allocate cache of %i dataset\n", dN);
		}
	}

	pl->data[dN].cache[xN].chunk_N = kN;
	pl->data[dN].cache[xN].dirty = 0;

	pl->data[dN].raw[kN] = pl->data[dN].cache[xN].raw;

	if (pl->data[dN].compress[kN].raw != NULL) {

		lzLEN = LZ4_decompress_safe(
				(const char *) pl->data[dN].compress[kN].raw,
				(char *) pl->data[dN].raw[kN],
				pl->data[dN].compress[kN].length,
				pl->data[dN].chunk_bSIZE);

		if (lzLEN != pl->data[dN].chunk_bSIZE) {

			ERROR("Unable to decompress the chunk of %i dataset\n", dN);
		}
	}
}

static void
plotDataChunkFetch(plot_t *pl, int dN, int kN)
{
	if (		   pl->data[dN].raw[kN] == NULL
			&& pl->data[dN].length_N != 0) {

		plotDataCacheFetch(pl, dN, kN);
	}
}

static void
plotDataChunkWrite(plot_t *pl, int dN, int kN)
{
	int		N;

	if (		   pl->data[dN].raw[kN] == NULL
			&& pl->data[dN].length_N != 0) {

		plotDataCacheFetch(pl, dN, kN);
	}

	if (pl->data[dN].raw[kN] != NULL) {

		for (N = 0; N < PLOT_CHUNK_CACHE; ++N) {

			if (pl->data[dN].cache[N].chunk_N == kN) {

				pl->data[dN].cache[N].dirty = 1;
				break;
			}
		}
	}
}

void plotDataAlloc(plot_t *pl, int dN, int cN, int lN)
{
	int		*map;
	int		N, bSIZE;

	if (dN < 0 || dN >= PLOT_DATASET_MAX) {

		ERROR("Dataset number is out of range\n");
		return ;
	}

	if (cN < 1) {

		ERROR("Number of columns is too few\n");
		return ;
	}

	if (lN < 1) {

		ERROR("Length of dataset is too short\n");
		return ;
	}

	if (pl->data[dN].column_N != 0) {

		if (pl->data[dN].column_N != cN) {

			ERROR("Number of columns cannot be changed\n");
			return ;
		}

		plotDataRangeCacheClean(pl, dN);
		plotDataChunkAlloc(pl, dN, lN);

		pl->data[dN].head_N = 0;
		pl->data[dN].tail_N = 0;
		pl->data[dN].id_N = 0;
		pl->data[dN].sub_N = 0;
	}
	else {
		pl->data[dN].column_N = cN;

		for (N = 0; N < 30; ++N) {

			bSIZE = sizeof(fval_t) * (cN + PLOT_SUBTRACT) * (1UL << N);

			if (bSIZE >= PLOT_CHUNK_SIZE) {

				pl->data[dN].chunk_SHIFT = N;
				pl->data[dN].chunk_MASK = (1UL << N) - 1UL;
				pl->data[dN].chunk_bSIZE = bSIZE;
				break;
			}
		}

		plotDataChunkAlloc(pl, dN, lN);

		pl->data[dN].cache_ID = 0;

		pl->data[dN].head_N = 0;
		pl->data[dN].tail_N = 0;
		pl->data[dN].id_N = 0;
		pl->data[dN].sub_N = 0;

		for (N = 0; N < PLOT_SUBTRACT; ++N) {

			pl->data[dN].sub[N].busy = SUBTRACT_FREE;
		}

		map = (int *) malloc(sizeof(int) * (cN + PLOT_SUBTRACT + 1));

		if (map == NULL) {

			ERROR("No memory allocated for %i map\n", dN);
			return ;
		}

		pl->data[dN].map = (int *) map + 1;

		for (N = -1; N < (cN + PLOT_SUBTRACT); ++N) {

			pl->data[dN].map[N] = -1;
		}
	}
}

void plotDataResize(plot_t *pl, int dN, int lN)
{
	if (dN < 0 || dN >= PLOT_DATASET_MAX) {

		ERROR("Dataset number is out of range\n");
		return ;
	}

	if (lN < 1) {

		ERROR("Length of dataset is too short\n");
		return ;
	}

	if (pl->data[dN].column_N != 0) {

		if (lN < pl->data[dN].length_N) {

			/* FIXME: In the case of length reduction we should
			 * compact the remaining data instead of resetting it.
			 * */

			pl->data[dN].head_N = 0;
			pl->data[dN].tail_N = 0;
			pl->data[dN].id_N = 0;
			pl->data[dN].sub_N = 0;
		}

		plotDataChunkAlloc(pl, dN, lN);
	}
}

int plotDataSpaceLeft(plot_t *pl, int dN)
{
	int		N;

	N = pl->data[dN].tail_N - pl->data[dN].head_N;
	N += (N < 0) ? pl->data[dN].length_N : 0;

	return pl->data[dN].length_N - N;
}

void plotDataGrowUp(plot_t *pl, int dN)
{
	int			lSHIFT, lN;

	lSHIFT = pl->data[dN].chunk_SHIFT;

	lN = pl->data[dN].length_N;
	lN = ((lN >> lSHIFT) + 1) << lSHIFT;

	plotDataResize(pl, dN, lN);
}

static const fval_t *
plotDataGet(plot_t *pl, int dN, int *rN)
{
	const fval_t	*row = NULL;
	int		lN, kN, jN;

	if (*rN != pl->data[dN].tail_N) {

		kN = *rN >> pl->data[dN].chunk_SHIFT;
		jN = *rN & pl->data[dN].chunk_MASK;

		if (pl->lz4_compress != 0) {

			plotDataChunkFetch(pl, dN, kN);
		}

		row = pl->data[dN].raw[kN];

		if (row != NULL) {

			row += (pl->data[dN].column_N + PLOT_SUBTRACT) * jN;

			lN = pl->data[dN].length_N;
			*rN = (*rN < lN - 1) ? *rN + 1 : 0;
		}
	}

	return row;
}

static void
plotDataRangeCacheWipe(plot_t *pl, int dN, int kN)
{
	int		N;

	for (N = 0; N < PLOT_RCACHE_SIZE; ++N) {

		if (		pl->rcache[N].busy != 0
				&& pl->rcache[N].data_N == dN) {

			pl->rcache[N].chunk[kN].computed = 0;
			pl->rcache[N].cached = 0;
		}
	}
}

static fval_t *
plotDataWrite(plot_t *pl, int dN, int *rN)
{
	fval_t		*row = NULL;
	int		lN, kN, jN;

	if (*rN != pl->data[dN].tail_N) {

		kN = *rN >> pl->data[dN].chunk_SHIFT;
		jN = *rN & pl->data[dN].chunk_MASK;

		if (pl->lz4_compress != 0) {

			plotDataChunkWrite(pl, dN, kN);
		}

		if (		   pl->rcache_wipe_data_N != dN
				|| pl->rcache_wipe_chunk_N != kN) {

			plotDataRangeCacheWipe(pl, dN, kN);

			pl->rcache_wipe_data_N = dN;
			pl->rcache_wipe_chunk_N = kN;
		}

		row = pl->data[dN].raw[kN];

		if (row != NULL) {

			row += (pl->data[dN].column_N + PLOT_SUBTRACT) * jN;

			lN = pl->data[dN].length_N;
			*rN = (*rN < lN - 1) ? *rN + 1 : 0;
		}
	}

	return row;
}

static void
plotDataSkip(plot_t *pl, int dN, int *rN, int *id_N, int sk_N)
{
	int		lN, N, tN;

	lN = pl->data[dN].length_N;

	N = *rN - pl->data[dN].head_N;
	N = (N < 0) ? N + lN : N;

	tN = pl->data[dN].tail_N - pl->data[dN].head_N;
	tN = (tN < 0) ? tN + lN : tN;

	sk_N = (N + sk_N < 0) ? - N : sk_N;
	sk_N = (N + sk_N > tN) ? tN - N : sk_N;

	N += sk_N;

	N = pl->data[dN].head_N + N;
	N = (N > lN - 1) ? N - lN : N;

	if (rN != NULL) {

		*rN = N;
	}

	if (id_N != NULL) {

		*id_N += sk_N;
	}
}

static int
plotDataChunkN(plot_t *pl, int dN, int rN)
{
	int		kN;

	kN = rN >> pl->data[dN].chunk_SHIFT;

	return kN;
}

static void
plotDataChunkSkip(plot_t *pl, int dN, int *rN, int *id_N)
{
	int		skip_N, wrap_N;

	skip_N = (1UL << pl->data[dN].chunk_SHIFT)
		- (*rN & pl->data[dN].chunk_MASK);

	wrap_N = pl->data[dN].length_N - *rN;
	skip_N = (wrap_N < skip_N) ? wrap_N : skip_N;

	plotDataSkip(pl, dN, rN, id_N, skip_N);
}

static void
plotDataResample(plot_t *pl, int dN, int cN_X, int cN_Y, int r_dN, int r_cN_X, int r_cN_Y)
{
	fval_t		*row, X, Y, r_X, r_Y, r_X_prev, r_Y_prev, Q;
	const fval_t	*r_row;
	int		rN, id_N, r_rN, r_id_N;

	rN = pl->data[dN].head_N;
	id_N = pl->data[dN].id_N;

	r_rN = pl->data[r_dN].head_N;
	r_id_N = pl->data[r_dN].id_N;

	do {
		r_row = plotDataGet(pl, r_dN, &r_rN);

		if (r_row == NULL)
			break;

		r_X = (r_cN_X < 0) ? r_id_N : r_row[r_cN_X];
		r_Y = (r_cN_Y < 0) ? r_id_N : r_row[r_cN_Y];

		r_id_N++;

		if (r_X == r_X)
			break;
	}
	while (1);

	if (r_id_N != pl->data[r_dN].id_N) {

		r_X_prev = r_X;
		r_Y_prev = r_Y;
	}
	else {
		ERROR("No data to resample in dataset %i column %i\n", r_dN, r_cN_X);
		return ;
	}

	do {
		row = plotDataWrite(pl, dN, &rN);

		if (row == NULL)
			break;

		X = (cN_X < 0) ? id_N : row[cN_X];

		if (fp_isfinite(X)) {

			do {
				if (r_X >= X)
					break;

				r_row = plotDataGet(pl, r_dN, &r_rN);

				if (r_row == NULL)
					break;

				if (fp_isfinite(r_X)) {

					r_X_prev = r_X;
					r_Y_prev = r_Y;
				}

				r_X = (r_cN_X < 0) ? r_id_N : r_row[r_cN_X];
				r_Y = (r_cN_Y < 0) ? r_id_N : r_row[r_cN_Y];

				r_id_N++;
			}
			while (1);

			if (r_X >= X) {

				if (r_X_prev <= X) {

					Q = (X - r_X_prev) / (r_X - r_X_prev);
					Y = r_Y_prev + (r_Y - r_Y_prev) * Q;
				}
				else {
					Y = r_Y_prev;
				}
			}
			else {
				Y = r_Y;
			}
		}
		else {
			Y = FP_NAN;
		}

		row[cN_Y] = Y;

		id_N++;
	}
	while (1);
}

static void
plotDataPolyfit(plot_t *pl, int dN, int cN_X, int cN_Y,
		double scale_X, double offset_X,
		double scale_Y, double offset_Y, int poly_N)
{
	const fval_t	*row;
	double		fval_X, fval_Y, fvec[LSE_FULL_MAX];
	int		N, xN, yN, kN, rN, id_N, job;

	lse_initiate(&pl->lsq, LSE_CASCADE_MAX, poly_N + 1, 1);

	xN = plotDataRangeCacheFetch(pl, dN, cN_X);
	yN = plotDataRangeCacheFetch(pl, dN, cN_Y);

	rN = pl->data[dN].head_N;
	id_N = pl->data[dN].id_N;

	do {
		kN = plotDataChunkN(pl, dN, rN);
		job = 1;

		if (xN >= 0 && pl->rcache[xN].chunk[kN].computed != 0) {

			if (pl->rcache[xN].chunk[kN].finite != 0) {

				fvec[0] = pl->rcache[xN].chunk[kN].fmin * scale_X + offset_X;
				fvec[1] = pl->rcache[xN].chunk[kN].fmax * scale_X + offset_X;

				if (fvec[0] > 1. || fvec[1] < 0.) {

					job = 0;
				}
			}
			else {
				job = 0;
			}
		}

		if (yN >= 0 && pl->rcache[yN].chunk[kN].computed != 0) {

			if (pl->rcache[yN].chunk[kN].finite != 0) {

				fvec[0] = pl->rcache[yN].chunk[kN].fmin * scale_Y + offset_Y;
				fvec[1] = pl->rcache[yN].chunk[kN].fmax * scale_Y + offset_Y;

				if (fvec[0] > 1. || fvec[1] < 0.) {

					job = 0;
				}
			}
			else {
				job = 0;
			}
		}

		if (job != 0) {

			do {
				if (kN != plotDataChunkN(pl, dN, rN))
					break;

				row = plotDataGet(pl, dN, &rN);

				if (row == NULL)
					break;

				fval_X = (cN_X < 0) ? id_N : row[cN_X];
				fval_Y = (cN_Y < 0) ? id_N : row[cN_Y];

				if (fp_isfinite(fval_X) && fp_isfinite(fval_Y)) {

					fvec[0] = fval_X * scale_X + offset_X;
					fvec[1] = fval_Y * scale_Y + offset_Y;

					if (		   fvec[0] >= 0. && fvec[0] <= 1.
							&& fvec[1] >= 0. && fvec[1] <= 1.) {

						fvec[0] = 1.;

						for (N = 0; N < poly_N; ++N)
							fvec[N + 1] = fvec[N] * fval_X;

						fvec[poly_N + 1] = fval_Y;

						lse_insert(&pl->lsq, fvec);
					}
				}

				id_N++;
			}
			while (1);
		}
		else {
			plotDataChunkSkip(pl, dN, &rN, &id_N);
		}

		if (rN == pl->data[dN].tail_N)
			break;
	}
	while (1);

	lse_finalise(&pl->lsq);
}

void plotDataSubtract(plot_t *pl, int dN, int sN)
{
	fval_t		*row, X_1, X_2, X_3;
	double		scale, offset, gain;
	int		cN, cN_1, cN_2, cN_3, dN_1;
	int		rN, rS, sE, id_N, id_S, mode;

	if (dN < 0 || dN >= PLOT_DATASET_MAX) {

		ERROR("Dataset number is out of range\n");
		return ;
	}

	if (sN < -1 || sN >= PLOT_SUBTRACT) {

		ERROR("Subtract number %i is out of range\n", sN);
		return ;
	}

	if (sN < 0) {

		sN = 0;
		sE = PLOT_SUBTRACT;

		rS = pl->data[dN].sub_N;
		pl->data[dN].sub_N = pl->data[dN].tail_N;
	}
	else {
		sE = sN;
		rS = pl->data[dN].head_N;
	}

	do {
		mode = pl->data[dN].sub[sN].busy;
		cN = sN + pl->data[dN].column_N;

		rN = rS;
		id_N = pl->data[dN].id_N;

		id_S = rS - pl->data[dN].head_N;
		id_S += (id_S < 0) ? pl->data[dN].length_N : 0;

		id_N += id_S;

		if (mode == SUBTRACT_TIME_UNWRAP) {

			if (rS == pl->data[dN].head_N) {

				pl->data[dN].sub[sN].op.time.unwrap = (double) 0.;
				pl->data[dN].sub[sN].op.time.prev = FP_NAN;
				pl->data[dN].sub[sN].op.time.prev2 = FP_NAN;
			}

			cN_1 = pl->data[dN].sub[sN].op.time.column_1;
			offset = pl->data[dN].sub[sN].op.time.unwrap;
			X_2 = (fval_t) pl->data[dN].sub[sN].op.time.prev;
			X_3 = (fval_t) pl->data[dN].sub[sN].op.time.prev2;

			do {
				row = plotDataWrite(pl, dN, &rN);

				if (row == NULL)
					break;

				X_1 = (cN_1 < 0) ? id_N : row[cN_1];

				if (X_1 < X_2) {

					offset += X_2 - X_1;

					if (X_3 < X_2) {

						offset += X_2 - X_3;
					}
				}

				row[cN] = X_1 + offset;

				if (fp_isfinite(X_1)) {

					X_3 = X_2;
					X_2 = X_1;
				}

				id_N++;
			}
			while (1);

			pl->data[dN].sub[sN].op.time.unwrap = offset;
			pl->data[dN].sub[sN].op.time.prev = (double) X_2;
			pl->data[dN].sub[sN].op.time.prev2 = (double) X_3;
		}
		else if (mode == SUBTRACT_SCALE) {

			cN_1 = pl->data[dN].sub[sN].op.scale.column_1;
			scale = pl->data[dN].sub[sN].op.scale.scale;
			offset = pl->data[dN].sub[sN].op.scale.offset;

			do {
				row = plotDataWrite(pl, dN, &rN);

				if (row == NULL)
					break;

				X_1 = (cN_1 < 0) ? id_N : row[cN_1];
				X_1 = X_1 * scale + offset;

				row[cN] = X_1;

				id_N++;
			}
			while (1);
		}
		else if (mode == SUBTRACT_BINARY_SUBTRACTION) {

			cN_1 = pl->data[dN].sub[sN].op.binary.column_1;
			cN_2 = pl->data[dN].sub[sN].op.binary.column_2;

			do {
				row = plotDataWrite(pl, dN, &rN);

				if (row == NULL)
					break;

				X_1 = (cN_1 < 0) ? id_N : row[cN_1];
				X_2 = (cN_2 < 0) ? id_N : row[cN_2];

				row[cN] = X_1 - X_2;

				id_N++;
			}
			while (1);
		}
		else if (mode == SUBTRACT_BINARY_ADDITION) {

			cN_1 = pl->data[dN].sub[sN].op.binary.column_1;
			cN_2 = pl->data[dN].sub[sN].op.binary.column_2;

			do {
				row = plotDataWrite(pl, dN, &rN);

				if (row == NULL)
					break;

				X_1 = (cN_1 < 0) ? id_N : row[cN_1];
				X_2 = (cN_2 < 0) ? id_N : row[cN_2];

				row[cN] = X_1 + X_2;

				id_N++;
			}
			while (1);
		}
		else if (mode == SUBTRACT_BINARY_MULTIPLICATION) {

			cN_1 = pl->data[dN].sub[sN].op.binary.column_1;
			cN_2 = pl->data[dN].sub[sN].op.binary.column_2;

			do {
				row = plotDataWrite(pl, dN, &rN);

				if (row == NULL)
					break;

				X_1 = (cN_1 < 0) ? id_N : row[cN_1];
				X_2 = (cN_2 < 0) ? id_N : row[cN_2];

				row[cN] = X_1 * X_2;

				id_N++;
			}
			while (1);
		}
		else if (mode == SUBTRACT_BINARY_HYPOTENUSE) {

			cN_1 = pl->data[dN].sub[sN].op.binary.column_1;
			cN_2 = pl->data[dN].sub[sN].op.binary.column_2;

			do {
				row = plotDataWrite(pl, dN, &rN);

				if (row == NULL)
					break;

				X_1 = (cN_1 < 0) ? id_N : row[cN_1];
				X_2 = (cN_2 < 0) ? id_N : row[cN_2];

				row[cN] = sqrt(X_1 * X_1 + X_2 * X_2);

				id_N++;
			}
			while (1);
		}
		else if (mode == SUBTRACT_FILTER_DIFFERENCE) {

			if (rS == pl->data[dN].head_N) {

				pl->data[dN].sub[sN].op.filter.state = FP_NAN;
			}

			cN_1 = pl->data[dN].sub[sN].op.filter.column_1;
			X_2 = (fval_t) pl->data[dN].sub[sN].op.filter.state;

			do {
				row = plotDataWrite(pl, dN, &rN);

				if (row == NULL)
					break;

				X_1 = (cN_1 < 0) ? id_N : row[cN_1];

				row[cN] = X_1 - X_2;

				X_2 = X_1;

				id_N++;
			}
			while (1);

			pl->data[dN].sub[sN].op.filter.state = (double) X_2;
		}
		else if (mode == SUBTRACT_FILTER_CUMULATIVE) {

			if (rS == pl->data[dN].head_N) {

				pl->data[dN].sub[sN].op.filter.state = 0.;
			}

			cN_1 = pl->data[dN].sub[sN].op.filter.column_1;
			X_2 = (fval_t) pl->data[dN].sub[sN].op.filter.state;

			do {
				row = plotDataWrite(pl, dN, &rN);

				if (row == NULL)
					break;

				X_1 = (cN_1 < 0) ? id_N : row[cN_1];

				if (fp_isfinite(X_1)) {

					X_2 += X_1;
				}

				row[cN] = X_2;

				id_N++;
			}
			while (1);

			pl->data[dN].sub[sN].op.filter.state = (double) X_2;
		}
		else if (mode == SUBTRACT_FILTER_BITMASK) {

			int		shift_1, temp_1;
			unsigned long	mask_1;

			cN_1 = pl->data[dN].sub[sN].op.filter.column_1;
			shift_1 = (int) pl->data[dN].sub[sN].op.filter.arg_1;
			temp_1 = (int) pl->data[dN].sub[sN].op.filter.arg_2;

			for (mask_1 = 0UL; temp_1 >= shift_1; --temp_1)
				mask_1 |= (1UL << temp_1);

			do {
				row = plotDataWrite(pl, dN, &rN);

				if (row == NULL)
					break;

				X_1 = (cN_1 < 0) ? id_N : row[cN_1];

				temp_1 = ((unsigned long) X_1 & mask_1) >> shift_1;
				row[cN] = (fval_t) temp_1;

				id_N++;
			}
			while (1);
		}
		else if (mode == SUBTRACT_FILTER_LOW_PASS) {

			if (rS == pl->data[dN].head_N) {

				pl->data[dN].sub[sN].op.filter.state = FP_NAN;
			}

			cN_1 = pl->data[dN].sub[sN].op.filter.column_1;
			gain = pl->data[dN].sub[sN].op.filter.arg_1;
			X_2 = (fval_t) pl->data[dN].sub[sN].op.filter.state;

			do {
				row = plotDataWrite(pl, dN, &rN);

				if (row == NULL)
					break;

				X_1 = (cN_1 < 0) ? id_N : row[cN_1];

				if (fp_isfinite(X_1)) {

					if (fp_isfinite(X_2)) {

						X_2 += (X_1 - X_2) * gain;
					}
					else {
						X_2 = X_1;
					}
				}

				row[cN] = X_2;

				id_N++;
			}
			while (1);

			pl->data[dN].sub[sN].op.filter.state = (double) X_2;
		}
		else if (mode == SUBTRACT_RESAMPLE) {

			if (rS == pl->data[dN].head_N) {

				/* FIXME: Unable to resample in real time.
				 * */

				cN_1 = pl->data[dN].sub[sN].op.resample.column_X;
				cN_2 = pl->data[dN].sub[sN].op.resample.column_in_X;
				cN_3 = pl->data[dN].sub[sN].op.resample.column_in_Y;
				dN_1 = pl->data[dN].sub[sN].op.resample.in_data_N;

				plotDataResample(pl, dN, cN_1, cN, dN_1, cN_2, cN_3);
			}
		}
		else if (mode == SUBTRACT_POLYFIT) {

			const double	*coefs;
			int		N, poly_N;

			cN_1 = pl->data[dN].sub[sN].op.polyfit.column_X;
			poly_N = pl->data[dN].sub[sN].op.polyfit.poly_N;
			coefs = pl->data[dN].sub[sN].op.polyfit.coefs;

			do {
				row = plotDataWrite(pl, dN, &rN);

				if (row == NULL)
					break;

				X_1 = (cN_1 < 0) ? id_N : row[cN_1];
				X_2 = coefs[poly_N];

				for (N = poly_N - 1; N >= 0; --N)
					X_2 = X_2 * X_1 + coefs[N];

				row[cN] = X_2;

				id_N++;
			}
			while (1);
		}

		sN++;
	}
	while (sN < sE);
}

void plotDataSubtractClean(plot_t *pl)
{
	int		dN, N;

	for (dN = 0; dN < PLOT_DATASET_MAX; ++dN) {

		if (pl->data[dN].column_N != 0) {

			for (N = 0; N < PLOT_SUBTRACT; ++N) {

				pl->data[dN].sub[N].busy = SUBTRACT_FREE;
			}
		}
	}
}

void plotDataInsert(plot_t *pl, int dN, const fval_t *row)
{
	fval_t		*place;
	int		cN, lN, hN, tN, kN, jN, sN;

	cN = pl->data[dN].column_N;
	lN = pl->data[dN].length_N;
	hN = pl->data[dN].head_N;
	tN = pl->data[dN].tail_N;

	kN = tN >> pl->data[dN].chunk_SHIFT;
	jN = tN & pl->data[dN].chunk_MASK;

	if (pl->lz4_compress != 0) {

		plotDataChunkWrite(pl, dN, kN);
	}

	if (		   pl->rcache_wipe_data_N != dN
			|| pl->rcache_wipe_chunk_N != kN) {

		plotDataRangeCacheWipe(pl, dN, kN);

		pl->rcache_wipe_data_N = dN;
		pl->rcache_wipe_chunk_N = kN;
	}

	place = pl->data[dN].raw[kN];

	if (place != NULL) {

		place += (cN + PLOT_SUBTRACT) * jN;

		memcpy(place, row, cN * sizeof(fval_t));

		tN = (tN < lN - 1) ? tN + 1 : 0;

		if (hN == tN) {

			pl->data[dN].id_N++;

			hN = (hN < lN - 1) ? hN + 1 : 0;
			pl->data[dN].head_N = hN;

			sN = pl->data[dN].sub_N;
			pl->data[dN].sub_N = (sN == tN) ? hN : sN;
		}

		pl->data[dN].tail_N = tN;
	}
}

void plotDataClean(plot_t *pl, int dN)
{
	int		N;

	if (pl->data[dN].column_N != 0) {

		pl->data[dN].column_N = 0;
		pl->data[dN].length_N = 0;

		if (pl->lz4_compress != 0) {

			for (N = 0; N < PLOT_CHUNK_CACHE; ++N) {

				if (pl->data[dN].cache[N].raw) {

					free(pl->data[dN].cache[N].raw);

					pl->data[dN].cache[N].raw = NULL;
				}
			}

			for (N = 0; N < PLOT_CHUNK_MAX; ++N) {

				pl->data[dN].raw[N] = NULL;

				if (pl->data[dN].compress[N].raw != NULL) {

					free(pl->data[dN].compress[N].raw);

					pl->data[dN].compress[N].raw = NULL;
				}
			}
		}
		else {
			for (N = 0; N < PLOT_CHUNK_MAX; ++N) {

				if (pl->data[dN].raw[N] != NULL) {

					free(pl->data[dN].raw[N]);

					pl->data[dN].raw[N] = NULL;
				}
			}
		}

		free(pl->data[dN].map - 1);

		pl->data[dN].map = NULL;
	}
}

static int
plotDataRangeCacheGetNode(plot_t *pl, int dN, int cN)
{
	int		N, xN = -1;

	for (N = 0; N < PLOT_RCACHE_SIZE; ++N) {

		if (		pl->rcache[N].busy != 0
				&& pl->rcache[N].data_N == dN
				&& pl->rcache[N].column_N == cN) {

			xN = N;
			break;
		}
	}

	return xN;
}

void plotDataRangeCacheClean(plot_t *pl, int dN)
{
	int		N;

	for (N = 0; N < PLOT_RCACHE_SIZE; ++N) {

		if (pl->rcache[N].data_N == dN)
			pl->rcache[N].busy = 0;
	}
}

void plotDataRangeCacheSubtractClean(plot_t *pl)
{
	int		N, dN;

	for (N = 0; N < PLOT_RCACHE_SIZE; ++N) {

		if (pl->rcache[N].busy != 0) {

			dN = pl->rcache[N].data_N;

			if (		dN >= 0 && dN < PLOT_DATASET_MAX
					&& pl->data[dN].column_N != 0) {

				if (pl->rcache[N].column_N >= pl->data[dN].column_N)
					pl->rcache[N].busy = 0;
			}
		}
	}
}

int plotDataRangeCacheFetch(plot_t *pl, int dN, int cN)
{
	const fval_t	*row;
	fval_t		fval, fmin, fmax, ymin, ymax;
	int		N, xN, rN, id_N, kN;
	int		job, finite, started;

	xN = plotDataRangeCacheGetNode(pl, dN, cN);

	if (xN >= 0) {

		if (pl->rcache[xN].cached != 0)
			return xN;
	}
	else {
		xN = pl->rcache_ID;

		pl->rcache_ID = (pl->rcache_ID < PLOT_RCACHE_SIZE - 1)
			? pl->rcache_ID + 1 : 0;

		for (N = 0; N < PLOT_CHUNK_MAX; ++N) {

			pl->rcache[xN].chunk[N].computed = 0;
		}
	}

	rN = pl->data[dN].head_N;
	id_N = pl->data[dN].id_N;

	fmin = (fval_t) 0.;
	fmax = (fval_t) 0.;

	started = 0;

	do {
		kN = plotDataChunkN(pl, dN, rN);

		if (pl->rcache[xN].chunk[kN].computed != 0) {

			if (kN == plotDataChunkN(pl, dN, pl->data[dN].tail_N)) {

				job = 1;

				finite = pl->rcache[xN].chunk[kN].finite;
				ymin = pl->rcache[xN].chunk[kN].fmin;
				ymax = pl->rcache[xN].chunk[kN].fmax;
			}
			else {
				job = 0;
			}
		}
		else {
			finite = 0;
			job = 1;
		}

		if (job != 0) {

			do {
				if (kN != plotDataChunkN(pl, dN, rN))
					break;

				row = plotDataGet(pl, dN, &rN);

				if (row == NULL)
					break;

				fval = (cN < 0) ? id_N : row[cN];

				if (fp_isfinite(fval)) {

					if (finite != 0) {

						ymin = (fval < ymin) ? fval : ymin;
						ymax = (fval > ymax) ? fval : ymax;
					}
					else {
						finite = 1;

						ymin = fval;
						ymax = fval;
					}
				}

				id_N++;
			}
			while (1);

			pl->rcache[xN].chunk[kN].computed = 1;
			pl->rcache[xN].chunk[kN].finite = finite;

			if (finite != 0) {

				pl->rcache[xN].chunk[kN].fmin = ymin;
				pl->rcache[xN].chunk[kN].fmax = ymax;
			}
		}
		else {
			plotDataChunkSkip(pl, dN, &rN, &id_N);
		}

		if (pl->rcache[xN].chunk[kN].finite != 0) {

			if (started != 0) {

				fmin = (pl->rcache[xN].chunk[kN].fmin < fmin)
					? pl->rcache[xN].chunk[kN].fmin : fmin;

				fmax = (pl->rcache[xN].chunk[kN].fmax > fmax)
					? pl->rcache[xN].chunk[kN].fmax : fmax;
			}
			else {
				started = 1;

				fmin = pl->rcache[xN].chunk[kN].fmin;
				fmax = pl->rcache[xN].chunk[kN].fmax;
			}
		}

		if (rN == pl->data[dN].tail_N)
			break;
	}
	while (1);

	pl->rcache[xN].busy = 1;
	pl->rcache[xN].data_N = dN;
	pl->rcache[xN].column_N = cN;
	pl->rcache[xN].cached = 1;
	pl->rcache[xN].fmin = fmin;
	pl->rcache[xN].fmax = fmax;

	pl->rcache_wipe_data_N = -1;
	pl->rcache_wipe_chunk_N = -1;

	return xN;
}

static void
plotDataRangeGet(plot_t *pl, int dN, int cN, double *pmin, double *pmax)
{
	int		xN;

	xN = plotDataRangeCacheFetch(pl, dN, cN);

	*pmin = (double) pl->rcache[xN].fmin;
	*pmax = (double) pl->rcache[xN].fmax;
}

static void
plotDataRangeCond(plot_t *pl, int dN, int cN, int cN_cond, int *pflag,
		double scale, double offset, double *pmin, double *pmax)
{
	const fval_t	*row;
	double		fval, fmin, fmax, fcond, vmin, vmax;
	int		xN, yN, kN, rN, id_N, job, started;

	xN = plotDataRangeCacheFetch(pl, dN, cN_cond);
	yN = plotDataRangeCacheFetch(pl, dN, cN);

	rN = pl->data[dN].head_N;
	id_N = pl->data[dN].id_N;

	started = *pflag;
	fmin = *pmin;
	fmax = *pmax;

	do {
		kN = plotDataChunkN(pl, dN, rN);
		job = 1;

		if (xN >= 0 && pl->rcache[xN].chunk[kN].computed != 0) {

			if (pl->rcache[xN].chunk[kN].finite != 0) {

				vmin = pl->rcache[xN].chunk[kN].fmin * scale + offset;
				vmax = pl->rcache[xN].chunk[kN].fmax * scale + offset;

				if (yN >= 0	&& pl->rcache[yN].chunk[kN].computed != 0
						&& vmin >= 0. && vmin <= 1.
						&& vmax >= 0. && vmax <= 1.) {

					job = 0;

					if (pl->rcache[yN].chunk[kN].finite != 0) {

						if (started != 0) {

							fmin = (pl->rcache[yN].chunk[kN].fmin < fmin)
								? pl->rcache[yN].chunk[kN].fmin : fmin;

							fmax = (pl->rcache[yN].chunk[kN].fmax > fmax)
								? pl->rcache[yN].chunk[kN].fmax : fmax;
						}
						else {
							started = 1;

							fmin = pl->rcache[yN].chunk[kN].fmin;
							fmax = pl->rcache[yN].chunk[kN].fmax;
						}
					}
				}
				else if (vmin > 1. || vmax < 0.) {

					job = 0;
				}
			}
			else {
				job = 0;
			}
		}

		if (job != 0) {

			do {
				if (kN != plotDataChunkN(pl, dN, rN))
					break;

				row = plotDataGet(pl, dN, &rN);

				if (row == NULL)
					break;

				fval = (cN < 0) ? id_N : row[cN];
				fcond = (cN_cond < 0) ? id_N : row[cN_cond];

				fcond = fcond * scale + offset;

				if (fcond >= 0. && fcond <= 1.) {

					if (fp_isfinite(fval)) {

						if (started != 0) {

							fmin = (fval < fmin) ? fval : fmin;
							fmax = (fval > fmax) ? fval : fmax;
						}
						else {
							started = 1;

							fmin = fval;
							fmax = fval;
						}
					}
				}

				id_N++;
			}
			while (1);
		}
		else {
			plotDataChunkSkip(pl, dN, &rN, &id_N);
		}

		if (rN == pl->data[dN].tail_N)
			break;
	}
	while (1);

	*pflag = started;
	*pmin = fmin;
	*pmax = fmax;
}

static void
plotDataRangeAxis(plot_t *pl, int dN, int cN, int aN, double *pmin, double *pmax)
{
	double		scale, offset, fmin, fmax;
	int		xN, yN, fN, cN_cond, job, started;

	started = 0;

	fmin = 0.;
	fmax = 0.;

	for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		job = 0;

		if (pl->figure[fN].busy != 0 && pl->figure[fN].hidden == 0
				&& pl->figure[fN].data_N == dN) {

			if (pl->figure[fN].axis_X == aN && pl->figure[fN].column_Y == cN) {

				scale = 1.;
				offset = 0.;

				cN_cond = pl->figure[fN].column_X;
				job = 1;
			}
			else if (pl->figure[fN].axis_Y == aN && pl->figure[fN].column_X == cN) {

				scale = 1.;
				offset = 0.;

				cN_cond = pl->figure[fN].column_Y;
				job = 1;
			}

			xN = pl->figure[fN].axis_X;
			yN = pl->figure[fN].axis_Y;

			if (pl->axis[xN].slave != 0 && pl->axis[xN].slave_N == aN
					&& pl->figure[fN].column_Y == cN) {

				scale = pl->axis[xN].scale;
				offset = pl->axis[xN].offset;

				cN_cond = pl->figure[fN].column_X;
				job = 1;
			}
			else if (pl->axis[yN].slave != 0 && pl->axis[yN].slave_N == aN
					&& pl->figure[fN].column_X == cN) {

				scale = pl->axis[yN].scale;
				offset = pl->axis[yN].offset;

				cN_cond = pl->figure[fN].column_Y;
				job = 1;
			}
		}

		if (job != 0) {

			scale *= pl->axis[aN].scale;
			offset = offset * pl->axis[aN].scale + pl->axis[aN].offset;

			plotDataRangeCond(pl, dN, cN, cN_cond, &started,
					scale, offset, &fmin, &fmax);
		}
	}

	if (started != 0) {

		*pmin = fmin;
		*pmax = fmax;
	}
	else {
		plotDataRangeGet(pl, dN, cN, pmin, pmax);
	}
}

static const fval_t *
plotDataSliceGet(plot_t *pl, int dN, int cN, double fsamp, int *m_id_N)
{
	const fval_t	*row;
	double		fval, fbest, fmin, fmax, fneard;
	int		xN, lN, rN, id_N, kN, kN_rep, best_N;
	int		job, started, span;

	xN = plotDataRangeCacheFetch(pl, dN, cN);

	rN = pl->data[dN].head_N;
	id_N = pl->data[dN].id_N;

	kN_rep = -1;

	started = 0;
	span = 0;

	do {
		kN = plotDataChunkN(pl, dN, rN);
		job = 1;

		if (xN >= 0 && pl->rcache[xN].chunk[kN].computed != 0) {

			if (pl->rcache[xN].chunk[kN].finite != 0) {

				fmin = pl->rcache[xN].chunk[kN].fmin;
				fmax = pl->rcache[xN].chunk[kN].fmax;

				if (fsamp < fmin || fsamp > fmax) {

					job = 0;

					fmin = fabs(fmin - fsamp);
					fmax = fabs(fmax - fsamp);

					if (kN_rep >= 0) {

						if (fmin < fneard) {

							fneard = fmin;
							kN_rep = kN;
						}

						if (fmax < fneard) {

							fneard = fmax;
							kN_rep = kN;
						}
					}
					else {
						fneard = (fmin < fmax)
							? fmin : fmax;

						kN_rep = kN;
					}
				}
			}
			else {
				job = 0;
			}
		}

		if (job != 0) {

			span++;

			do {
				if (kN != plotDataChunkN(pl, dN, rN))
					break;

				row = plotDataGet(pl, dN, &rN);

				if (row == NULL)
					break;

				fval = (cN < 0) ? id_N : row[cN];

				if (fp_isfinite(fval)) {

					if (started != 0) {

						fval = fabs(fsamp - fval);

						if (fval < fbest) {

							fbest = fval;
							best_N = id_N;
						}
					}
					else {
						started = 1;

						fbest = fabs(fsamp - fval);
						best_N = id_N;
					}
				}

				id_N++;
			}
			while (1);

			if (span >= PLOT_SLICE_SPAN)
				break;
		}
		else {
			plotDataChunkSkip(pl, dN, &rN, &id_N);
		}

		if (rN == pl->data[dN].tail_N)
			break;
	}
	while (1);

	if (		started == 0
			&& kN_rep >= 0) {

		rN = pl->data[dN].head_N;
		id_N = pl->data[dN].id_N;

		do {
			kN = plotDataChunkN(pl, dN, rN);
			job = 1;

			if (kN == kN_rep) {

				do {
					if (kN != plotDataChunkN(pl, dN, rN))
						break;

					row = plotDataGet(pl, dN, &rN);

					if (row == NULL)
						break;

					fval = (cN < 0) ? id_N : row[cN];

					if (fp_isfinite(fval)) {

						if (started != 0) {

							fval = fabs(fsamp - fval);

							if (fval < fbest) {

								fbest = fval;
								best_N = id_N;
							}
						}
						else {
							started = 1;

							fbest = fabs(fsamp - fval);
							best_N = id_N;
						}
					}

					id_N++;
				}
				while (1);
			}
			else {
				plotDataChunkSkip(pl, dN, &rN, &id_N);
			}

			if (rN == pl->data[dN].tail_N)
				break;
		}
		while (1);
	}

	if (started != 0) {

		*m_id_N = best_N;

		lN = pl->data[dN].length_N;

		rN = pl->data[dN].head_N + (best_N - pl->data[dN].id_N);
		rN = (rN > lN - 1) ? rN - lN : rN;

		row = plotDataGet(pl, dN, &rN);
	}
	else {
		row = NULL;
	}

	return row;
}

void plotAxisLabel(plot_t *pl, int aN, const char *label)
{
	if (aN < 0 || aN >= PLOT_AXES_MAX) {

		ERROR("Axis number is out of range\n");
		return ;
	}

	if (label[0] != 0) {

		strcpy(pl->axis[aN].label, label);
	}
}

void plotAxisScaleManual(plot_t *pl, int aN, double min, double max)
{
	if (aN < 0 || aN >= PLOT_AXES_MAX) {

		ERROR("Axis number is out of range\n");
		return ;
	}

	if (pl->axis[aN].busy == AXIS_FREE)
		return ;

	if (pl->axis[aN].slave != 0)
		return ;

	pl->axis[aN].scale = 1. / (max - min);
	pl->axis[aN].offset = - min / (max - min);
}

void plotAxisScaleAutoCond(plot_t *pl, int aN, int bN)
{
	double		min, max, fmin, fmax;
	double		scale, offset;
	int		fN, dN, cN, xN, yN, started;

	if (aN < 0 || aN >= PLOT_AXES_MAX) {

		ERROR("Axis number is out of range\n");
		return ;
	}

	if (bN < -1 || bN >= PLOT_AXES_MAX) {

		ERROR("Conditional axis number is out of range\n");
		return ;
	}

	if (pl->axis[aN].busy == AXIS_FREE)
		return ;

	if (pl->axis[aN].slave != 0)
		return ;

	started = 0;

	for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		if (pl->figure[fN].busy != 0 && pl->figure[fN].hidden == 0) {

			dN = pl->figure[fN].data_N;

			do {
				if (pl->figure[fN].axis_X == aN)
					cN = pl->figure[fN].column_X;
				else if (pl->figure[fN].axis_Y == aN)
					cN = pl->figure[fN].column_Y;
				else
					break;

				if (bN == -1) {

					plotDataRangeGet(pl, dN, cN, &min, &max);
				}
				else {
					plotDataRangeAxis(pl, dN, cN, bN, &min, &max);
				}

				if (started != 0) {

					fmin = (min < fmin) ? min : fmin;
					fmax = (max > fmax) ? max : fmax;
				}
				else {
					started = 1;

					fmin = min;
					fmax = max;
				}
			}
			while (0);

			do {
				xN = pl->figure[fN].axis_X;
				yN = pl->figure[fN].axis_Y;

				if (pl->axis[xN].slave != 0 && pl->axis[xN].slave_N == aN) {

					cN = pl->figure[fN].column_X;
					scale = pl->axis[xN].scale;
					offset = pl->axis[xN].offset;
				}
				else if (pl->axis[yN].slave != 0 && pl->axis[yN].slave_N == aN) {

					cN = pl->figure[fN].column_Y;
					scale = pl->axis[yN].scale;
					offset = pl->axis[yN].offset;
				}
				else
					break;

				if (bN == -1) {

					plotDataRangeGet(pl, dN, cN, &min, &max);
				}
				else {
					plotDataRangeAxis(pl, dN, cN, bN, &min, &max);
				}

				min = min * scale + offset;
				max = max * scale + offset;

				if (started != 0) {

					fmin = (min < fmin) ? min : fmin;
					fmax = (max > fmax) ? max : fmax;
				}
				else {
					started = 1;

					fmin = min;
					fmax = max;
				}
			}
			while (0);
		}
	}

	if (started != 0) {

		if (fmin == fmax) {

			fmin += (fval_t) -1.;
			fmax += (fval_t) +1.;
		}

		plotAxisScaleManual(pl, aN, fmin, fmax);

		if (pl->axis[aN].busy == AXIS_BUSY_X) {

			plotAxisScaleManual(pl, aN,
				plotAxisConvInv(pl, aN, pl->viewport.min_x - pl->layout_mark),
				plotAxisConvInv(pl, aN, pl->viewport.max_x + pl->layout_mark));
		}
		else if (pl->axis[aN].busy == AXIS_BUSY_Y) {

			plotAxisScaleManual(pl, aN,
				plotAxisConvInv(pl, aN, pl->viewport.max_y + pl->layout_mark),
				plotAxisConvInv(pl, aN, pl->viewport.min_y - pl->layout_mark));
		}
	}
}

void plotAxisScaleLock(plot_t *pl, int lock)
{
	int		aN;

	for (aN = 0; aN < PLOT_AXES_MAX; ++aN) {

		pl->axis[aN].lock_scale = lock;
	}
}

void plotAxisScaleAuto(plot_t *pl, int aN)
{
	plotAxisScaleAutoCond(pl, aN, -1);

	pl->axis[aN].lock_scale = 1;
}

void plotAxisScaleDefault(plot_t *pl)
{
	int		aN;

	for (aN = 0; aN < PLOT_AXES_MAX; ++aN) {

		if (pl->axis[aN].busy != AXIS_FREE
			&& pl->axis[aN].lock_scale) {

			plotAxisScaleAuto(pl, aN);
		}
	}
}

void plotAxisScaleZoom(plot_t *pl, int aN, int origin, double zoom)
{
	if (aN < 0 || aN >= PLOT_AXES_MAX) {

		ERROR("Axis number is out of range\n");
		return ;
	}

	if (pl->axis[aN].slave != 0)
		return ;

	if (pl->axis[aN].busy == AXIS_BUSY_X) {

		pl->axis[aN].offset = pl->axis[aN].offset * zoom
			+ (double) (pl->viewport.min_x - origin) /
			(double) (pl->viewport.max_x - pl->viewport.min_x) * (zoom - 1.);
		pl->axis[aN].scale *= zoom;
	}
	else if (pl->axis[aN].busy == AXIS_BUSY_Y) {

		pl->axis[aN].offset = pl->axis[aN].offset * zoom
			+ (double) (pl->viewport.max_y - origin) /
			(double) (pl->viewport.min_y - pl->viewport.max_y) * (zoom - 1.);
		pl->axis[aN].scale *= zoom;
	}

	pl->axis[aN].lock_scale = 0;
}

void plotAxisScaleMove(plot_t *pl, int aN, int move)
{
	if (aN < 0 || aN >= PLOT_AXES_MAX) {

		ERROR("Axis number is out of range\n");
		return ;
	}

	if (pl->axis[aN].slave != 0)
		return ;

	if (pl->axis[aN].busy == AXIS_BUSY_X) {

		pl->axis[aN].offset += (double) (move)
			/ (double) (pl->viewport.max_x - pl->viewport.min_x);
	}
	else if (pl->axis[aN].busy == AXIS_BUSY_Y) {

		pl->axis[aN].offset += (double) (move)
			/ (double) (pl->viewport.min_y - pl->viewport.max_y);
	}

	pl->axis[aN].lock_scale = 0;
}

void plotAxisScaleEqual(plot_t *pl)
{
	double		zoom, aspect_x, aspect_y;

	if (pl->on_X < 0 || pl->on_X >= PLOT_AXES_MAX)
		return ;

	if (pl->on_Y < 0 || pl->on_Y >= PLOT_AXES_MAX)
		return ;

	aspect_x = (double) (pl->viewport.max_x - pl->viewport.min_x);
	aspect_y = (double) (pl->viewport.max_y - pl->viewport.min_y);

	if (pl->axis[pl->on_Y].scale < pl->axis[pl->on_X].scale) {

		zoom = pl->axis[pl->on_Y].scale / pl->axis[pl->on_X].scale;
		zoom *= aspect_y / aspect_x;

		pl->axis[pl->on_X].offset *= zoom;
		pl->axis[pl->on_X].offset += (1. - zoom) / 2.;
		pl->axis[pl->on_X].scale *= zoom;
	}
	else {
		zoom = pl->axis[pl->on_X].scale / pl->axis[pl->on_Y].scale;
		zoom *= aspect_x / aspect_y;

		pl->axis[pl->on_Y].offset *= zoom;
		pl->axis[pl->on_Y].offset += (1. - zoom) / 2.;
		pl->axis[pl->on_Y].scale *= zoom;
	}

	pl->axis[pl->on_X].lock_scale = 0;
	pl->axis[pl->on_Y].lock_scale = 0;
}

static void
plotAxisScaleGridInner(plot_t *pl, int aN, int bN)
{
	if (pl->axis[aN].slave != 0)
		return ;

	if (aN != bN) {

		pl->axis[aN].offset += pl->axis[bN]._tis - pl->axis[aN]._tis;
		pl->axis[aN].scale *= pl->axis[bN]._tih / pl->axis[aN]._tih;

		pl->axis[aN].lock_scale = 0;
	}
}

void plotAxisScaleGridAlign(plot_t *pl)
{
	int		aN;

	if (pl->on_X < 0 || pl->on_Y < 0)
		return ;

	for (aN = 0; aN < PLOT_AXES_MAX; ++aN) {

		if (pl->axis[aN].busy == AXIS_BUSY_X) {

			plotAxisScaleGridInner(pl, aN, pl->on_X);
		}
		else if (pl->axis[aN].busy == AXIS_BUSY_Y) {

			plotAxisScaleGridInner(pl, aN, pl->on_Y);
		}
	}

	pl->axis[pl->on_X].lock_scale = 0;
	pl->axis[pl->on_Y].lock_scale = 0;
}

static int
plotAxisStakedSort(plot_t *pl, int *LIST)
{
	int		aN, fN, aSel, N = 0;

	for (aN = 0; aN < PLOT_AXES_MAX; ++aN) {

		if (		pl->axis[aN].busy == AXIS_BUSY_Y
				&& pl->axis[aN].slave == 0) {

			aSel = 0;

			for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

				if (pl->figure[fN].busy != 0 && pl->figure[fN].hidden == 0) {

					if (pl->figure[fN].axis_Y == aN) {

						aSel = 1;
						break;
					}
				}
			}

			if (aSel != 0) {

				LIST[N++] = aN;
			}
		}
	}

	return N;
}

void plotAxisScaleStaked(plot_t *pl)
{
	int		LIST[PLOT_AXES_MAX];
	double		shift, zoom, step;
	int		aN, iN, N;

	N = plotAxisStakedSort(pl, LIST);

	if (N > 1) {

		shift = (double) pl->layout_mark / (double)
			(pl->viewport.max_y - pl->viewport.min_y);

		step = 1. / (double) N;
		zoom = step - 2. * shift;

		shift = (double) (N - 1) / (double) N + shift;

		for (iN = 0; iN < N; ++iN) {

			aN = LIST[iN];

			plotAxisScaleAutoCond(pl, aN, pl->on_X);

			pl->axis[aN].offset = pl->axis[aN].offset * zoom + shift;
			pl->axis[aN].scale *= zoom;

			pl->axis[aN].lock_scale = 0;

			shift += - step;
		}
	}
}

int plotAxisGetByClick(plot_t *pl, int cur_X, int cur_Y)
{
	int		aN, rN = -1;
	int		boxSZ;

	cur_X = pl->viewport.min_x - pl->layout_border - cur_X;
	cur_Y = cur_Y - pl->viewport.max_y - pl->layout_border;

	for (aN = 0; aN < PLOT_AXES_MAX; ++aN) {

		if (pl->axis[aN].busy == AXIS_BUSY_X) {

			boxSZ = pl->layout_axis_box;
			boxSZ += (pl->axis[aN].compact == 0) ? pl->layout_label_box : 0;

			if (cur_Y < pl->axis[aN]._pos + boxSZ
				&& cur_Y > pl->axis[aN]._pos) {

					rN = aN;
					break;
			}
		}

		if (pl->axis[aN].busy == AXIS_BUSY_Y) {

			boxSZ = pl->layout_axis_box;
			boxSZ += (pl->axis[aN].compact == 0) ? pl->layout_label_box : 0;

			if (cur_X < pl->axis[aN]._pos + boxSZ
				&& cur_X > pl->axis[aN]._pos) {

					rN = aN;
					break;
			}
		}
	}

	pl->hover_axis = rN;

	return rN;
}

double plotAxisConv(plot_t *pl, int aN, double fval)
{
	double		scale, offset, temp;
	int		bN;

	scale = pl->axis[aN].scale;
	offset = pl->axis[aN].offset;

	if (pl->axis[aN].slave != 0) {

		bN = pl->axis[aN].slave_N;
		scale *= pl->axis[bN].scale;
		offset = offset * pl->axis[bN].scale + pl->axis[bN].offset;
	}

	if (pl->axis[aN].busy == AXIS_BUSY_X) {

		temp = (double) (pl->viewport.max_x - pl->viewport.min_x);
		scale *= temp;
		offset = offset * temp + pl->viewport.min_x;
	}
	else if (pl->axis[aN].busy == AXIS_BUSY_Y) {

		temp = (double) (pl->viewport.min_y - pl->viewport.max_y);
		scale *= temp;
		offset = offset * temp + pl->viewport.max_y;
	}

	return (fval * scale + offset);
}

double plotAxisConvInv(plot_t *pl, int aN, double px)
{
	double		scale, offset, temp;
	int		bN;

	scale = pl->axis[aN].scale;
	offset = pl->axis[aN].offset;

	if (pl->axis[aN].slave != 0) {

		bN = pl->axis[aN].slave_N;
		scale *= pl->axis[bN].scale;
		offset = offset * pl->axis[bN].scale + pl->axis[bN].offset;
	}

	if (pl->axis[aN].busy == AXIS_BUSY_X) {

		temp = (double) (pl->viewport.max_x - pl->viewport.min_x);
		scale *= temp;
		offset = offset * temp + pl->viewport.min_x;
	}
	else if (pl->axis[aN].busy == AXIS_BUSY_Y) {

		temp = (double) (pl->viewport.min_y - pl->viewport.max_y);
		scale *= temp;
		offset = offset * temp + pl->viewport.max_y;
	}

	return (px - offset) / scale;
}

void plotAxisSlave(plot_t *pl, int aN, int bN, double scale, double offset, int action)
{
	int		N, base = 0;

	if (aN < 0 || aN >= PLOT_AXES_MAX) {

		ERROR("Slave axis number is out of range\n");
		return ;
	}

	if (action == AXIS_SLAVE_DISABLE) {

		bN = pl->axis[aN].slave_N;
	}

	if (bN < 0 || bN >= PLOT_AXES_MAX) {

		ERROR("Base axis number is out of range\n");
		return ;
	}

	if (bN == aN) {

		ERROR("Axes must not be the same\n");
		return ;
	}

	if (pl->axis[bN].slave != 0) {

		ERROR("Base axis must not be slave\n");
		return ;
	}

	for (N = 0; N < PLOT_AXES_MAX; ++N) {

		if (pl->axis[N].busy != AXIS_FREE
				&& pl->axis[N].slave != 0) {

			if (pl->axis[N].slave_N == aN) {

				base = 1;
				break;
			}
		}
	}

	if (base) {

		ERROR("The axis is base for another slave\n");
		return ;
	}

	if (action == AXIS_SLAVE_ENABLE) {

		if (pl->axis[aN].slave == 0) {

			pl->axis[aN].slave = 1;
			pl->axis[aN].slave_N = bN;
			pl->axis[aN].scale = scale;
			pl->axis[aN].offset = offset;

			pl->on_X = (aN == pl->on_X) ? bN : pl->on_X;
			pl->on_Y = (aN == pl->on_Y) ? bN : pl->on_Y;
		}
	}
	else if (action == AXIS_SLAVE_HOLD_AS_IS) {

		if (bN < 0 || bN >= PLOT_AXES_MAX) {

			ERROR("Base axis number is out of range\n");
			return ;
		}

		if (pl->axis[aN].slave == 0) {

			pl->axis[aN].slave = 1;
			pl->axis[aN].slave_N = bN;

			pl->axis[aN].scale = pl->axis[aN].scale / pl->axis[bN].scale;
			pl->axis[aN].offset = (pl->axis[aN].offset - pl->axis[bN].offset)
				/ pl->axis[bN].scale;

			pl->on_X = (aN == pl->on_X) ? bN : pl->on_X;
			pl->on_Y = (aN == pl->on_Y) ? bN : pl->on_Y;
		}
	}
	else {
		if (pl->axis[aN].slave != 0) {

			pl->axis[aN].slave = 0;

			pl->axis[aN].scale = pl->axis[aN].scale * pl->axis[bN].scale;
			pl->axis[aN].offset = pl->axis[aN].offset * pl->axis[bN].scale
				+ pl->axis[bN].offset;
		}
	}
}

void plotAxisRemove(plot_t *pl, int aN)
{
	int		N, cN;

	if (aN < 0 || aN >= PLOT_AXES_MAX) {

		ERROR("Axis number is out of range\n");
		return ;
	}

	if (aN == pl->on_X || aN == pl->on_Y) {

		ERROR("Unable to remove active axis\n");
		return ;
	}

	for (N = 0; N < PLOT_FIGURE_MAX; ++N) {

		if (pl->figure[N].busy != 0) {

			if (pl->figure[N].axis_X == aN) {

				if (pl->axis[aN].slave != 0) {

					cN = plotGetSubtractScale(pl, pl->figure[N].data_N,
							pl->figure[N].column_X,
							pl->axis[aN].scale,
							pl->axis[aN].offset);

					if (cN != -1) {

						pl->figure[N].column_X = cN;
					}

					pl->figure[N].axis_X = pl->axis[aN].slave_N;
				}
				else {
					pl->figure[N].axis_X = pl->on_X;
				}
			}

			if (pl->figure[N].axis_Y == aN) {

				if (pl->axis[aN].slave != 0) {

					cN = plotGetSubtractScale(pl, pl->figure[N].data_N,
							pl->figure[N].column_Y,
							pl->axis[aN].scale,
							pl->axis[aN].offset);

					if (cN != -1) {

						pl->figure[N].column_Y = cN;
					}

					pl->figure[N].axis_Y = pl->axis[aN].slave_N;
				}
				else {
					pl->figure[N].axis_Y = pl->on_Y;
				}
			}
		}
	}

	for (N = 0; N < PLOT_AXES_MAX; ++N) {

		if (pl->axis[N].busy != AXIS_FREE
				&& pl->axis[N].slave != 0) {

			if (pl->axis[N].slave_N == aN) {

				plotAxisSlave(pl, N, -1, 0., 0., AXIS_SLAVE_DISABLE);
			}
		}
	}

	pl->axis[aN].busy = AXIS_FREE;
	pl->axis[aN].slave = 0;
	pl->axis[aN].label[0] = 0;
	pl->axis[aN].expen = 0;
	pl->axis[aN].compact = 0;
}

void plotFigureAdd(plot_t *pl, int fN, int dN, int nX, int nY, int aX, int aY, const char *label)
{
	int		gN;

	if (fN < 0 || fN >= PLOT_FIGURE_MAX) {

		ERROR("Figure number is out of range\n");
		return ;
	}

	if (dN < 0 || dN >= PLOT_DATASET_MAX) {

		ERROR("Dataset number is out of range\n");
		return ;
	}

	if (pl->data[dN].column_N < 1) {

		ERROR("Dataset %i has no DATA\n", dN);
		return ;
	}

	if (nX < -1 || nX >= pl->data[dN].column_N + PLOT_SUBTRACT) {

		ERROR("X column number %i is out of range\n", nX);
		return ;
	}

	if (nY < -1 || nY >= pl->data[dN].column_N + PLOT_SUBTRACT) {

		ERROR("Y column number %i is out of range\n", nY);
		return ;
	}

	if (aX < 0 || aX >= PLOT_AXES_MAX) {

		ERROR("X axis number %i is out of range\n", aX);
		return ;
	}

	if (aY < 0 || aY >= PLOT_AXES_MAX) {

		ERROR("Y axis number %i is out of range\n", aY);
		return ;
	}

	if (aX == aY || pl->axis[aX].busy == AXIS_BUSY_Y
		|| pl->axis[aY].busy == AXIS_BUSY_X) {

		ERROR("Invalid axes mapping %i %i\n", aX, aY);
		return ;
	}

	pl->draw[fN].sketch = SKETCH_FINISHED;

	pl->figure[fN].busy = 1;
	pl->figure[fN].hidden = 0;
	pl->figure[fN].drawing = pl->default_drawing;
	pl->figure[fN].width = pl->default_width;
	pl->figure[fN].data_N = dN;
	pl->figure[fN].column_X = nX;
	pl->figure[fN].column_Y = nY;
	pl->figure[fN].axis_X = aX;
	pl->figure[fN].axis_Y = aY;

	if (pl->axis[aX].busy == AXIS_FREE) {

		pl->axis[aX].busy = AXIS_BUSY_X;
		pl->axis[aX].lock_scale = 1;
	}

	if (pl->axis[aY].busy == AXIS_FREE) {

		pl->axis[aY].busy = AXIS_BUSY_Y;
		pl->axis[aY].lock_scale = 1;
	}

	gN = pl->data[dN].map[nX];

	if (gN != -1) {

		plotAxisLabel(pl, aX, pl->group[gN].label);
	}

	gN = pl->data[dN].map[nY];

	if (gN != -1) {

		plotAxisLabel(pl, aY, pl->group[gN].label);
	}

	strcpy(pl->figure[fN].label, label);

	pl->on_X = (pl->on_X < 0) ? aX : pl->on_X;
	pl->on_Y = (pl->on_Y < 0) ? aY : pl->on_Y;
}

static void
plotDataBoxTextFmt(plot_t *pl, int fN, double val)
{
	char		tfmt[PLOT_STRING_MAX];
	char		tbuf[PLOT_STRING_MAX];

	int		fexp = 1;

	if (val != 0.) {

		fexp += (int) floor(log10(fabs(val)));
	}

	if (fexp >= -2 && fexp < pl->fprecision) {

		fexp = (fexp < 1) ? 1 : fexp;

		sprintf(tfmt, "%% .%df ", pl->fprecision - fexp);
	}
	else {
		sprintf(tfmt, "%% .%dE ", pl->fprecision - 1);
	}

	sprintf(tbuf, tfmt, val);
	strcat(pl->data_box_text[fN], tbuf);
}

static int
plotCheckColumnLinked(plot_t *pl, int dN, int cN)
{
	int		sN, fN, linked = 0;

	for (sN = 0; sN < PLOT_SUBTRACT; ++sN) {

		if (pl->data[dN].sub[sN].busy == SUBTRACT_SCALE) {

			if (cN == pl->data[dN].sub[sN].op.scale.column_1) {

				linked = 1;
				break;
			}
		}
		else if (pl->data[dN].sub[sN].busy == SUBTRACT_BINARY_SUBTRACTION
				|| pl->data[dN].sub[sN].busy == SUBTRACT_BINARY_ADDITION
				|| pl->data[dN].sub[sN].busy == SUBTRACT_BINARY_MULTIPLICATION
				|| pl->data[dN].sub[sN].busy == SUBTRACT_BINARY_HYPOTENUSE) {

			if (cN == pl->data[dN].sub[sN].op.binary.column_1
					|| cN == pl->data[dN].sub[sN].op.binary.column_2) {

				linked = 1;
				break;
			}
		}
		else if (pl->data[dN].sub[sN].busy == SUBTRACT_FILTER_DIFFERENCE
				|| pl->data[dN].sub[sN].busy == SUBTRACT_FILTER_CUMULATIVE
				|| pl->data[dN].sub[sN].busy == SUBTRACT_FILTER_BITMASK
				|| pl->data[dN].sub[sN].busy == SUBTRACT_FILTER_LOW_PASS) {

			if (cN == pl->data[dN].sub[sN].op.filter.column_1) {

				linked = 1;
				break;
			}
		}
		else if (pl->data[dN].sub[sN].busy == SUBTRACT_RESAMPLE) {

			if (cN == pl->data[dN].sub[sN].op.resample.column_X) {

				linked = 1;
				break;
			}
		}
	}

	for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		if (pl->figure[fN].busy != 0) {

			if (cN == pl->figure[fN].column_X || cN == pl->figure[fN].column_Y) {

				linked = 1;
				break;
			}
		}
	}

	return linked;
}

static void
plotSubtractGarbage(plot_t *pl, int dN)
{
	int		sN, cN, N;

	do {
		N = 0;

		for (sN = 0; sN < PLOT_SUBTRACT; ++sN) {

			if (pl->data[dN].sub[sN].busy != SUBTRACT_FREE) {

				cN = sN + pl->data[dN].column_N;

				if (plotCheckColumnLinked(pl, dN, cN) == 0) {

					pl->data[dN].sub[sN].busy = SUBTRACT_FREE;

					N++;
				}
			}
		}
	}
	while (N != 0);
}

void plotFigureRemove(plot_t *pl, int fN)
{
	int		N, aN, rX = 1, rY = 1;

	if (fN < 0 || fN >= PLOT_FIGURE_MAX) {

		ERROR("Figure number is out of range\n");
		return ;
	}

	for (N = 0; N < PLOT_FIGURE_MAX; ++N) {

		if (pl->figure[N].busy != 0 && N != fN) {

			if (pl->figure[N].axis_X == pl->figure[fN].axis_X)
				rX = 0;

			if (pl->figure[N].axis_Y == pl->figure[fN].axis_Y)
				rY = 0;
		}
	}

	pl->figure[fN].busy = 0;

	if (rX != 0) {

		aN = pl->figure[fN].axis_X;

		if (pl->on_X == aN) {

			for (N = 0; N < PLOT_AXES_MAX; ++N) {

				if (N != aN && pl->axis[N].busy == AXIS_BUSY_X
						&& pl->axis[N].slave == 0) {

					pl->on_X = N;
					break;
				}
			}
		}

		if (pl->on_X != aN) {

			plotAxisRemove(pl, aN);
		}
	}

	if (rY != 0) {

		aN = pl->figure[fN].axis_Y;

		if (pl->on_Y == aN) {

			for (N = 0; N < PLOT_AXES_MAX; ++N) {

				if (N != aN && pl->axis[N].busy == AXIS_BUSY_Y
						&& pl->axis[N].slave == 0) {

					pl->on_Y = N;
					break;
				}
			}
		}

		if (pl->on_Y != aN) {

			plotAxisRemove(pl, aN);
		}
	}

	plotSubtractGarbage(pl, pl->figure[fN].data_N);
}

void plotFigureGarbage(plot_t *pl, int dN)
{
	int		fN;

	for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		if (		pl->figure[fN].busy != 0
				&& pl->figure[fN].data_N == dN) {

			plotFigureRemove(pl, fN);
		}
	}
}

void plotFigureMoveAxes(plot_t *pl, int fN)
{
	int		N, aN, rX = 1, rY = 1;

	if (fN < 0 || fN >= PLOT_FIGURE_MAX) {

		ERROR("Figure number is out of range\n");
		return ;
	}

	if (pl->on_X < 0 || pl->on_X >= PLOT_AXES_MAX)
		return ;

	if (pl->on_Y < 0 || pl->on_Y >= PLOT_AXES_MAX)
		return ;

	for (N = 0; N < PLOT_FIGURE_MAX; ++N) {

		if (pl->figure[N].busy != 0 && N != fN) {

			if (pl->figure[N].axis_X == pl->figure[fN].axis_X)
				rX = 0;

			if (pl->figure[N].axis_Y == pl->figure[fN].axis_Y)
				rY = 0;
		}
	}

	if (pl->figure[fN].axis_X != pl->on_X) {

		aN = pl->figure[fN].axis_X;
		pl->figure[fN].axis_X = pl->on_X;

		if (rX != 0) {

			plotAxisRemove(pl, aN);
		}
	}

	if (pl->figure[fN].axis_Y != pl->on_Y) {

		aN = pl->figure[fN].axis_Y;
		pl->figure[fN].axis_Y = pl->on_Y;

		if (rY != 0) {

			plotAxisRemove(pl, aN);
		}
	}
}

static int
plotGetFreeAxis(plot_t *pl)
{
	int		N, aN = -1;

	for (N = 0; N < PLOT_AXES_MAX; ++N) {

		if (pl->axis[N].busy == AXIS_FREE) {

			aN = N;
			break;
		}
	}

	return aN;
}

void plotFigureMakeIndividualAxes(plot_t *pl, int fN)
{
	int		N, aN, rX = 1, rY = 1;

	if (fN < 0 || fN >= PLOT_FIGURE_MAX) {

		ERROR("Figure number is out of range\n");
		return ;
	}

	for (N = 0; N < PLOT_FIGURE_MAX; ++N) {

		if (pl->figure[N].busy != 0 && N != fN) {

			if (pl->figure[N].axis_X == pl->figure[fN].axis_X)
				rX = 0;

			if (pl->figure[N].axis_Y == pl->figure[fN].axis_Y)
				rY = 0;
		}
	}

	if (rX == 0) {

		aN = plotGetFreeAxis(pl);

		if (aN != -1) {

			N = pl->figure[fN].axis_X;

			pl->axis[aN].busy = AXIS_BUSY_X;
			pl->figure[fN].axis_X = aN;

			plotAxisScaleAuto(pl, aN);
			plotAxisLabel(pl, aN, pl->axis[N].label);
		}
		else {
			ERROR("Unable to get free axis on X\n");
			return ;
		}
	}

	if (rY == 0) {

		aN = plotGetFreeAxis(pl);

		if (aN != -1) {

			N = pl->figure[fN].axis_Y;

			pl->axis[aN].busy = AXIS_BUSY_Y;
			pl->figure[fN].axis_Y = aN;

			plotAxisScaleAuto(pl, aN);
			plotAxisLabel(pl, aN, pl->axis[N].label);
		}
		else {
			ERROR("Unable to get free axis on Y\n");
			return ;
		}
	}
}

void plotFigureExchange(plot_t *pl, int fN, int fN_1)
{
	char		backup[sizeof(pl->figure[0])];

	if (fN < 0 || fN >= PLOT_FIGURE_MAX) {

		ERROR("Figure number is out of range\n");
		return ;
	}

	if (fN_1 < 0 || fN_1 >= PLOT_FIGURE_MAX) {

		ERROR("Figure number (exchange) is out of range\n");
		return ;
	}

	memcpy(backup, &pl->figure[fN_1], sizeof(pl->figure[0]));
	memcpy(&pl->figure[fN_1], &pl->figure[fN], sizeof(pl->figure[0]));
	memcpy(&pl->figure[fN], backup, sizeof(pl->figure[0]));
}

static int
plotGetSubtractTimeUnwrapByMatch(plot_t *pl, int dN, int cN)
{
	int		sN, rN = -1;

	for (sN = 0; sN < PLOT_SUBTRACT; ++sN) {

		if (pl->data[dN].sub[sN].busy == SUBTRACT_TIME_UNWRAP
				&& pl->data[dN].sub[sN].op.time.column_1 == cN) {

			rN = sN;
			break;
		}
	}

	return rN;
}

static int
plotGetSubtractScaleByMatch(plot_t *pl, int dN, int cN, double scale, double offset)
{
	int		sN, rN = -1;

	for (sN = 0; sN < PLOT_SUBTRACT; ++sN) {

		if (pl->data[dN].sub[sN].busy == SUBTRACT_SCALE
				&& pl->data[dN].sub[sN].op.scale.column_1 == cN
				&& pl->data[dN].sub[sN].op.scale.scale == scale
				&& pl->data[dN].sub[sN].op.scale.offset == offset) {

			rN = sN;
			break;
		}
	}

	return rN;
}

static int
plotGetFreeSubtract(plot_t *pl, int dN)
{
	int		sN, rN = -1;

	for (sN = 0; sN < PLOT_SUBTRACT; ++sN) {

		if (pl->data[dN].sub[sN].busy == 0) {

			rN = sN;
			break;
		}
	}

	return rN;
}

int plotGetSubtractTimeUnwrap(plot_t *pl, int dN, int cN)
{
	int		sN;

	if (dN < 0 || dN >= PLOT_DATASET_MAX) {

		ERROR("Dataset number is out of range\n");
		return -1;
	}

	sN = plotGetSubtractTimeUnwrapByMatch(pl, dN, cN);

	if (sN == -1) {

		sN = plotGetFreeSubtract(pl, dN);

		if (sN == -1) {

			ERROR("Unable to get free subtract\n");
			return -1;
		}

		pl->data[dN].sub[sN].busy = SUBTRACT_TIME_UNWRAP;
		pl->data[dN].sub[sN].op.time.column_1 = cN;

		plotDataSubtract(pl, dN, sN);
	}

	cN = sN + pl->data[dN].column_N;

	return cN;
}

int plotGetSubtractScale(plot_t *pl, int dN, int cN, double scale, double offset)
{
	int		sN;

	if (dN < 0 || dN >= PLOT_DATASET_MAX) {

		ERROR("Dataset number is out of range\n");
		return -1;
	}

	sN = plotGetSubtractScaleByMatch(pl, dN, cN, scale, offset);

	if (sN == -1) {

		sN = plotGetFreeSubtract(pl, dN);

		if (sN == -1) {

			ERROR("Unable to get free subtract\n");
			return -1;
		}

		pl->data[dN].sub[sN].busy = SUBTRACT_SCALE;
		pl->data[dN].sub[sN].op.scale.column_1 = cN;
		pl->data[dN].sub[sN].op.scale.scale = scale;
		pl->data[dN].sub[sN].op.scale.offset = offset;

		plotDataSubtract(pl, dN, sN);
	}

	cN = sN + pl->data[dN].column_N;

	return cN;
}

int plotGetSubtractResample(plot_t *pl, int dN, int cN_X, int in_dN, int in_cN_X, int in_cN_Y)
{
	int		sN, cN;

	if (dN < 0 || dN >= PLOT_DATASET_MAX) {

		ERROR("Dataset number is out of range\n");
		return -1;
	}

	sN = plotGetFreeSubtract(pl, dN);

	if (sN == -1) {

		ERROR("Unable to get free subtract\n");
		return -1;
	}

	pl->data[dN].sub[sN].busy = SUBTRACT_RESAMPLE;
	pl->data[dN].sub[sN].op.resample.column_X = cN_X;
	pl->data[dN].sub[sN].op.resample.column_in_X = in_cN_X;
	pl->data[dN].sub[sN].op.resample.column_in_Y = in_cN_Y;
	pl->data[dN].sub[sN].op.resample.in_data_N = in_dN;

	plotDataSubtract(pl, dN, sN);

	cN = sN + pl->data[dN].column_N;

	return cN;
}

int plotGetSubtractBinary(plot_t *pl, int dN, int opSUB, int cN_1, int cN_2)
{
	int		sN, cN;

	if (dN < 0 || dN >= PLOT_DATASET_MAX) {

		ERROR("Dataset number is out of range\n");
		return -1;
	}

	if (cN_1 < -1 || cN_1 >= pl->data[dN].column_N + PLOT_SUBTRACT) {

		ERROR("Column number %i is out of range\n", cN_1);
		return -1;
	}

	if (cN_2 < -1 || cN_2 >= pl->data[dN].column_N + PLOT_SUBTRACT) {

		ERROR("Column number %i is out of range\n", cN_2);
		return -1;
	}

	sN = plotGetFreeSubtract(pl, dN);

	if (sN == -1) {

		ERROR("Unable to get free subtract\n");
		return -1;
	}

	pl->data[dN].sub[sN].busy = opSUB;
	pl->data[dN].sub[sN].op.binary.column_1 = cN_1;
	pl->data[dN].sub[sN].op.binary.column_2 = cN_2;

	plotDataSubtract(pl, dN, sN);

	cN = sN + pl->data[dN].column_N;

	return cN;
}

int plotGetFreeFigure(plot_t *pl)
{
	int		N, fN = -1;

	for (N = 0; N < PLOT_FIGURE_MAX; ++N) {

		if (pl->figure[N].busy == 0) {

			fN = N;
			break;
		}
	}

	return fN;
}

void plotFigureSubtractTimeUnwrap(plot_t *pl, int fN_1)
{
	int		dN, cN;

	if (fN_1 < 0 || fN_1 >= PLOT_FIGURE_MAX) {

		ERROR("Figure number is out of range\n");
		return ;
	}

	dN = pl->figure[fN_1].data_N;
	cN = plotGetSubtractTimeUnwrap(pl, dN, pl->figure[fN_1].column_X);

	if (cN != -1) {

		pl->figure[fN_1].column_X = cN;
	}
}

void plotFigureSubtractScale(plot_t *pl, int fN_1, int aBUSY, double scale, double offset)
{
	int		dN, cN;

	if (fN_1 < 0 || fN_1 >= PLOT_FIGURE_MAX) {

		ERROR("Figure number is out of range\n");
		return ;
	}

	dN = pl->figure[fN_1].data_N;

	if (aBUSY == AXIS_BUSY_X) {

		cN = plotGetSubtractScale(pl, dN, pl->figure[fN_1].column_X, scale, offset);

		if (cN != -1) {

			pl->figure[fN_1].column_X = cN;
		}
	}
	else if (aBUSY == AXIS_BUSY_Y) {

		cN = plotGetSubtractScale(pl, dN, pl->figure[fN_1].column_Y, scale, offset);

		if (cN != -1) {

			pl->figure[fN_1].column_Y = cN;
		}
	}
}

static int
plotFigureSubtractAdd(plot_t *pl, int fN, int fN_1, int fN_2, int opSUB)
{
	int		dN, aN_X, aN_Y, cN_X, cN_Y;

	dN = pl->figure[fN_1].data_N;

	cN_X = pl->figure[fN_1].column_X;
	aN_X = pl->figure[fN_1].axis_X;

	if (aN_X != pl->figure[fN_2].axis_X) {

		ERROR("Both figures must be on the same axis on X\n");
		return 0;
	}

	if (dN != pl->figure[fN_2].data_N || cN_X != pl->figure[fN_2].column_X) {

		cN_Y = plotGetSubtractResample(pl, dN, cN_X,
				pl->figure[fN_2].data_N,
				pl->figure[fN_2].column_X,
				pl->figure[fN_2].column_Y);

		if (cN_Y == -1) {

			ERROR("Unable to get resample subtract\n");
			return 0;
		}
	}
	else {
		cN_Y = pl->figure[fN_2].column_Y;
	}

	cN_Y = plotGetSubtractBinary(pl, dN, opSUB, pl->figure[fN_1].column_Y, cN_Y);

	if (cN_Y == -1) {

		return 0;
	}

	aN_Y = plotGetFreeAxis(pl);

	if (aN_Y != -1) {

		pl->axis[aN_Y].busy = AXIS_BUSY_Y;
		plotAxisLabel(pl, aN_Y, pl->axis[pl->figure[fN_1].axis_Y].label);
	}
	else {
		aN_Y = pl->figure[fN_1].axis_Y;
	}

	plotFigureAdd(pl, fN, dN, cN_X, cN_Y, aN_X, aN_Y, "");

	if (opSUB == SUBTRACT_BINARY_SUBTRACTION) {

		sprintf(pl->figure[fN].label, "R: (%.35s) - (%.35s)",
				pl->figure[fN_1].label,
				pl->figure[fN_2].label);
	}
	else if (opSUB == SUBTRACT_BINARY_ADDITION) {

		sprintf(pl->figure[fN].label, "A: (%.35s) + (%.35s)",
				pl->figure[fN_1].label,
				pl->figure[fN_2].label);
	}
	else if (opSUB == SUBTRACT_BINARY_MULTIPLICATION) {

		sprintf(pl->figure[fN].label, "M: (%.35s) * (%.35s)",
				pl->figure[fN_1].label,
				pl->figure[fN_2].label);
	}
	else if (opSUB == SUBTRACT_BINARY_HYPOTENUSE) {

		sprintf(pl->figure[fN].label, "H: (%.35s) (%.35s)",
				pl->figure[fN_1].label,
				pl->figure[fN_2].label);
	}

	pl->figure[fN].drawing = pl->figure[fN_1].drawing;
	pl->figure[fN].width = pl->figure[fN_1].width;

	return AXIS_BUSY_Y;
}

void plotFigureSubtractFilter(plot_t *pl, int fN_1, int opSUB, double arg_1, double arg_2)
{
	int		fN, dN, sN, cN, aN;

	if (fN_1 < 0 || fN_1 >= PLOT_FIGURE_MAX) {

		ERROR("Figure number is out of range\n");
		return ;
	}

	fN = plotGetFreeFigure(pl);

	if (fN == -1) {

		ERROR("Unable to get free figure to subtract\n");
		return ;
	}

	dN = pl->figure[fN_1].data_N;
	sN = plotGetFreeSubtract(pl, dN);

	if (sN == -1) {

		ERROR("Unable to get free subtract\n");
		return ;
	}

	pl->data[dN].sub[sN].busy = opSUB;
	pl->data[dN].sub[sN].op.filter.column_1 = pl->figure[fN_1].column_Y;
	pl->data[dN].sub[sN].op.filter.arg_1 = arg_1;
	pl->data[dN].sub[sN].op.filter.arg_2 = arg_2;

	plotDataSubtract(pl, dN, sN);

	cN = sN + pl->data[dN].column_N;

	if (opSUB == SUBTRACT_FILTER_LOW_PASS) {

		aN = pl->figure[fN_1].axis_Y;
	}
	else {
		aN = plotGetFreeAxis(pl);

		if (aN != -1) {

			pl->axis[aN].busy = AXIS_BUSY_Y;
			plotAxisLabel(pl, aN, pl->axis[pl->figure[fN_1].axis_Y].label);
		}
		else {
			aN = pl->figure[fN_1].axis_Y;
		}
	}

	plotFigureAdd(pl, fN, dN, pl->figure[fN_1].column_X, cN,
			pl->figure[fN_1].axis_X, aN, "");

	if (opSUB == SUBTRACT_FILTER_DIFFERENCE) {

		sprintf(pl->figure[fN].label, "D: %.75s", pl->figure[fN_1].label);
	}
	else if (opSUB == SUBTRACT_FILTER_CUMULATIVE) {

		sprintf(pl->figure[fN].label, "C: %.75s", pl->figure[fN_1].label);
	}
	else if (opSUB == SUBTRACT_FILTER_BITMASK) {

		if (arg_1 == arg_2) {

			sprintf(pl->figure[fN].label, "B(%d): %.75s",
					(int) arg_1, pl->figure[fN_1].label);
		}
		else {
			sprintf(pl->figure[fN].label, "B(%d-%d): %.75s",
					(int) arg_1, (int) arg_2, pl->figure[fN_1].label);
		}
	}
	else if (opSUB == SUBTRACT_FILTER_LOW_PASS) {

		sprintf(pl->figure[fN].label, "L(%.2E): %.75s", arg_1, pl->figure[fN_1].label);
	}

	pl->figure[fN].drawing = pl->figure[fN_1].drawing;
	pl->figure[fN].width = pl->figure[fN_1].width;

	if (opSUB == SUBTRACT_FILTER_LOW_PASS) {

		/* Do nothing */
	}
	else {
		plotAxisScaleAutoCond(pl, pl->figure[fN].axis_Y, pl->figure[fN].axis_X);

		pl->on_X = pl->figure[fN].axis_X;
		pl->on_Y = pl->figure[fN].axis_Y;

		if (pl->axis[pl->on_X].slave != 0) {

			pl->on_X = pl->axis[pl->on_X].slave_N;
		}

		if (pl->axis[pl->on_Y].slave != 0) {

			pl->on_Y = pl->axis[pl->on_Y].slave_N;
		}
	}
}

static void
plotFigureSubtractBinaryLinked(plot_t *pl, int fN, int opSUB, int fNP[2])
{
	int		dN, sN, sE, cN, fN_1 = -1, fN_2 = -1;

	dN = pl->figure[fN].data_N;
	sN = pl->figure[fN].column_Y - pl->data[dN].column_N;

	if (sN >= 0 && sN < PLOT_SUBTRACT && pl->data[dN].sub[sN].busy == opSUB) {

		cN = pl->data[dN].sub[sN].op.binary.column_1;
		sE = cN - pl->data[dN].column_N;

		if (		sE >= 0 && sE < PLOT_SUBTRACT
				&& pl->data[dN].sub[sE].busy == SUBTRACT_RESAMPLE) {

			cN = pl->data[dN].sub[sE].op.resample.column_in_Y;
		}

		for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

			if (pl->figure[fN].busy != 0) {

				if (		dN == pl->figure[fN].data_N
						&& cN == pl->figure[fN].column_Y) {

					fN_1 = fN;
					break;
				}
			}
		}

		cN = pl->data[dN].sub[sN].op.binary.column_2;
		sE = cN - pl->data[dN].column_N;

		if (		sE >= 0 && sE < PLOT_SUBTRACT
				&& pl->data[dN].sub[sE].busy == SUBTRACT_RESAMPLE) {

			cN = pl->data[dN].sub[sE].op.resample.column_in_Y;
			dN = pl->data[dN].sub[sE].op.resample.in_data_N;
		}

		for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

			if (pl->figure[fN].busy != 0) {

				if (		dN == pl->figure[fN].data_N
						&& cN == pl->figure[fN].column_Y) {

					fN_2 = fN;
					break;
				}
			}
		}
	}

	fNP[0] = fN_1;
	fNP[1] = fN_2;
}

void plotFigureSubtractSwitch(plot_t *pl, int opSUB)
{
	int		fN, fN_1, fN_2, fNQ[2], rBUSY, N = 0;

	for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		if (pl->figure[fN].busy != 0 && pl->figure[fN].hidden == 0) {

			if (N < 2) {

				fNQ[N] = fN;
			}

			N++;
		}
	}

	if (N == 1) {

		fN = fNQ[0];

		plotFigureSubtractBinaryLinked(pl, fN, opSUB, fNQ);

		fN_1 = fNQ[0];
		fN_2 = fNQ[1];

		if (fN_1 != -1 && fN_2 != -1) {

			pl->figure[fN].hidden = 1;
			pl->figure[fN_1].hidden = 0;
			pl->figure[fN_2].hidden = 0;

			pl->on_X = pl->figure[fN_1].axis_X;
			pl->on_Y = pl->figure[fN_1].axis_Y;
		}
	}
	else if (N == 2) {

		fN_1 = fNQ[0];
		fN_2 = fNQ[1];

		fN = -1;

		for (N = 0; N < PLOT_FIGURE_MAX; ++N) {

			if (pl->figure[N].busy != 0) {

				plotFigureSubtractBinaryLinked(pl, N, opSUB, fNQ);

				if (fNQ[0] == fN_1 && fNQ[1] == fN_2) {

					fN = N;
					break;
				}

				if (fNQ[0] == fN_2 && fNQ[1] == fN_1) {

					fN = N;
					break;
				}
			}
		}

		if (fN != -1) {

			pl->figure[fN_1].hidden = 1;
			pl->figure[fN_2].hidden = 1;
			pl->figure[fN].hidden = 0;

			if (pl->figure[fN].axis_X == pl->figure[fN_1].axis_X
					&& pl->figure[fN].axis_X == pl->figure[fN_2].axis_X) {

				plotAxisScaleAutoCond(pl, pl->figure[fN].axis_Y,
						pl->figure[fN].axis_X);
			}
			else if (pl->figure[fN].axis_Y == pl->figure[fN_1].axis_Y
					&& pl->figure[fN].axis_Y == pl->figure[fN_2].axis_Y) {

				plotAxisScaleAutoCond(pl, pl->figure[fN].axis_X,
						pl->figure[fN].axis_Y);
			}

			pl->on_X = pl->figure[fN].axis_X;
			pl->on_Y = pl->figure[fN].axis_Y;
		}
		else {
			fN = plotGetFreeFigure(pl);

			if (fN == -1) {

				ERROR("Unable to get free figure to subtract\n");
				return ;
			}

			rBUSY = plotFigureSubtractAdd(pl, fN, fN_1, fN_2, opSUB);

			if (rBUSY != 0) {

				pl->figure[fN_1].hidden = 1;
				pl->figure[fN_2].hidden = 1;

				if (rBUSY == AXIS_BUSY_X) {

					plotAxisScaleAutoCond(pl, pl->figure[fN].axis_X,
							pl->figure[fN].axis_Y);
				}
				else if (rBUSY == AXIS_BUSY_Y) {

					plotAxisScaleAutoCond(pl, pl->figure[fN].axis_Y,
							pl->figure[fN].axis_X);
				}
				else {
					plotAxisScaleAuto(pl, pl->figure[fN].axis_X);
					plotAxisScaleAuto(pl, pl->figure[fN].axis_Y);
				}

				pl->on_X = pl->figure[fN].axis_X;
				pl->on_Y = pl->figure[fN].axis_Y;
			}
		}
	}

	if (pl->axis[pl->on_X].slave != 0) {

		pl->on_X = pl->axis[pl->on_X].slave_N;
	}

	if (pl->axis[pl->on_Y].slave != 0) {

		pl->on_Y = pl->axis[pl->on_Y].slave_N;
	}
}

void plotFigureSubtractPolifit(plot_t *pl, int fN_1, int poly_N)
{
	int		N, fN, dN, sN, cN, aN, bN;
	double		scale_X, offset_X, scale_Y, offset_Y;

	if (fN_1 < 0 || fN_1 >= PLOT_FIGURE_MAX) {

		ERROR("Figure number is out of range\n");
		return ;
	}

	if (poly_N < 0 || poly_N > PLOT_POLYFIT_MAX) {

		ERROR("Polynomial degree is out of range\n");
		return ;
	}

	fN = plotGetFreeFigure(pl);

	if (fN == -1) {

		ERROR("Unable to get free figure to subtract\n");
		return ;
	}

	dN = pl->figure[fN_1].data_N;
	sN = plotGetFreeSubtract(pl, dN);

	if (sN == -1) {

		ERROR("Unable to get free subtract\n");
		return ;
	}

	aN = pl->figure[fN_1].axis_X;

	scale_X = pl->axis[aN].scale;
	offset_X = pl->axis[aN].offset;

	if (pl->axis[aN].slave != 0) {

		bN = pl->axis[aN].slave_N;
		scale_X *= pl->axis[bN].scale;
		offset_X = offset_X * pl->axis[bN].scale + pl->axis[bN].offset;
	}

	aN = pl->figure[fN_1].axis_Y;

	scale_Y = pl->axis[aN].scale;
	offset_Y = pl->axis[aN].offset;

	if (pl->axis[aN].slave != 0) {

		bN = pl->axis[aN].slave_N;
		scale_Y *= pl->axis[bN].scale;
		offset_Y = offset_X * pl->axis[bN].scale + pl->axis[bN].offset;
	}

	plotDataPolyfit(pl, dN, pl->figure[fN_1].column_X, pl->figure[fN_1].column_Y,
			scale_X, offset_X, scale_Y, offset_Y, poly_N);

	pl->data[dN].sub[sN].busy = SUBTRACT_POLYFIT;
	pl->data[dN].sub[sN].op.polyfit.column_X = pl->figure[fN_1].column_X;
	pl->data[dN].sub[sN].op.polyfit.column_Y = pl->figure[fN_1].column_Y;
	pl->data[dN].sub[sN].op.polyfit.poly_N = poly_N;

	for (N = 0; N < poly_N + 1; ++N) {

		pl->data[dN].sub[sN].op.polyfit.coefs[N] = pl->lsq.b[N];
	}

	plotDataSubtract(pl, dN, sN);

	cN = sN + pl->data[dN].column_N;
	aN = pl->figure[fN_1].axis_Y;

	plotFigureAdd(pl, fN, dN, pl->figure[fN_1].column_X, cN,
			pl->figure[fN_1].axis_X, aN, "");

	sprintf(pl->figure[fN].label, "P: %.75s", pl->figure[fN_1].label);

	pl->figure[fN].drawing = pl->figure[fN_1].drawing;
	pl->figure[fN].width = pl->figure[fN_1].width;

	for (N = 0; N < PLOT_DATA_BOX_MAX; ++N) {

		pl->data_box_text[N][0] = 0;

		if (N == 0 && poly_N == 0) {

			sprintf(pl->data_box_text[N], " [%i] = ", N);
			plotDataBoxTextFmt(pl, N, pl->lsq.b[N]);
		}
		else if (N < poly_N + 1) {

			char		sfmt[PLOT_STRING_MAX];

			sprintf(sfmt, " [%%i] = %% .%iE ", pl->fprecision - 1);
			sprintf(pl->data_box_text[N], sfmt, N, pl->lsq.b[N]);
		}
		else if (N == poly_N + 1) {

			sprintf(pl->data_box_text[N], " STD = ");
			plotDataBoxTextFmt(pl, N, pl->lsq.e[0]);
		}
	}

	if (pl->data_box_on != DATA_BOX_POLYFIT) {

		pl->data_box_on = DATA_BOX_POLYFIT;
		pl->data_box_X = pl->viewport.max_x;
		pl->data_box_Y = 0;
	}
}

void plotFigureClean(plot_t *pl)
{
	int		fN;

	for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		pl->figure[fN].busy = 0;
		pl->figure[fN].hidden = 0;
		pl->figure[fN].label[0] = 0;
	}

	for (fN = 0; fN < PLOT_AXES_MAX; ++fN) {

		pl->axis[fN].busy = AXIS_FREE;
		pl->axis[fN].slave = 0;
		pl->axis[fN].label[0] = 0;
		pl->axis[fN].expen = 0;
		pl->axis[fN].compact = 0;
	}

	pl->legend_X = 0;
	pl->legend_Y = 0;

	pl->data_box_on = DATA_BOX_FREE;
	pl->data_box_X = pl->viewport.max_x;
	pl->data_box_Y = 0;

	pl->slice_on = 0;
	pl->slice_range_on = 0;

	pl->on_X = -1;
	pl->on_Y = -1;

	pl->hover_figure = -1;
	pl->hover_legend = -1;
	pl->hover_data_box = -1;
	pl->hover_axis = -1;

	pl->mark_on = 0;

	plotSketchClean(pl);
}

static void
plotMarkLayout(plot_t *pl)
{
	const fval_t	*row;
	double		scale, offset, fval_X, fval_Y, bH;
	int		fN, fN_1, aN, bN, cX, cY, cZ, N, id_N, fig_N = 0;

	for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		if (pl->figure[fN].busy != 0 && pl->figure[fN].hidden == 0) {

			fig_N++;
		}
	}

	if (fig_N == 0) {

		return ;
	}

	bH = (double) pl->layout_mark * sqrt(fig_N) * 4.;

	pl->mark_N = (int) ((pl->viewport.max_x - pl->viewport.min_x) / bH);

	pl->mark_N = (pl->mark_N < 1) ? 1 : pl->mark_N;
	pl->mark_N = (pl->mark_N > PLOT_MARK_MAX) ? PLOT_MARK_MAX : pl->mark_N;

	bH = 1. / (double) (pl->mark_N * fig_N);

	for (fN = 0, fN_1 = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		if (pl->figure[fN].busy != 0 && pl->figure[fN].hidden == 0) {

			aN = pl->figure[fN].axis_X;
			cZ = pl->figure[fN].column_X;

			scale = pl->axis[aN].scale;
			offset = pl->axis[aN].offset;

			if (pl->axis[aN].slave != 0) {

				bN = pl->axis[aN].slave_N;
				scale *= pl->axis[bN].scale;
				offset = offset * pl->axis[bN].scale + pl->axis[bN].offset;
			}

			for (N = 0; N < pl->mark_N; ++N) {

				fval_X = (N * fig_N + fN_1) * bH;
				fval_X = (fval_X - offset) / scale;

				row = plotDataSliceGet(pl, pl->figure[fN].data_N,
							cZ, fval_X, &id_N);

				if (row != NULL) {

					cX = pl->figure[fN].column_X;
					cY = pl->figure[fN].column_Y;

					fval_X = (cX < 0) ? id_N : row[cX];
					fval_Y = (cY < 0) ? id_N : row[cY];

					pl->figure[fN].mark_X[N] = fval_X;
					pl->figure[fN].mark_Y[N] = fval_Y;
				}
				else {
					pl->figure[fN].mark_X[N] = 0.;
					pl->figure[fN].mark_Y[N] = 0.;
				}
			}

			fN_1++;
		}
	}
}

static void
plotMarkDraw(plot_t *pl, SDL_Surface *surface)
{
	double		X, Y, scale_X, scale_Y, offset_X, offset_Y;
	int		N, fN, aN, bN, fwidth;

	int		ncolor;

	SDL_LockSurface(surface);

	for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		if (pl->figure[fN].busy != 0 && pl->figure[fN].hidden == 0) {

			ncolor = (pl->figure[fN].hidden != 0) ? 9 : fN + 1;

			fwidth = pl->figure[fN].width;
			fwidth = (fwidth < 1) ? 1 : fwidth;

			aN = pl->figure[fN].axis_X;
			scale_X = pl->axis[aN].scale;
			offset_X = pl->axis[aN].offset;

			if (pl->axis[aN].slave != 0) {

				bN = pl->axis[aN].slave_N;
				scale_X *= pl->axis[bN].scale;
				offset_X = offset_X * pl->axis[bN].scale + pl->axis[bN].offset;
			}

			aN = pl->figure[fN].axis_Y;
			scale_Y = pl->axis[aN].scale;
			offset_Y = pl->axis[aN].offset;

			if (pl->axis[aN].slave != 0) {

				bN = pl->axis[aN].slave_N;
				scale_Y *= pl->axis[bN].scale;
				offset_Y = offset_Y * pl->axis[bN].scale + pl->axis[bN].offset;
			}

			X = (double) (pl->viewport.max_x - pl->viewport.min_x);
			Y = (double) (pl->viewport.min_y - pl->viewport.max_y);

			scale_X *= X;
			offset_X = offset_X * X + pl->viewport.min_x;
			scale_Y *= Y;
			offset_Y = offset_Y * Y + pl->viewport.max_y;

			for (N = 0; N < pl->mark_N; ++N) {

				X = pl->figure[fN].mark_X[N] * scale_X + offset_X;
				Y = pl->figure[fN].mark_Y[N] * scale_Y + offset_Y;

				if (fp_isfinite(X) && fp_isfinite(Y)) {

					drawMarkCanvas(pl->dw, surface, &pl->viewport, X, Y,
							pl->layout_mark, fN, ncolor, fwidth);
				}
			}
		}
	}

	SDL_UnlockSurface(surface);
}

void plotGroupAdd(plot_t *pl, int dN, int gN, int cN)
{
	if (dN < 0 || dN >= PLOT_DATASET_MAX) {

		ERROR("Dataset number is out of range\n");
		return ;
	}

	if (gN < 0 || gN >= PLOT_GROUP_MAX) {

		ERROR("Group number is out of range\n");
		return ;
	}

	if (cN < -1 || cN >= pl->data[dN].column_N + PLOT_SUBTRACT) {

		ERROR("Column number %i is out of range\n", cN);
		return ;
	}

	pl->data[dN].map[cN] = gN;
}

void plotGroupLabel(plot_t *pl, int gN, const char *label)
{
	if (gN < 0 || gN >= PLOT_GROUP_MAX) {

		ERROR("Group number is out of range\n");
		return ;
	}

	if (label[0] != 0) {

		strcpy(pl->group[gN].label, label);
	}
}

void plotGroupTimeUnwrap(plot_t *pl, int gN, int unwrap)
{
	if (gN < 0 || gN >= PLOT_GROUP_MAX) {

		ERROR("Group number is out of range\n");
		return ;
	}

	pl->group[gN].op_time_unwrap = (unwrap != 0) ? 1 : 0;
}

void plotGroupScale(plot_t *pl, int gN, double scale, double offset)
{
	if (gN < 0 || gN >= PLOT_GROUP_MAX) {

		ERROR("Group number is out of range\n");
		return ;
	}

	pl->group[gN].op_scale = 1;
	pl->group[gN].scale = scale;
	pl->group[gN].offset = offset;
}

void plotSliceSwitch(plot_t *pl)
{
	int		fN;

	if (pl->slice_range_on == 0) {

		pl->slice_range_on = 1;

		for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

			if (pl->figure[fN].slice_busy != 0) {

				pl->figure[fN].slice_base_X = pl->figure[fN].slice_X;
				pl->figure[fN].slice_base_Y = pl->figure[fN].slice_Y;
			}
		}
	}
	else if (pl->slice_range_on == 1) {

		pl->slice_range_on = 2;
	}
	else if (pl->slice_range_on == 2) {

		pl->slice_range_on = 0;
	}
}

void plotSliceTrack(plot_t *pl, int cur_X, int cur_Y)
{
	const fval_t	*row = NULL;
	double		fval_X, fval_Y;
	int		fN, aN, bN, dN, cX, cY, id_N;
	int		dN_s, aN_s, cX_s, job;

	if (pl->slice_range_on == 2)
		return ;

	if (pl->slice_axis_N < 0) {

		pl->slice_axis_N = pl->on_X;
	}

	if (pl->slice_axis_N < 0) {

		ERROR("No valid axis number to slice\n");
		return ;
	}

	dN_s = -1;
	aN_s = -1;
	cX_s = -1;

	for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		pl->figure[fN].slice_busy = 0;

		job = 0;

		if (pl->figure[fN].busy != 0 && pl->figure[fN].hidden == 0) {

			aN = pl->slice_axis_N;

			if (pl->axis[aN].busy == AXIS_BUSY_X) {

				if (pl->figure[fN].axis_X == aN) {

					job = 1;
				}
				else {
					bN = pl->figure[fN].axis_X;

					if (pl->axis[bN].slave != 0) {

						if (pl->axis[bN].slave_N == aN)
							job = 1;
					}
					else if (pl->axis[aN].slave != 0) {

						if (pl->axis[aN].slave_N == bN)
							job = 1;
					}
				}

				aN = pl->figure[fN].axis_X;
				cX = pl->figure[fN].column_X;

				fval_X = plotAxisConvInv(pl, aN, cur_X);
			}
			else if (pl->axis[aN].busy == AXIS_BUSY_Y) {

				if (pl->figure[fN].axis_Y == aN) {

					job = 1;
				}
				else {
					bN = pl->figure[fN].axis_Y;

					if (pl->axis[bN].slave != 0) {

						if (pl->axis[bN].slave_N == aN)
							job = 1;
					}
					else if (pl->axis[aN].slave != 0) {

						if (pl->axis[aN].slave_N == bN)
							job = 1;
					}
				}

				aN = pl->figure[fN].axis_Y;
				cX = pl->figure[fN].column_Y;

				fval_X = plotAxisConvInv(pl, aN, cur_Y);
			}
		}

		if (job) {

			dN = pl->figure[fN].data_N;

			if (dN_s != dN || aN_s != aN || cX_s != cX) {

				row = plotDataSliceGet(pl, dN, cX,
						fval_X, &id_N);

				dN_s = dN;
				aN_s = aN;
				cX_s = cX;
			}

			if (row != NULL) {

				cX = pl->figure[fN].column_X;
				cY = pl->figure[fN].column_Y;

				fval_X = (cX < 0) ? id_N : row[cX];
				fval_Y = (cY < 0) ? id_N : row[cY];

				pl->figure[fN].slice_busy = 1;
				pl->figure[fN].slice_X = fval_X;
				pl->figure[fN].slice_Y = fval_Y;
			}
		}
	}

	for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		pl->data_box_text[fN][0] = 0;

		if (pl->figure[fN].slice_busy != 0) {

			if (pl->slice_range_on != 0) {

				fval_X = pl->figure[fN].slice_base_X;
				fval_Y = pl->figure[fN].slice_base_Y;

				strcat(pl->data_box_text[fN], " \xCE\x94");
				plotDataBoxTextFmt(pl, fN, pl->figure[fN].slice_X - fval_X);

				strcat(pl->data_box_text[fN], "\xCE\x94");
				plotDataBoxTextFmt(pl, fN, pl->figure[fN].slice_Y - fval_Y);
			}
			else {
				plotDataBoxTextFmt(pl, fN, pl->figure[fN].slice_X);
				plotDataBoxTextFmt(pl, fN, pl->figure[fN].slice_Y);
			}
		}
	}

	if (pl->data_box_on != DATA_BOX_SLICE) {

		pl->data_box_on = DATA_BOX_SLICE;
		pl->data_box_X = pl->viewport.max_x;
		pl->data_box_Y = 0;
	}
}

static void
plotSliceLightDraw(plot_t *pl, SDL_Surface *surface)
{
	double		base_X, base_Y, data_X, data_Y, temp;
	int		fN, aN, bN;

	for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		if (pl->figure[fN].slice_busy != 0) {

			aN = pl->figure[fN].axis_X;
			bN = pl->figure[fN].axis_Y;

			base_X = plotAxisConv(pl, aN, pl->figure[fN].slice_base_X);
			base_Y = plotAxisConv(pl, bN, pl->figure[fN].slice_base_Y);

			data_X = plotAxisConv(pl, aN, pl->figure[fN].slice_X);
			data_Y = plotAxisConv(pl, bN, pl->figure[fN].slice_Y);

			if (data_X < base_X) {

				temp = base_X;
				base_X = data_X;
				data_X = temp;
			}

			if (data_Y < base_Y) {

				temp = base_Y;
				base_Y = data_Y;
				data_Y = temp;
			}

			SDL_LockSurface(surface);

			if (pl->axis[pl->slice_axis_N].busy == AXIS_BUSY_X) {

				if (fp_isfinite(base_X) && fp_isfinite(data_X)) {

					drawClipRect(surface, &pl->viewport,
							base_X, pl->viewport.min_y,
							data_X, pl->viewport.max_y,
							pl->sch->plot_hidden);
				}
			}
			else if (pl->axis[pl->slice_axis_N].busy == AXIS_BUSY_Y) {

				if (fp_isfinite(base_Y) && fp_isfinite(data_Y)) {

					drawClipRect(surface, &pl->viewport,
							pl->viewport.min_x, base_Y,
							pl->viewport.max_x, data_Y,
							pl->sch->plot_hidden);
				}
			}

			SDL_UnlockSurface(surface);
		}
	}
}

static void
plotSliceDraw(plot_t *pl, SDL_Surface *surface)
{
	double		base_X, base_Y, data_X, data_Y;
	int		fN, aN, bN;

	for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		if (pl->figure[fN].slice_busy != 0) {

			aN = pl->figure[fN].axis_X;
			bN = pl->figure[fN].axis_Y;

			if (pl->slice_range_on != 0) {

				base_X = plotAxisConv(pl, aN, pl->figure[fN].slice_base_X);
				base_Y = plotAxisConv(pl, bN, pl->figure[fN].slice_base_Y);
			}

			data_X = plotAxisConv(pl, aN, pl->figure[fN].slice_X);
			data_Y = plotAxisConv(pl, bN, pl->figure[fN].slice_Y);

			SDL_LockSurface(surface);

			drawDashReset(pl->dw);

			if (pl->axis[pl->slice_axis_N].busy == AXIS_BUSY_X) {

				if (pl->slice_range_on != 0) {

					if (fp_isfinite(base_X)) {

						drawLineDashed(pl->dw, surface, &pl->viewport,
								base_X, pl->viewport.min_y,
								base_X, pl->viewport.max_y,
								pl->sch->plot_text,
								pl->layout_fence_dash,
								pl->layout_fence_space);
					}
				}

				if (fp_isfinite(data_X)) {

					drawLineDashed(pl->dw, surface, &pl->viewport,
							data_X, pl->viewport.min_y,
							data_X, pl->viewport.max_y,
							pl->sch->plot_text,
							pl->layout_fence_dash,
							pl->layout_fence_space);
				}
			}
			else if (pl->axis[pl->slice_axis_N].busy == AXIS_BUSY_Y) {

				if (pl->slice_range_on != 0) {

					if (fp_isfinite(base_Y)) {

						drawLineDashed(pl->dw, surface, &pl->viewport,
								pl->viewport.min_x, base_Y,
								pl->viewport.max_x, base_Y,
								pl->sch->plot_text,
								pl->layout_fence_dash,
								pl->layout_fence_space);
					}
				}

				if (fp_isfinite(data_Y)) {

					drawLineDashed(pl->dw, surface, &pl->viewport,
							pl->viewport.min_x, data_Y,
							pl->viewport.max_x, data_Y,
							pl->sch->plot_text,
							pl->layout_fence_dash,
							pl->layout_fence_space);
				}
			}

			if (pl->slice_range_on != 0) {

				if (fp_isfinite(base_X) && fp_isfinite(base_Y)) {

					drawDotCanvas(pl->dw, surface, &pl->viewport,
							base_X, base_Y,
							pl->layout_fence_point,
							10, 0);
				}
			}

			if (fp_isfinite(data_X) && fp_isfinite(data_Y)) {

				drawDotCanvas(pl->dw, surface, &pl->viewport,
						data_X, data_Y,
						pl->layout_fence_point,
						10, 0);
			}

			SDL_UnlockSurface(surface);
		}
	}
}

static void
plotSketchDataChunkSetUp(plot_t *pl, int fN)
{
	int		hN;

	hN = pl->draw[fN].list_self;

	if (hN >= 0	&& pl->sketch[hN].figure_N == fN
			&& pl->sketch[hN].drawing == pl->figure[fN].drawing
			&& pl->sketch[hN].width == pl->figure[fN].width
			&& pl->sketch[hN].length < PLOT_SKETCH_CHUNK_SIZE) {

		/* Keep using this chunk */
	}
	else if (pl->sketch_list_garbage >= 0) {

		hN = pl->sketch_list_garbage;
		pl->sketch_list_garbage = pl->sketch[hN].linked;

		pl->sketch[hN].figure_N = fN;
		pl->sketch[hN].drawing = pl->figure[fN].drawing;
		pl->sketch[hN].width = pl->figure[fN].width;

		if (pl->sketch[hN].chunk == NULL) {

			pl->sketch[hN].chunk = (double *) malloc(sizeof(double) * PLOT_SKETCH_CHUNK_SIZE);

			if (pl->sketch[hN].chunk == NULL) {

				ERROR("Unable to allocate memory of %i sketch chunk\n", hN);
			}
		}

		pl->sketch[hN].length = 0;

		if (pl->draw[fN].list_self >= 0) {

			pl->sketch[hN].linked = pl->sketch[pl->draw[fN].list_self].linked;
			pl->sketch[pl->draw[fN].list_self].linked = hN;

			if (pl->draw[fN].list_self == pl->sketch_list_current_end)
				pl->sketch_list_current_end = hN;
		}
		else {
			pl->sketch[hN].linked = -1;

			if (pl->sketch_list_current >= 0) {

				pl->sketch[pl->sketch_list_current_end].linked = hN;
				pl->sketch_list_current_end = hN;
			}
			else {
				pl->sketch_list_current = hN;
				pl->sketch_list_current_end = hN;
			}
		}

		pl->draw[fN].list_self = hN;
	}
	else {
		ERROR("Unable to get free sketch chunk\n");

		pl->draw[fN].list_self = -1;
	}
}

static void
plotSketchDataAdd(plot_t *pl, int fN, double X, double Y)
{
	int		hN, length;

	hN = pl->draw[fN].list_self;

	if (hN >= 0) {

		length = pl->sketch[hN].length;

		pl->sketch[hN].chunk[length++] = X;
		pl->sketch[hN].chunk[length++] = Y;

		pl->sketch[hN].length = length;

		if (length >= PLOT_SKETCH_CHUNK_SIZE) {

			plotSketchDataChunkSetUp(pl, fN);
		}
	}
}

static void
plotSketchGarbage(plot_t *pl)
{
	int		N, hN, linked;

	hN = pl->sketch_list_todraw;

	while (hN >= 0) {

		linked = pl->sketch[hN].linked;

		pl->sketch[hN].linked = pl->sketch_list_garbage;
		pl->sketch_list_garbage = hN;

		hN = linked;
	}

	pl->sketch_list_todraw = pl->sketch_list_current;
	pl->sketch_list_current = -1;
	pl->sketch_list_current_end = -1;

	for (N = 0; N < PLOT_FIGURE_MAX; ++N)
		pl->draw[N].list_self = -1;
}

void plotSketchClean(plot_t *pl)
{
	int		N, hN, linked;

	hN = pl->sketch_list_todraw;

	while (hN >= 0) {

		linked = pl->sketch[hN].linked;

		pl->sketch[hN].linked = pl->sketch_list_garbage;
		pl->sketch_list_garbage = hN;

		hN = linked;
	}

	hN = pl->sketch_list_current;

	while (hN >= 0) {

		linked = pl->sketch[hN].linked;

		pl->sketch[hN].linked = pl->sketch_list_garbage;
		pl->sketch_list_garbage = hN;

		hN = linked;
	}

	pl->sketch_list_todraw = -1;
	pl->sketch_list_current = -1;
	pl->sketch_list_current_end = -1;

	for (N = 0; N < PLOT_FIGURE_MAX; ++N)
		pl->draw[N].list_self = -1;

	pl->draw_in_progress = 0;
}

static void
plotDrawPalette(plot_t *pl)
{
	draw_t			*dw = pl->dw;
	scheme_t		*sch = pl->sch;
	colType_t		*palette;

	palette = dw->palette;

	palette[0] = sch->plot_background;
	palette[1] = sch->plot_figure[0];
	palette[2] = sch->plot_figure[1];
	palette[3] = sch->plot_figure[2];
	palette[4] = sch->plot_figure[3];
	palette[5] = sch->plot_figure[4];
	palette[6] = sch->plot_figure[5];
	palette[7] = sch->plot_figure[6];
	palette[8] = sch->plot_figure[7];
	palette[9] = sch->plot_hidden;
	palette[10] = sch->plot_text;
}

static void
plotDrawFigureTrial(plot_t *pl, int fN)
{
	const fval_t	*row;
	double		scale_X, scale_Y, offset_X, offset_Y, im_MIN, im_MAX;
	double		X, Y, last_X, last_Y, im_X, im_Y, last_im_X, last_im_Y;
	int		dN, rN, xN, yN, xNR, yNR, aN, bN, id_N, top_N, kN, kN_cached;
	int		job, skipped, line, rc, ncolor, fdrawing, fwidth;

	ncolor = (pl->figure[fN].hidden != 0) ? 9 : fN + 1;

	fdrawing = pl->figure[fN].drawing;
	fwidth = pl->figure[fN].width;

	dN = pl->figure[fN].data_N;
	xN = pl->figure[fN].column_X;
	yN = pl->figure[fN].column_Y;

	xNR = plotDataRangeCacheFetch(pl, dN, xN);
	yNR = plotDataRangeCacheFetch(pl, dN, yN);

	aN = pl->figure[fN].axis_X;
	scale_X = pl->axis[aN].scale;
	offset_X = pl->axis[aN].offset;

	if (pl->axis[aN].slave != 0) {

		bN = pl->axis[aN].slave_N;
		scale_X *= pl->axis[bN].scale;
		offset_X = offset_X * pl->axis[bN].scale + pl->axis[bN].offset;
	}

	aN = pl->figure[fN].axis_Y;
	scale_Y = pl->axis[aN].scale;
	offset_Y = pl->axis[aN].offset;

	if (pl->axis[aN].slave != 0) {

		bN = pl->axis[aN].slave_N;
		scale_Y *= pl->axis[bN].scale;
		offset_Y = offset_Y * pl->axis[bN].scale + pl->axis[bN].offset;
	}

	X = (double) (pl->viewport.max_x - pl->viewport.min_x);
	Y = (double) (pl->viewport.min_y - pl->viewport.max_y);

	scale_X *= X;
	offset_X = offset_X * X + pl->viewport.min_x;
	scale_Y *= Y;
	offset_Y = offset_Y * Y + pl->viewport.max_y;

	rN = pl->draw[fN].rN;
	id_N = pl->draw[fN].id_N;

	top_N = id_N + (1UL << pl->data[dN].chunk_SHIFT);
	kN_cached = -1;

	plotSketchDataChunkSetUp(pl, fN);

	if (		fdrawing == FIGURE_DRAWING_LINE
			|| fdrawing == FIGURE_DRAWING_DASH) {

		skipped = pl->draw[fN].skipped;
		line = pl->draw[fN].line;

		last_X = pl->draw[fN].last_X;
		last_Y = pl->draw[fN].last_Y;

		last_im_X = last_X * scale_X + offset_X;
		last_im_Y = last_Y * scale_Y + offset_Y;

		do {
			kN = plotDataChunkN(pl, dN, rN);
			job = 1;

			if (kN != kN_cached) {

				if (xNR >= 0 && pl->rcache[xNR].chunk[kN].computed != 0) {

					if (pl->rcache[xNR].chunk[kN].finite != 0) {

						im_MIN = pl->rcache[xNR].chunk[kN].fmin * scale_X + offset_X;
						im_MAX = pl->rcache[xNR].chunk[kN].fmax * scale_X + offset_X;

						job = (	   im_MAX < pl->viewport.min_x - 16
							|| im_MIN > pl->viewport.max_x + 16) ? 0 : job;
					}
					else {
						job = 0;
					}
				}

				if (yNR >= 0 && pl->rcache[yNR].chunk[kN].computed != 0) {

					if (pl->rcache[yNR].chunk[kN].finite != 0) {

						im_MIN = pl->rcache[yNR].chunk[kN].fmin * scale_Y + offset_Y;
						im_MAX = pl->rcache[yNR].chunk[kN].fmax * scale_Y + offset_Y;

						job = (	   im_MIN < pl->viewport.min_y - 16
							|| im_MAX > pl->viewport.max_y + 16) ? 0 : job;
					}
					else {
						job = 0;
					}
				}

				kN_cached = kN;
			}

			if (job != 0 || line != 0) {

				if (skipped != 0) {

					plotDataSkip(pl, dN, &rN, &id_N, -1);

					skipped = 0;
				}

				row = plotDataGet(pl, dN, &rN);

				if (row == NULL) {

					pl->draw[fN].sketch = SKETCH_FINISHED;
					break;
				}

				X = (xN < 0) ? id_N : row[xN];
				Y = (yN < 0) ? id_N : row[yN];

				im_X = X * scale_X + offset_X;
				im_Y = Y * scale_Y + offset_Y;

				if (fp_isfinite(im_X) && fp_isfinite(im_Y)) {

					if (line != 0) {

						rc = drawLineTrial(pl->dw, &pl->viewport,
								last_im_X, last_im_Y, im_X, im_Y,
								ncolor, fwidth);

						if (rc != 0) {

							plotSketchDataAdd(pl, fN, last_X, last_Y);
							plotSketchDataAdd(pl, fN, X, Y);
						}
					}
					else {
						line = 1;
					}

					last_X = X;
					last_Y = Y;

					last_im_X = im_X;
					last_im_Y = im_Y;
				}
				else {
					line = 0;
				}

				id_N++;
			}

			if (job == 0) {

				plotDataChunkSkip(pl, dN, &rN, &id_N);

				skipped = 1;
				line = 0;
			}

			if (id_N > top_N) {

				pl->draw[fN].sketch = SKETCH_INTERRUPTED;
				pl->draw[fN].rN = rN;
				pl->draw[fN].id_N = id_N;
				pl->draw[fN].skipped = skipped;
				pl->draw[fN].line = line;
				pl->draw[fN].last_X = last_X;
				pl->draw[fN].last_Y = last_Y;
				break;
			}
		}
		while (1);
	}
	else if (fdrawing == FIGURE_DRAWING_DOT) {

		do {
			kN = plotDataChunkN(pl, dN, rN);
			job = 1;

			if (kN != kN_cached) {

				if (xNR >= 0 && pl->rcache[xNR].chunk[kN].computed != 0) {

					if (pl->rcache[xNR].chunk[kN].finite != 0) {

						im_MIN = pl->rcache[xNR].chunk[kN].fmin * scale_X + offset_X;
						im_MAX = pl->rcache[xNR].chunk[kN].fmax * scale_X + offset_X;

						job = (	   im_MAX < pl->viewport.min_x - 16
							|| im_MIN > pl->viewport.max_x + 16) ? 0 : job;
					}
					else {
						job = 0;
					}
				}

				if (yNR >= 0 && pl->rcache[yNR].chunk[kN].computed != 0) {

					if (pl->rcache[yNR].chunk[kN].finite != 0) {

						im_MIN = pl->rcache[yNR].chunk[kN].fmin * scale_Y + offset_Y;
						im_MAX = pl->rcache[yNR].chunk[kN].fmax * scale_Y + offset_Y;

						job = (	   im_MIN < pl->viewport.min_y - 16
							|| im_MAX > pl->viewport.max_y + 16) ? 0 : job;
					}
					else {
						job = 0;
					}
				}

				kN_cached = kN;
			}

			if (job != 0) {

				row = plotDataGet(pl, dN, &rN);

				if (row == NULL) {

					pl->draw[fN].sketch = SKETCH_FINISHED;
					break;
				}

				X = (xN < 0) ? id_N : row[xN];
				Y = (yN < 0) ? id_N : row[yN];

				im_X = X * scale_X + offset_X;
				im_Y = Y * scale_Y + offset_Y;

				if (fp_isfinite(im_X) && fp_isfinite(im_Y)) {

					rc = drawDotTrial(pl->dw, &pl->viewport,
							im_X, im_Y, fwidth,
							ncolor, 1);

					if (rc != 0) {

						plotSketchDataAdd(pl, fN, X, Y);
					}
				}

				id_N++;
			}

			if (job == 0) {

				plotDataChunkSkip(pl, dN, &rN, &id_N);
			}

			if (id_N > top_N) {

				pl->draw[fN].sketch = SKETCH_INTERRUPTED;
				pl->draw[fN].rN = rN;
				pl->draw[fN].id_N = id_N;
				break;
			}
		}
		while (1);
	}
}

static void
plotDrawSketch(plot_t *pl, SDL_Surface *surface)
{
	double		scale_X, offset_X, scale_Y, offset_Y;
	double		X, Y, last_X, last_Y, *chunk, *lend;
	int		hN, fN, aN, bN;

	int		fdrawing, fwidth, ncolor;

	hN = pl->sketch_list_todraw;

	drawDashReset(pl->dw);

	SDL_LockSurface(surface);

	while (hN >= 0) {

		fN = pl->sketch[hN].figure_N;

		ncolor = (pl->figure[fN].hidden != 0) ? 9 : fN + 1;

		fdrawing = pl->sketch[hN].drawing;
		fwidth = pl->sketch[hN].width;

		aN = pl->figure[fN].axis_X;
		scale_X = pl->axis[aN].scale;
		offset_X = pl->axis[aN].offset;

		if (pl->axis[aN].slave != 0) {

			bN = pl->axis[aN].slave_N;
			scale_X *= pl->axis[bN].scale;
			offset_X = offset_X * pl->axis[bN].scale + pl->axis[bN].offset;
		}

		aN = pl->figure[fN].axis_Y;
		scale_Y = pl->axis[aN].scale;
		offset_Y = pl->axis[aN].offset;

		if (pl->axis[aN].slave != 0) {

			bN = pl->axis[aN].slave_N;
			scale_Y *= pl->axis[bN].scale;
			offset_Y = offset_Y * pl->axis[bN].scale + pl->axis[bN].offset;
		}

		X = (double) (pl->viewport.max_x - pl->viewport.min_x);
		Y = (double) (pl->viewport.min_y - pl->viewport.max_y);

		scale_X *= X;
		offset_X = offset_X * X + pl->viewport.min_x;
		scale_Y *= Y;
		offset_Y = offset_Y * Y + pl->viewport.max_y;

		chunk = pl->sketch[hN].chunk;
		lend = chunk + pl->sketch[hN].length;

		if (fdrawing == FIGURE_DRAWING_LINE) {

			while (chunk < lend) {

				X = *chunk++;
				Y = *chunk++;

				last_X = X * scale_X + offset_X;
				last_Y = Y * scale_Y + offset_Y;

				X = *chunk++;
				Y = *chunk++;

				X = X * scale_X + offset_X;
				Y = Y * scale_Y + offset_Y;

				drawLineCanvas(pl->dw, surface, &pl->viewport,
						last_X, last_Y, X, Y,
						ncolor, fwidth);
			}
		}
		else if (fdrawing == FIGURE_DRAWING_DASH) {

			while (chunk < lend) {

				X = *chunk++;
				Y = *chunk++;

				last_X = X * scale_X + offset_X;
				last_Y = Y * scale_Y + offset_Y;

				X = *chunk++;
				Y = *chunk++;

				X = X * scale_X + offset_X;
				Y = Y * scale_Y + offset_Y;

				drawDashCanvas(pl->dw, surface, &pl->viewport,
						last_X, last_Y, X, Y,
						ncolor, fwidth, pl->layout_drawing_dash,
						pl->layout_drawing_space);
			}
		}
		else if (fdrawing == FIGURE_DRAWING_DOT) {

			while (chunk < lend) {

				X = *chunk++;
				Y = *chunk++;

				X = X * scale_X + offset_X;
				Y = Y * scale_Y + offset_Y;

				drawDotCanvas(pl->dw, surface, &pl->viewport,
						X, Y, fwidth,
						ncolor, 1);
			}
		}

		hN = pl->sketch[hN].linked;
	}

	SDL_UnlockSurface(surface);
}

static void
plotDrawAxis(plot_t *pl, SDL_Surface *surface, int aN)
{
	double		scale, offset, fmin, fmax, tih, tis, temp, emul;
	int		fexp, tmp, lpos, tpos, tmove, tfar, tfarb, tleft, tright, txlen;

	char		numfmt[PLOT_STRING_MAX], numbuf[PLOT_STRING_MAX];

	colType_t	axCol = pl->sch->plot_hidden;
	int		fN, bN, hover;

	for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		if (pl->figure[fN].busy != 0 && pl->figure[fN].hidden == 0) {

			if (	pl->figure[fN].axis_X == aN
				|| pl->figure[fN].axis_Y == aN) {

				if (axCol != pl->sch->plot_hidden) {

					axCol = pl->sch->plot_text;
				}
				else {
					axCol = pl->sch->plot_figure[fN];
				}
			}
		}
	}

	scale = pl->axis[aN].scale;
	offset = pl->axis[aN].offset;

	if (pl->axis[aN].slave != 0) {

		bN = pl->axis[aN].slave_N;
		scale *= pl->axis[bN].scale;
		offset = offset * pl->axis[bN].scale + pl->axis[bN].offset;
	}

	fmin = - offset / scale;
	fmax = 1. / scale + fmin;

	fexp = (int) ceil(log10((fmax - fmin) / 10.));
	tih = pow(10., (double) fexp);

	if ((fmax - fmin) / tih < 2.) {

		tih /= 5.;
		fexp--;
	}

	if ((fmax - fmin) / tih < 4.) {

		tih /= 2.;
		fexp--;
	}

	tis = ceil(fmin / tih) * tih;
	temp = tis * scale + offset;
	tis += (temp < 0.) ? tih : 0.;
	tih = (tis + tih == tis) ? fmax - tis : tih;

	emul = 1.;

	pl->axis[aN]._tih = tih * scale;
	pl->axis[aN]._tis = tis * scale + offset;

	if (pl->axis[aN].busy == AXIS_BUSY_X) {

		temp = (double) (pl->viewport.max_x - pl->viewport.min_x);
		scale *= temp;
		offset = offset * temp + pl->viewport.min_x;
	}
	else if (pl->axis[aN].busy == AXIS_BUSY_Y) {

		temp = (double) (pl->viewport.min_y - pl->viewport.max_y);
		scale *= temp;
		offset = offset * temp + pl->viewport.max_y;
	}

	SDL_LockSurface(surface);

	if (pl->axis[aN].busy == AXIS_BUSY_X) {

		lpos = pl->viewport.max_y + pl->layout_border + pl->axis[aN]._pos;
		hover = (pl->hover_axis == aN) ? 1 : 0;

		if (pl->hover_figure != -1 && pl->shift_on != 0) {

			fN = pl->hover_figure;

			hover = (pl->figure[fN].axis_X == aN) ? 1 : hover;
			hover = (pl->figure[fN].axis_Y == aN) ? 1 : hover;
		}

		if (hover != 0) {

			tmp = pl->layout_axis_box;
			tmp += (pl->axis[aN].compact == 0) ? pl->layout_label_box : 0;

			drawFillRect(surface, pl->viewport.min_x, lpos, pl->viewport.max_x,
					lpos + tmp, pl->sch->plot_hovered);
		}

		drawLine(pl->dw, surface, &pl->screen, pl->viewport.min_x, lpos, pl->viewport.max_x, lpos, pl->sch->plot_axis);

		for (temp = tis; temp < fmax; temp += tih) {

			tpos = (int) (temp * scale + offset);
			drawLine(pl->dw, surface, &pl->screen, tpos, lpos, tpos, lpos + pl->layout_tick_tooth, pl->sch->plot_axis);

			drawDashReset(pl->dw);

			if (pl->on_X == aN) {

				drawDashReset(pl->dw);

				drawLineDashed(pl->dw, surface, &pl->screen, tpos, pl->viewport.min_y, tpos, pl->viewport.max_y,
						pl->sch->plot_axis, pl->layout_grid_dash, pl->layout_grid_space);
			}
		}

		if (pl->on_X == aN) {

			drawLine(pl->dw, surface, &pl->screen, pl->viewport.min_x, lpos + 1, pl->viewport.max_x,
					lpos + 1, pl->sch->plot_axis);
		}

		if (pl->axis[aN].slave != 0) {

			drawLine(pl->dw, surface, &pl->screen, pl->viewport.min_x, lpos + pl->layout_tick_tooth,
				pl->viewport.max_x, lpos + pl->layout_tick_tooth, pl->sch->plot_axis);
		}
	}
	else if (pl->axis[aN].busy == AXIS_BUSY_Y) {

		lpos = pl->viewport.min_x - pl->layout_border - pl->axis[aN]._pos;
		hover = (pl->hover_axis == aN) ? 1 : 0;

		if (pl->hover_figure != -1 && pl->shift_on != 0) {

			fN = pl->hover_figure;

			hover = (pl->figure[fN].axis_X == aN) ? 1 : hover;
			hover = (pl->figure[fN].axis_Y == aN) ? 1 : hover;
		}

		if (hover != 0) {

			tmp = pl->layout_axis_box;
			tmp += (pl->axis[aN].compact == 0) ? pl->layout_label_box : 0;

			drawFillRect(surface, lpos - tmp, pl->viewport.min_y, lpos,
					pl->viewport.max_y, pl->sch->plot_hovered);
		}

		drawLine(pl->dw, surface, &pl->screen, lpos, pl->viewport.min_y, lpos, pl->viewport.max_y, pl->sch->plot_axis);

		for (temp = tis; temp < fmax; temp += tih) {

			tpos = (int) (temp * scale + offset);
			drawLine(pl->dw, surface, &pl->screen, lpos, tpos, lpos - pl->layout_tick_tooth, tpos, pl->sch->plot_axis);

			if (pl->on_Y == aN) {

				drawDashReset(pl->dw);

				drawLineDashed(pl->dw, surface, &pl->screen, pl->viewport.min_x, tpos, pl->viewport.max_x, tpos,
						pl->sch->plot_axis, pl->layout_grid_dash, pl->layout_grid_space);
			}
		}

		if (pl->on_Y == aN) {

			drawLine(pl->dw, surface, &pl->screen, lpos - 1, pl->viewport.min_y, lpos - 1,
					pl->viewport.max_y, pl->sch->plot_axis);
		}

		if (pl->axis[aN].slave != 0) {

			drawLine(pl->dw, surface, &pl->screen, lpos - pl->layout_tick_tooth, pl->viewport.min_y,
				lpos - pl->layout_tick_tooth, pl->viewport.max_y, pl->sch->plot_axis);
		}
	}

	SDL_UnlockSurface(surface);

	if (pl->axis[aN].busy == AXIS_BUSY_X) {

		lpos = pl->viewport.max_y + pl->layout_border + pl->axis[aN]._pos;
		tmove = pl->screen.min_x;
		tfar = pl->viewport.max_x;

		if (pl->axis[aN].expen != 0) {

			tmp = 0;

			while (fexp >= 3) {

				tmp += 3;
				fexp += -3;
				emul /= 1000.;
			}

			while (fexp <= -3) {

				tmp += -3;
				fexp += 3;
				emul *= 1000.;
			}

			if (tmp != 0) {

				sprintf(numbuf, "E%+d", tmp);

				tpos = (pl->axis[aN].compact == 0) ?
					lpos + pl->layout_axis_box :
					lpos + pl->layout_tick_tooth;

				tpos += pl->layout_font_height / 2;

				TTF_SizeUTF8(pl->font, numbuf, &txlen, &tmp);
				drawText(pl->dw, surface, pl->font, tfar - txlen, tpos,
						numbuf, TEXT_CENTERED_ON_Y, axCol);

				if (pl->axis[aN].compact != 0)
					tfar += - (txlen + pl->layout_font_long);
			}
		}

		if (pl->axis[aN].label[0] != 0 && pl->axis[aN].compact != 0) {

			TTF_SizeUTF8(pl->font, pl->axis[aN].label, &txlen, &tmp);
			tfar += - (txlen + pl->layout_font_long);
		}

		sprintf(numfmt, "%%.%df", (fexp < 0) ? - fexp : 0);

		for (temp = tis; temp < fmax; temp += tih) {

			tpos = (int) (temp * scale + offset);
			sprintf(numbuf, numfmt, temp * emul);

			TTF_SizeUTF8(pl->font, numbuf, &txlen, &tmp);
			tleft = tpos - txlen / 2 - pl->layout_font_long;
			tright = tpos + (txlen - txlen / 2);

			if (tmove < tleft && tright < tfar) {

				drawText(pl->dw, surface, pl->font, tpos, lpos + pl->layout_tick_tooth
						+ pl->layout_font_height / 2, numbuf,
						TEXT_CENTERED, axCol);

				tmove = tright;
			}
		}

		if (pl->axis[aN].compact != 0) {

			tpos = tfar + pl->layout_font_height / 2;
			lpos = lpos + pl->layout_tick_tooth + pl->layout_font_height / 2;
			tmp = TEXT_CENTERED_ON_Y;
		}
		else {
			tpos = (pl->viewport.min_x + pl->viewport.max_x) / 2;
			lpos = lpos + pl->layout_axis_box + pl->layout_font_height / 2;
			tmp = TEXT_CENTERED;
		}

		drawText(pl->dw, surface, pl->font, tpos, lpos, pl->axis[aN].label, tmp, axCol);
	}
	else if (pl->axis[aN].busy == AXIS_BUSY_Y) {

		lpos = pl->viewport.min_x - pl->layout_border - pl->axis[aN]._pos;
		tmove = pl->screen.max_y;
		tfar = pl->viewport.min_y;

		if (pl->axis[aN].expen != 0) {

			tmp = 0;

			while (fexp >= 3) {

				tmp += 3;
				fexp += -3;
				emul /= 1000.;
			}

			while (fexp <= -3) {

				tmp += -3;
				fexp += 3;
				emul *= 1000.;
			}

			if (tmp != 0) {

				sprintf(numbuf, "E%+d", tmp);

				tpos = (pl->axis[aN].compact == 0) ?
					lpos - pl->layout_axis_box :
					lpos - pl->layout_tick_tooth;

				tpos -= pl->layout_font_height / 2;

				TTF_SizeUTF8(pl->font, numbuf, &txlen, &tmp);
				drawText(pl->dw, surface, pl->font, tpos, tfar, numbuf,
						TEXT_CENTERED_ON_X | TEXT_VERTICAL, axCol);

				if (pl->axis[aN].compact != 0)
					tfar += txlen + pl->layout_font_long / 2;
			}
		}

		tfarb = tfar;

		if (pl->axis[aN].label[0] != 0 && pl->axis[aN].compact != 0) {

			TTF_SizeUTF8(pl->font, pl->axis[aN].label, &txlen, &tmp);
			tfar += txlen + pl->layout_font_long / 2;
		}

		sprintf(numfmt, "%%.%df", (fexp < 0) ? - fexp : 0);

		for (temp = tis; temp < fmax; temp += tih) {

			tpos = (int) (temp * scale + offset);
			sprintf(numbuf, numfmt, temp * emul);

			TTF_SizeUTF8(pl->font, numbuf, &txlen, &tmp);
			tleft = tpos + txlen / 2 + pl->layout_font_long;
			tright = tpos - (txlen - txlen / 2);

			if (tmove > tleft && tright > tfar) {

				drawText(pl->dw, surface, pl->font, lpos - pl->layout_tick_tooth
						- pl->layout_font_height / 2, tpos, numbuf,
						TEXT_CENTERED | TEXT_VERTICAL, axCol);

				tmove = tright;
			}
		}

		if (pl->axis[aN].compact != 0) {

			lpos = lpos - pl->layout_tick_tooth - pl->layout_font_height / 2;
			tpos = tfarb;
			tmp = TEXT_CENTERED_ON_X | TEXT_VERTICAL;
		}
		else {
			lpos = lpos - pl->layout_axis_box - pl->layout_font_height / 2;
			tpos = (pl->viewport.min_y + pl->viewport.max_y) / 2;
			tmp = TEXT_CENTERED | TEXT_VERTICAL;
		}

		drawText(pl->dw, surface, pl->font, lpos, tpos, pl->axis[aN].label, tmp, axCol);
	}
}

static void
plotLegendLayout(plot_t *pl)
{
	int		fN, size_X, size_Y;
	int		size_N = 0, size_MAX = 0;

	for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		if (pl->figure[fN].busy != 0) {

			TTF_SizeUTF8(pl->font, pl->figure[fN].label, &size_X, &size_Y);
			size_MAX = (size_MAX < size_X) ? size_X : size_MAX;

			size_N++;
		}
	}

	pl->legend_size_X = size_MAX + pl->layout_font_long * 2;
	pl->legend_N = size_N;

	if (pl->legend_X > pl->viewport.max_x - (size_MAX + pl->layout_font_height * 3))
		pl->legend_X = pl->viewport.max_x - (size_MAX + pl->layout_font_height * 3);

	if (pl->legend_Y > pl->viewport.max_y - pl->layout_font_height * (size_N + 1))
		pl->legend_Y = pl->viewport.max_y - pl->layout_font_height * (size_N + 1);

	if (pl->legend_X < pl->viewport.min_x + pl->layout_font_height)
		pl->legend_X = pl->viewport.min_x + pl->layout_font_height;

	if (pl->legend_Y < pl->viewport.min_y + pl->layout_font_height)
		pl->legend_Y = pl->viewport.min_y + pl->layout_font_height;
}

static void
plotLegendDraw(plot_t *pl, SDL_Surface *surface)
{
	int		boxX, boxY, size_X, size_Y;
	int		fN, legX, legY, ncolor, fwidth, fhover;

	legX = pl->legend_X;
	legY = pl->legend_Y;
	size_X = pl->layout_font_height * 2 + pl->legend_size_X;
	size_Y = pl->layout_font_height * pl->legend_N;

	SDL_LockSurface(surface);

	if (pl->hover_legend != -1) {

		drawFillRect(surface, legX, legY, legX + size_X,
				legY + size_Y, pl->sch->plot_hovered);
	}
	else if (pl->transparency_mode == 0) {

		drawFillRect(surface, legX, legY, legX + size_X,
				legY + size_Y, pl->sch->plot_background);
	}

	SDL_UnlockSurface(surface);

	for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		if (pl->figure[fN].busy != 0) {

			SDL_LockSurface(surface);

			ncolor = (pl->figure[fN].hidden != 0) ? 9 : fN + 1;

			fhover = (pl->hover_figure == fN) ? 1 : 0;

			if (pl->shift_on != 0) {

				fhover = (pl->figure[fN].axis_X == pl->hover_axis) ? 1 : fhover;
				fhover = (pl->figure[fN].axis_Y == pl->hover_axis) ? 1 : fhover;
			}

			if (fhover != 0) {

				boxX = legX + pl->layout_font_height * 2;
				size_X = pl->legend_size_X;
				size_Y = pl->layout_font_height;

				drawFillRect(surface, boxX, legY, boxX + size_X,
						legY + size_Y, pl->sch->plot_hovered);
			}

			fwidth = pl->figure[fN].width;
			boxY = legY + pl->layout_font_height / 2;

			if (pl->figure[fN].drawing == FIGURE_DRAWING_LINE) {

				boxX = legX + pl->layout_font_height / 2;

				if (fwidth > 1) {

					drawLineCanvas(pl->dw, surface, &pl->viewport, boxX, boxY,
							boxX + pl->layout_font_height, boxY,
							ncolor, fwidth);
				}
				else {
					drawLineCanvas(pl->dw, surface, &pl->viewport, boxX, boxY + .5,
							boxX + pl->layout_font_height, boxY + .5,
							ncolor, fwidth);
				}
			}
			else if (pl->figure[fN].drawing == FIGURE_DRAWING_DASH) {

				boxX = legX + pl->layout_font_height / 2;

				drawDashReset(pl->dw);

				if (fwidth > 1) {

					drawDashCanvas(pl->dw, surface, &pl->viewport, boxX, boxY,
							boxX + pl->layout_font_height, boxY,
							ncolor, fwidth, pl->layout_drawing_dash,
							pl->layout_drawing_space);
				}
				else {
					drawDashCanvas(pl->dw, surface, &pl->viewport, boxX, boxY + .5,
							boxX + pl->layout_font_height, boxY + .5,
							ncolor, fwidth, pl->layout_drawing_dash,
							pl->layout_drawing_space);
				}
			}
			else if (pl->figure[fN].drawing == FIGURE_DRAWING_DOT) {

				boxX = legX + pl->layout_font_height;

				if (fwidth > 2) {

					drawDotCanvas(pl->dw, surface, &pl->viewport,
							boxX + .5, boxY + .5,
							fwidth, ncolor, 1);
				}
				else {
					drawDotCanvas(pl->dw, surface, &pl->viewport,
							boxX + .5, boxY + .5,
							2, ncolor, 1);
				}
			}

			if (pl->mark_on != 0) {

				boxX = legX + pl->layout_font_height;
				fwidth = (fwidth < 1) ? 1 : fwidth;

				drawMarkCanvas(pl->dw, surface, &pl->viewport, boxX, boxY,
						pl->layout_mark, fN, ncolor, fwidth);
			}

			SDL_UnlockSurface(surface);

			drawText(pl->dw, surface, pl->font, legX + pl->layout_font_height * 2 + pl->layout_font_long,
				boxY, pl->figure[fN].label, TEXT_CENTERED_ON_Y,
				(pl->figure[fN].hidden) ? pl->sch->plot_hidden : pl->sch->plot_text);

			legY += pl->layout_font_height;
		}
	}
}

int plotLegendGetByClick(plot_t *pl, int cur_X, int cur_Y)
{
	int		fN, rN = -1;
	int		size_X, size_Y;
	int		legX, legY, relX, relY;

	legX = pl->legend_X;
	legY = pl->legend_Y;

	for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		if (pl->figure[fN].busy != 0) {

			TTF_SizeUTF8(pl->font, pl->figure[fN].label, &size_X, &size_Y);

			relX = cur_X - (legX + pl->layout_font_height * 2);
			relY = cur_Y - legY;

			if (relX > 0 && relX < pl->legend_size_X
					&& relY > 0 && relY < pl->layout_font_height) {

				rN = fN;
				break;
			}

			legY += pl->layout_font_height;
		}
	}

	pl->hover_figure = rN;

	return rN;
}

int plotLegendBoxGetByClick(plot_t *pl, int cur_X, int cur_Y)
{
	int		rN = -1;

	cur_X = cur_X - pl->legend_X;
	cur_Y = cur_Y - pl->legend_Y;

	if (cur_X > 0 && cur_X < pl->layout_font_height * 2
			&& cur_Y > 0 && cur_Y < pl->layout_font_height * pl->legend_N)
		rN = 0;

	pl->hover_legend = rN;

	return rN;
}

static void
plotDataBoxLayout(plot_t *pl)
{
	int		N, size_X, size_Y;
	int		size_N = 0, size_MAX = 0;

	if (pl->data_box_on == DATA_BOX_SLICE) {

		for (N = 0; N < PLOT_FIGURE_MAX; ++N) {

			if (pl->figure[N].busy != 0) {

				TTF_SizeUTF8(pl->font, pl->data_box_text[N], &size_X, &size_Y);

				size_MAX = (size_MAX < size_X) ? size_X : size_MAX;
				size_N++;
			}
		}
	}
	else if (pl->data_box_on == DATA_BOX_POLYFIT) {

		for (N = 0; N < PLOT_DATA_BOX_MAX; ++N) {

			if (pl->data_box_text[N][0] != 0) {

				TTF_SizeUTF8(pl->font, pl->data_box_text[N], &size_X, &size_Y);

				size_MAX = (size_MAX < size_X) ? size_X : size_MAX;
				size_N++;
			}
		}
	}

	pl->data_box_size_X = size_MAX;
	pl->data_box_N = size_N;

	if (pl->data_box_X > pl->viewport.max_x - (size_MAX + pl->layout_font_height))
		pl->data_box_X = pl->viewport.max_x - (size_MAX + pl->layout_font_height);

	if (pl->data_box_Y > pl->viewport.max_y - pl->layout_font_height * (size_N + 1))
		pl->data_box_Y = pl->viewport.max_y - pl->layout_font_height * (size_N + 1);

	if (pl->data_box_X < pl->viewport.min_x + pl->layout_font_height)
		pl->data_box_X = pl->viewport.min_x + pl->layout_font_height;

	if (pl->data_box_Y < pl->viewport.min_y + pl->layout_font_height)
		pl->data_box_Y = pl->viewport.min_y + pl->layout_font_height;
}

static void
plotDataBoxDraw(plot_t *pl, SDL_Surface *surface)
{
	int		boxY, size_X, size_Y;
	int		N, legX, legY;

	legX = pl->data_box_X;
	legY = pl->data_box_Y;
	size_X = pl->data_box_size_X;
	size_Y = pl->layout_font_height * pl->data_box_N;

	SDL_LockSurface(surface);

	if (pl->hover_data_box != -1) {

		drawFillRect(surface, legX, legY, legX + size_X,
				legY + size_Y, pl->sch->plot_hovered);
	}
	else if (pl->transparency_mode == 0) {

		drawFillRect(surface, legX, legY, legX + size_X,
				legY + size_Y, pl->sch->plot_background);
	}

	SDL_UnlockSurface(surface);

	if (pl->data_box_on == DATA_BOX_SLICE) {

		for (N = 0; N < PLOT_FIGURE_MAX; ++N) {

			if (pl->figure[N].busy != 0) {

				if (pl->data_box_text[N][0] != 0) {

					boxY = legY + pl->layout_font_height / 2;

					drawText(pl->dw, surface, pl->font, legX, boxY,
							pl->data_box_text[N], TEXT_CENTERED_ON_Y,
							pl->sch->plot_figure[N]);
				}

				legY += pl->layout_font_height;
			}
		}
	}
	else if (pl->data_box_on == DATA_BOX_POLYFIT) {

		for (N = 0; N < PLOT_DATA_BOX_MAX; ++N) {

			if (pl->data_box_text[N][0] != 0) {

				boxY = legY + pl->layout_font_height / 2;

				drawText(pl->dw, surface, pl->font, legX, boxY,
						pl->data_box_text[N], TEXT_CENTERED_ON_Y,
						pl->sch->plot_text);

				legY += pl->layout_font_height;
			}
		}
	}
}

int plotDataBoxGetByClick(plot_t *pl, int cur_X, int cur_Y)
{
	int		rN = -1;

	cur_X = cur_X - pl->data_box_X;
	cur_Y = cur_Y - pl->data_box_Y;

	if (cur_X > 0 && cur_X < pl->data_box_size_X
			&& cur_Y > 0 && cur_Y < pl->layout_font_height * pl->data_box_N)
		rN = 0;

	pl->hover_data_box = rN;

	return rN;
}

void plotLayout(plot_t *pl)
{
	int		aN, posX, posY;

	posX = 0;
	posY = 0;

	for (aN = 0; aN < PLOT_AXES_MAX; ++aN) {

		if (pl->axis[aN].busy == AXIS_BUSY_X) {

			if (pl->axis[aN].label[0] == 0)
				pl->axis[aN].compact = 1;

			pl->axis[aN]._pos = posX;
			posX += pl->layout_axis_box;
			posX += (pl->axis[aN].compact == 0) ? pl->layout_label_box : 0;
		}

		if (pl->axis[aN].busy == AXIS_BUSY_Y) {

			if (pl->axis[aN].label[0] == 0)
				pl->axis[aN].compact = 1;

			pl->axis[aN]._pos = posY;
			posY += pl->layout_axis_box;
			posY += (pl->axis[aN].compact == 0) ? pl->layout_label_box : 0;
		}
	}

	pl->viewport.min_x = pl->screen.min_x + posY + pl->layout_border;
	pl->viewport.max_x = pl->screen.max_x - pl->layout_border;
	pl->viewport.min_y = pl->screen.min_y + pl->layout_border;
	pl->viewport.max_y = pl->screen.max_y - posX - pl->layout_border;

	plotLegendLayout(pl);

	if (pl->data_box_on != DATA_BOX_FREE) {

		plotDataBoxLayout(pl);
	}

	if (pl->mark_on != 0) {

		if (pl->mark_N == 0) {

			plotMarkLayout(pl);
		}
	}
	else {
		pl->mark_N = 0;
	}
}

static void
plotDrawFigureTrialAll(plot_t *pl)
{
	int		FIGS[PLOT_FIGURE_MAX];
	int		N, fN, fQ, lN, dN, tTOP;

	lN = 0;

	for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		if (pl->figure[fN].busy != 0 && pl->figure[fN].hidden != 0)
			FIGS[lN++] = fN;
	}

	for (fN = 0; fN < PLOT_FIGURE_MAX; ++fN) {

		if (pl->figure[fN].busy != 0 && pl->figure[fN].hidden == 0)
			FIGS[lN++] = fN;
	}

	if (pl->draw_in_progress == 0) {

		for (N = 0; N < lN; ++N) {

			fN = FIGS[N];
			dN = pl->figure[fN].data_N;

			pl->draw[fN].sketch = SKETCH_STARTED;
			pl->draw[fN].rN = pl->data[dN].head_N;
			pl->draw[fN].id_N = pl->data[dN].id_N;

			pl->draw[fN].skipped = 0;
			pl->draw[fN].line = 0;
		}

		pl->draw_in_progress = 1;
	}

	if (pl->draw_in_progress != 0) {

		tTOP = SDL_GetTicks() + 20;

		drawClearTrial(pl->dw);

		do {
			fN = -1;

			for (N = 0; N < lN; ++N) {

				fQ = FIGS[N];

				if (pl->draw[fQ].sketch != SKETCH_FINISHED) {

					if (fN < 0) {

						fN = fQ;
					}
					else if (pl->draw[fQ].id_N < pl->draw[fN].id_N) {

						fN = fQ;
					}
				}
			}

			if (fN >= 0) {

				plotDrawFigureTrial(pl, fN);
			}
			else {
				plotSketchGarbage(pl);

				pl->draw_in_progress = 0;
				break;
			}
		}
		while (SDL_GetTicks() < tTOP);
	}
}

static void
plotDrawAxisAll(plot_t *pl, SDL_Surface *surface)
{
	int		aN;

	for (aN = 0; aN < PLOT_AXES_MAX; ++aN) {

		if (pl->axis[aN].busy != AXIS_FREE) {

			plotDrawAxis(pl, surface, aN);
		}
	}
}

void plotDraw(plot_t *pl, SDL_Surface *surface)
{
	if (pl->slice_range_on != 0) {

		plotSliceLightDraw(pl, surface);
	}

	drawPixmapAlloc(pl->dw, surface);

	plotDrawPalette(pl);
	plotDrawFigureTrialAll(pl);

	drawClearCanvas(pl->dw);

	plotDrawSketch(pl, surface);

	if (pl->mark_on != 0) {

		plotMarkDraw(pl, surface);
	}

	drawFlushCanvas(pl->dw, surface, &pl->viewport);
	drawClearCanvas(pl->dw);

	drawDashReset(pl->dw);

	plotDrawAxisAll(pl, surface);

	if (pl->slice_on != 0) {

		plotSliceDraw(pl, surface);
	}

	plotLegendDraw(pl, surface);

	drawFlushCanvas(pl->dw, surface, &pl->viewport);

	if (pl->data_box_on != DATA_BOX_FREE) {

		plotDataBoxDraw(pl, surface);
	}
}

