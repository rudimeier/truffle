/*** truffle.c -- tool to roll-over futures contracts
 *
 * Copyright (C) 2011-2013 Sebastian Freundt
 *
 * Author:  Sebastian Freundt <freundt@ga-group.nl>
 *
 * This file is part of truffle.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the author nor the names of any contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ***/

#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <sys/mman.h>
#if defined WORDS_BIGENDIAN
# include <limits.h>
#endif	/* WORDS_BIGENDIAN */
#include "truffle.h"
#include "schema.h"
#include "trod.h"
#include "cut.h"
#include "yd.h"
#include "dt-strpf.h"
#include "series.h"
#include "mmy.h"
#include "gbs.h"

#if defined STANDALONE
# include <stdio.h>
#endif	/* STANDALONE */
#if !defined __GNUC__ && !defined __INTEL_COMPILER
# define __builtin_expect(x, y)	x
#endif	/* !GCC && !ICC */
#if defined __INTEL_COMPILER
# pragma warning (disable:1572)
#endif	/* __INTEL_COMPILER */
#if !defined LIKELY
# define LIKELY(_x)	__builtin_expect((_x), 1)
#endif
#if !defined UNLIKELY
# define UNLIKELY(_x)	__builtin_expect((_x), 0)
#endif
#if !defined UNUSED
# define UNUSED(_x)	_x __attribute__((unused))
#endif	/* !UNUSED */
#if defined DEBUG_FLAG
# define TRUF_DEBUG(args...)	fprintf(stderr, args)
#else  /* !DEBUG_FLAG */
# define TRUF_DEBUG(args...)
#endif	/* DEBUG_FLAG */
#if defined DEBUG_TRANSACTIONS_FLAG
# define TRUF_DEBUG_TR(args...)	fprintf(stderr, args)
#else  /* !DEBUG_TRANSACTIONS_FLAG */
# define TRUF_DEBUG_TR(args...)
#endif	/* DEBUG_TRANSACTIONS_FLAG */
#if !defined countof
# define countof(x)	(sizeof(x) / sizeof(*(x)))
#endif	/* !countof */

typedef struct trser_s *trser_t;


#if 0
DEFUN bool
cut_non_nil_expo_p(trcut_t cut)
{
/* return whether a cut has a non-nil exposure,
 * note it is possible for a cut to have total exposure 0.0 and
 * yet have cash flows */
	for (size_t i = 0; i < cut->ncomps; i++) {
		if (cut->comps[i].y != 0.0) {
			return true;
		}
	}
	return false;
}
#endif


/* cutflo handling */
typedef enum {
	CUTFLO_TRANS_NIL_NIL = 0,
	CUTFLO_TRANS_NON_NIL,
	CUTFLO_TRANS_NIL_NON,
	CUTFLO_TRANS_NON_NON,
	CUTFLO_HAS_TRANS_BIT = 4,
} cutflo_trans_t;

struct __cutflo_st_s {
	/* user settable */
	/** tick value by which to multiply the cash flow */
	double tick_val;
	/**
	 * basis, unused unless set to nan in which case cut flow will
	 * pick a suitable basis, most of the time the first quote found */
	double basis;
	const_trtsc_t tsc;

	/* our stuff */
	union {
		cutflo_trans_t e;
		struct {
#if defined WORDS_BIGENDIAN
			unsigned int:sizeof(unsigned int) * CHAR_BIT - 3;
			unsigned int has_trans:1;
			unsigned int is_non_nil:1;
			unsigned int was_non_nil:1;
#else  /* !WORDS_BIGENDIAN */
			unsigned int was_non_nil:1;
			unsigned int is_non_nil:1;
			unsigned int has_trans:1;
#endif	/* WORDS_BIGENDIAN */
		};
	};
	/* should align neatly with the previous on 64b systems */
	unsigned int dvv_idx;

	double *bases;
	double *expos;
	double cum_flo;
	double inc_flo;
};

static void
init_cutflo_st(
	struct __cutflo_st_s *st, const_trtsc_t series,
	double tick_val, double basis)
{
	st->tick_val = tick_val;
	st->basis = basis;
	st->tsc = series;
	st->e = CUTFLO_TRANS_NIL_NIL;
	st->dvv_idx = 0;
	st->bases = calloc(series->ncons, sizeof(*st->bases));
	st->expos = calloc(series->ncons, sizeof(*st->expos));
	st->cum_flo = 0.0;
	st->inc_flo = 0.0;
	return;
}

static void
free_cutflo_st(struct __cutflo_st_s *st)
{
	free(st->bases);
	free(st->expos);
	return;
}

static void
warn_noquo(idate_t dt, trym_t ym, double expo)
{
	char dts[32];
	unsigned int yr = trym_yr(ym);
	unsigned int mo = trym_mo(ym);

	snprint_idate(dts, sizeof(dts), dt);
	fprintf(stderr, "\
cut as of %s contained %c%u with an exposure of %.8g but no quotes\n",
		dts, i_to_m(mo), yr, expo);
	return;
}

static cutflo_trans_t
cut_flow(struct __cutflo_st_s *st, trcut_t c, idate_t dt)
{
	double res = 0.0;
	const double *new_v = NULL;
	int is_non_nil = 0;

	for (size_t i = st->dvv_idx; i < st->tsc->ndvvs; i++) {
		/* assume sortedness */
		if (st->tsc->dvvs[i].d > dt) {
			break;
		} else if (st->tsc->dvvs[i].d == dt) {
			new_v = st->tsc->dvvs[i].v;
			st->dvv_idx = i + 1;
			break;
		}
	}
	for (size_t i = 0; i < c->ncomps; i++) {
		unsigned int mo = m_to_i(c->comps[i].month);
		unsigned int yr = c->comps[i].year;
		trym_t ym = cym_to_trym(yr, mo);
		double expo;
		ssize_t idx;
		double flo;

		if (ym == 0) {
			continue;
		}
		expo = c->comps[i].y * st->tick_val;

		if ((idx = tsc_find_cym_idx(st->tsc, ym)) < 0 ||
		    isnan(new_v[idx])) {
			if (expo != 0.0) {
				warn_noquo(dt, ym, expo);
			} else {
				cut_rem_cc(c, c->comps + i);
			}
			continue;
		}
		/* check for transition changes */
		if (st->expos[idx] != expo) {
			if (st->expos[idx] != 0.0) {
				double tot_flo = new_v[idx] - st->bases[idx];
				flo = tot_flo * st->expos[idx];
			} else {
				flo = 0.0;
				/* guess a basis if the user asked us to */
				if (isnan(st->basis)) {
					st->basis = new_v[idx];
				}
			}

			TRUF_DEBUG_TR(
				"TR %+.8g @ %.8g -> %+.8g @ %.8g -> %.8g\n",
				expo - st->expos[idx], new_v[idx],
				expo, st->bases[idx], flo);

			/* record bases */
			st->bases[idx] = new_v[idx];
			st->expos[idx] = expo;
			is_non_nil = 1;
		} else if (st->expos[idx] != 0.0) {
			double tot_flo = new_v[idx] - st->bases[idx];

			flo = tot_flo * st->expos[idx];
			TRUF_DEBUG_TR(
				"NO %+.8g @ %.8g (- %.8g) -> %.8g => %.8g\n",
				expo, new_v[idx], st->bases[idx],
				flo, flo + st->bases[idx]);
			st->bases[idx] = new_v[idx];
			is_non_nil = 1;
		} else {
			/* st->expos[idx] == 0.0 && st->expos[idx] == expo */
			flo = 0.0;
			cut_rem_cc(c, c->comps + i);
		}
		/* munch it all together */
		res += flo;
	}
	st->was_non_nil = st->is_non_nil;
	st->is_non_nil = is_non_nil;
	st->inc_flo = res;
	st->cum_flo += res;
	return st->e;
}

static cutflo_trans_t
cut_base(struct __cutflo_st_s *st, trcut_t c, idate_t dt)
{
	double res = 0.0;
	const double *new_v = NULL;
	int is_non_nil = 0;

	for (size_t i = st->dvv_idx; i < st->tsc->ndvvs; i++) {
		if (st->tsc->dvvs[i].d == dt) {
			new_v = st->tsc->dvvs[i].v;
			st->dvv_idx = i + 1;
			break;
		}
	}
	for (size_t i = 0; i < c->ncomps; i++) {
		unsigned int mo = m_to_i(c->comps[i].month);
		unsigned int yr = c->comps[i].year;
		trym_t ym = cym_to_trym(yr, mo);
		double expo;
		ssize_t idx;
		double flo;

		if (ym == 0) {
			continue;
		}
		expo = c->comps[i].y * st->tick_val;

		if ((idx = tsc_find_cym_idx(st->tsc, ym)) < 0 ||
		    isnan(new_v[idx])) {
			if (expo != 0.0) {
				warn_noquo(dt, ym, expo);
			} else {
				cut_rem_cc(c, c->comps + i);
			}
			continue;
		}
		/* check for transition changes */
		if (expo != 0.0) {
			double tot_flo;

			if (UNLIKELY(isnan(st->basis))) {
				st->basis = 0.0;
			}

			tot_flo = (new_v[idx] - st->basis);

			flo = tot_flo * expo;
			TRUF_DEBUG_TR(
				"NO %+.8g @ %.8g => %.8g\n",
				expo, tot_flo, flo);
			is_non_nil = 1;
		} else {
			flo = 0.0;
			cut_rem_cc(c, c->comps + i);
		}
		/* munch it all together */
		res += flo;
	}
	st->was_non_nil = st->is_non_nil;
	st->is_non_nil = is_non_nil;
	st->inc_flo = res;
	st->cum_flo += res;
	return st->e;
}

static cutflo_trans_t
cut_sparse(struct __cutflo_st_s *st, trcut_t c, idate_t dt)
{
	double res = 0.0;
	const double *new_v = NULL;
	int is_non_nil = 0;
	int has_trans = 0;

	for (size_t i = st->dvv_idx; i < st->tsc->ndvvs; i++) {
		if (st->tsc->dvvs[i].d == dt) {
			new_v = st->tsc->dvvs[i].v;
			st->dvv_idx = i + 1;
			break;
		}
	}
	for (size_t i = 0; i < c->ncomps; i++) {
		unsigned int mo = m_to_i(c->comps[i].month);
		unsigned int yr = c->comps[i].year;
		trym_t ym = cym_to_trym(yr, mo);
		double expo;
		ssize_t idx;
		double flo;

		if (ym == 0) {
			continue;
		}
		expo = c->comps[i].y * st->tick_val;

		if ((idx = tsc_find_cym_idx(st->tsc, ym)) < 0 ||
		    isnan(new_v[idx])) {
			if (expo != 0.0) {
				warn_noquo(dt, ym, expo);
			} else {
				cut_rem_cc(c, c->comps + i);
			}
			continue;
		}
		/* check for transition changes */
		if (st->expos[idx] != expo) {
			if (st->expos[idx] != 0.0) {
				double tot_flo = new_v[idx] - st->bases[idx];
				double trans_expo = st->expos[idx] - expo;
				flo = tot_flo * trans_expo;
			} else {
				flo = 0.0;
				/* guess a basis if the user asked us to */
				if (isnan(st->basis)) {
					st->basis = 0.0;
				}
			}

			TRUF_DEBUG_TR(
				"TR %+.8g @ %.8g -> %+.8g @ %.8g -> %.8g\n",
				expo - st->expos[idx], new_v[idx],
				expo, st->bases[idx], flo);

			/* record bases */
			if (st->expos[idx] == 0.0 || expo == 0.0) {
				st->bases[idx] = new_v[idx];
			}
			st->expos[idx] = expo;
			is_non_nil = 1;
			has_trans = 1;
		} else {
			/* st->expos[idx] == 0.0 && st->expos[idx] == expo */
			flo = 0.0;
			has_trans = 0;
			cut_rem_cc(c, c->comps + i);
		}
		/* munch it all together */
		res += flo;
	}
	st->has_trans = has_trans;
	st->was_non_nil = st->is_non_nil;
	st->is_non_nil = is_non_nil;
	st->inc_flo = res;
	st->cum_flo += res;
	return st->e;
}


struct __series_spec_s {
	double tick_val;
	double basis;
	unsigned int cump:1;
	unsigned int abs_dimen_p:1;
	unsigned int sparsep:1;
};

static cutflo_trans_t(*pick_cf_fun(struct __series_spec_s ser_sp))
	(struct __cutflo_st_s*, trcut_t, idate_t)
{
	if (LIKELY(!ser_sp.abs_dimen_p && !ser_sp.sparsep)) {
		return cut_flow;
	} else if (!ser_sp.sparsep) {
		return cut_base;
	} else {
		return cut_sparse;
	}
}

static void
roll_over_series(
	trsch_t s, trtsc_t ser, struct __series_spec_s ser_sp, FILE *whither)
{
	trcut_t c = NULL;
	struct __cutflo_st_s cfst;
	cutflo_trans_t(*const cf)(struct __cutflo_st_s*, trcut_t, idate_t) =
		pick_cf_fun(ser_sp);
	const unsigned int trbit = UNLIKELY(ser_sp.sparsep)
		? CUTFLO_HAS_TRANS_BIT : CUTFLO_TRANS_NON_NIL;

	/* init out cut flow state structure */
	init_cutflo_st(&cfst, ser, ser_sp.tick_val, ser_sp.basis);

	/* find the earliest date */
	for (size_t i = 0; i < ser->ndvvs; i++) {
		idate_t dt = ser->dvvs[i].d;
		daysi_t mc_ds = idate_to_daysi(dt);

		/* anchor now contains the very first date and value */
		if ((c = make_cut(c, s, mc_ds)) == NULL) {
			continue;
		}

		if (cf(&cfst, c, dt) > trbit) {
			char buf[32];
			double val;

			if (LIKELY(!ser_sp.abs_dimen_p && ser_sp.cump)) {
				val = cfst.cum_flo + cfst.basis;
			} else if (LIKELY(!ser_sp.abs_dimen_p)) {
				val = cfst.inc_flo;
			} else if (LIKELY(!ser_sp.cump)) {
				val = cfst.inc_flo;
			} else {
				val = cfst.cum_flo;
			}
			snprint_idate(buf, sizeof(buf), dt);
			fprintf(whither, "%s\t%.8g\n", buf, val);
		}
	}

	/* free resources */
	if (c) {
		free_cut(c);
	}
	/* free up resources */
	free_cutflo_st(&cfst);
	return;
}


/* trod goodness */
typedef struct trod_state_s *trod_state_t;
typedef struct trod_event_s *trod_event_t;

struct trod_state_s {
	uint8_t val;
	trym_t ym:TRYM_WIDTH;
};

struct trod_event_s {
	trod_instant_t when;
	struct trod_state_s what[];
};

/* trod container */
struct trod_s {
	size_t ninst;
	size_t nev;
	/* 0_event terminated list of events (NINST of them) */
	trod_event_t ev[];
};

static inline void
activate(gbs_t bs, int ry, unsigned int m)
{
	gbs_set(bs, 12 * ry + (m - 1));
	return;
}

static inline void
deactivate(gbs_t bs, int ry, unsigned int m)
{
	gbs_unset(bs, 12 * ry + (m - 1));
	return;
}

static inline int
activep(gbs_t bs, int ry, unsigned int m)
{
	return gbs_set_p(bs, 12 * ry + (m - 1));
}

static inline void
flip_over(gbs_t bs, int ry)
{
/* flip over to a new year in the ACTIVE bitset */
	gbs_shift_lsb(bs, 12 * ry);
	return;
}

static int
update_gbs_ev(gbs_t bs, trod_event_t ev)
{
	int res = 0;

	for (const struct trod_state_s *s = ev->what; s->ym; s++) {
		unsigned int m = trym_mo(s->ym);
		unsigned int y = trym_yr(s->ym);
		int ry = y - ev->when.y;

		if (!s->val) {
			deactivate(bs, ry, m);
			res++;
		} else if (s->val > 1U && activep(bs, ry, m)) {
			continue;
		} else {
			activate(bs, ry, m);
			res++;
		}
	}
	return res;
}

static int
update_gbs(gbs_t bs, trod_t td, trod_instant_t inst)
{
	static trod_instant_t last;
	static size_t i;
	int res = 0;

	if (trod_inst_lt_p(inst, last)) {
		/* we'll have to build it all up again */
		last = (trod_instant_t){0};
		i = 0;
	}
	for (; i < td->ninst; i++) {
		trod_event_t x = td->ev[i];

		if (last.y < x->when.y) {
			flip_over(bs, x->when.y - last.y);
		}
		if (trod_inst_lt_p(inst, x->when)) {
			/* we went to far, aye? */
			last = inst;
			break;
		}
		res += update_gbs_ev(bs, x);
		last = x->when;
	}
	return res;
}

static void
print_contracts(gbs_t bs, trod_t td, trod_instant_t inst, struct trcut_pr_s opt)
{
	static char buf[256];
	char *q = buf;

	update_gbs(bs, td, inst);
	q += dt_strf(buf, sizeof(buf), inst);
	*q++ = '\t';
	for (size_t k = 0; k < bs->nbits; k++) {
		unsigned int yr = k / 12U;
		unsigned int mo = k % 12U;

		if (activep(bs, yr, mo + 1U)) {
			if (opt.abs || opt.oco) {
				yr += inst.y;
			}
			if (!opt.oco) {
				*q++ = i_to_m(mo + 1U);
			}
			/* always print the year */
			q += snprintf(
				q, sizeof(buf) - (q - buf),
				"%u", yr);
			if (opt.oco) {
				q += snprintf(
					q, sizeof(buf) - (q - buf),
					"%02u", mo + 1U);
			}
			*q++ = ' ';
		}
	}
	q--;
	*q++ = '\n';
	*q = '\0';
	fputs(buf, opt.out);
	return;
}

static trcut_t
make_cut_from_gbs(trcut_t cut, struct gbs_s bs[static 1], trod_instant_t inst)
{
	if (cut) {
		/* quickly rinse the old cut */
		for (size_t i = 0; i < cut->ncomps; i++) {
			cut->comps[i].y = 0.0;
		}
	}
	for (size_t k = 0; k < bs->nbits; k++) {
		unsigned int yr = k / 12U;
		unsigned int mo = k % 12U;

		if (activep(bs, yr, mo + 1U)) {
			struct trcc_s cc;

			cc.month = (uint8_t)i_to_m(mo + 1U);
			cc.year = (uint16_t)(yr + inst.y);
			cc.y = 1.0;

			/* add this cut cell */
			cut = cut_add_cc(cut, cc);
		}
	}
	return cut;
}

static void
trod_roll_over_series(
	trod_t td, trtsc_t ser, struct __series_spec_s ser_sp, FILE *whither)
{
	struct gbs_s active[1] = {{0}};
	trcut_t c = NULL;
	struct __cutflo_st_s cfst;
	cutflo_trans_t(*const cf)(struct __cutflo_st_s*, trcut_t, idate_t) =
		pick_cf_fun(ser_sp);
	const unsigned int trbit = UNLIKELY(ser_sp.sparsep)
		? CUTFLO_HAS_TRANS_BIT : CUTFLO_TRANS_NON_NIL;

	/* initialise the activity tracker */
	init_gbs(active, 12U * 5U);
	/* init out cut flow state structure */
	init_cutflo_st(&cfst, ser, ser_sp.tick_val, ser_sp.basis);
	/* traverse the series, it's chronological */
	for (size_t i = 0; i < ser->ndvvs; i++) {
		idate_t dt = ser->dvvs[i].d;
		trod_instant_t di = {
			idate_y(dt), idate_m(dt), idate_d(dt), TROD_ALL_DAY,
		};

		if (update_gbs(active, td, di)) {
			/* update the cut */
			c = make_cut_from_gbs(c, active, di);
		}
		/* do fuckall if cut is empty */
		if (c == NULL) {
			continue;
		}

		if (cf(&cfst, c, dt) > trbit) {
			char buf[32];
			double val;

			if (LIKELY(!ser_sp.abs_dimen_p && ser_sp.cump)) {
				val = cfst.cum_flo + cfst.basis;
			} else if (LIKELY(!ser_sp.abs_dimen_p)) {
				val = cfst.inc_flo;
			} else if (LIKELY(!ser_sp.cump)) {
				val = cfst.inc_flo;
			} else {
				val = cfst.cum_flo;
			}
			snprint_idate(buf, sizeof(buf), dt);
			fprintf(whither, "%s\t%.8g\n", buf, val);
		}
	}
	/* free up resources */
	if (c) {
		free_cut(c);
	}
	free_cutflo_st(&cfst);
	fini_gbs(active);
	return;
}


#if defined STANDALONE
#if defined __INTEL_COMPILER
# pragma warning (disable:593)
#endif	/* __INTEL_COMPILER */
#include "truffle-clo.h"
#include "truffle-clo.c"
#if defined __INTEL_COMPILER
# pragma warning (default:593)
#endif	/* __INTEL_COMPILER */

int
main(int argc, char *argv[])
{
	struct gengetopt_args_info argi[1];
	trsch_t sch = NULL;
	trod_t td = NULL;
	trtsc_t ser = NULL;
	int res = 0;

	if (cmdline_parser(argc, argv, argi)) {
		exit(1);
	}

	if (argi->schema_given) {
		sch = read_schema(argi->schema_arg);
	} else {
		sch = read_schema("-");
	}
	if (sch == NULL && argi->schema_given) {
		/* retry with trod reader */
		td = read_trod(argi->schema_arg);
	}
	if (UNLIKELY(sch == NULL && td == NULL)) {
		fputs("schema unreadable\n", stderr);
		res = 1;
		goto sch_out;
	}
	/* check if we're in series mode */
	if (argi->series_given) {
		const char *file = argi->series_arg;

		if ((ser = read_series_from_file(file)) == NULL) {
			fprintf(stderr, "cannot read series file %s\n", file);
			res = 1;
			goto ser_out;
		}
	}

	/* finally call our main routine */
	if (ser != NULL && sch != NULL) {
		struct __series_spec_s sp = {
			.tick_val = argi->tick_value_given
			? argi->tick_value_arg : 1.0,
			.basis = argi->basis_given
			? argi->basis_arg : NAN,
			.cump = !argi->flow_given,
			.abs_dimen_p = argi->abs_dimen_given,
			.sparsep = argi->sparse_given,
		};
		roll_over_series(sch, ser, sp, stdout);

	} else if (ser != NULL && td != NULL) {
		struct __series_spec_s sp = {
			.tick_val = argi->tick_value_given
			? argi->tick_value_arg : 1.0,
			.basis = argi->basis_given
			? argi->basis_arg : NAN,
			.cump = !argi->flow_given,
			.abs_dimen_p = argi->abs_dimen_given,
			.sparsep = argi->sparse_given,
		};
		trod_roll_over_series(td, ser, sp, stdout);

	} else if (sch != NULL && argi->inputs_num == 0) {
		print_schema(sch, stdout);

	} else if (td != NULL && argi->inputs_num == 0) {
		fputs("\
Use trod tool to display trod description files\n", stdout);

	} else if (sch != NULL) {
		struct trcut_pr_s opt = {
			.abs = argi->abs_given,
			.oco = argi->oco_given,
			.rnd = argi->round_given,
			.lever = argi->lever_given ? argi->lever_arg : 1.0,
			.out = stdout,
		};
		trcut_t c = NULL;

		for (size_t i = 0; i < argi->inputs_num; i++) {
			idate_t dt = read_date(argi->inputs[i], NULL);
			daysi_t ds = idate_to_daysi(dt);

			if ((c = make_cut(c, sch, ds))) {
				print_cut(c, dt, opt);
			}
		}
		if (c) {
			free_cut(c);
		}
	} else if (td != NULL) {
		struct gbs_s active[1U] = {{0U}};
		struct trcut_pr_s opt = {
			.abs = argi->abs_given,
			.oco = argi->oco_given,
			.rnd = argi->round_given,
			.out = stdout,
		};

		init_gbs(active, 12U * 5U);
		for (size_t i = 0; i < argi->inputs_num; i++) {
			trod_instant_t inst = dt_strp(argi->inputs[i]);

			print_contracts(active, td, inst, opt);
		}
		fini_gbs(active);

	} else {
		/* not reached */
		;
	}

	if (ser != NULL) {
		free_series(ser);
	}
ser_out:
	if (sch != NULL) {
		free_schema(sch);
	}
sch_out:
	/* just to make sure */
	fflush(stdout);
	cmdline_parser_free(argi);
	return res;
}
#endif	/* STANDALONE */

/* truffle.c ends here */
