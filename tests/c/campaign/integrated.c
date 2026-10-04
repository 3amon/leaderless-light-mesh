/* Integrated groups+clock campaign: every node runs the REAL group_engine
 * AND the REAL clock_estimator, coupled exactly as the firmware couples
 * them — engine membership weights feed the estimator's vote weights, and
 * the estimator's per-peer |last_delta| feeds the engine's timing gate.
 * This closes the gap where cohesion_sim used static deltas and clock_sim
 * had no membership. Measurement mode, no asserts. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "clock_estimator.h"
#include "group_engine.h"
#include "peer_slot_policy.h"
#include "sync_schedule.h"

#define MAXN 8

struct inode {
	bool alive, rogue;
	uint32_t node_id, seed;
	double ppm;
	int64_t offset_us, lie_us;
	struct group_engine engine;
	struct clock_estimator clk;
	struct clock_peer_estimator cpeer[MAXN];   /* indexed by sim node */
	int64_t last_any_ms[MAXN], last_direct_ms[MAXN];
	uint8_t peer_sid[MAXN];
	uint32_t peer_view[MAXN], peer_seed[MAXN];
	uint32_t rng;
	int64_t next_local_ms, next_realm_ms, next_update_ms, next_engine_ms;
};

static struct inode N[MAXN];
static int COUNT;
static int64_t now_ms;
static uint32_t grng;

static uint32_t rnd(void) { grng = grng * 1664525U + 1013904223U; return grng; }
/* Percentage of beacon copies dropped in transit, 0 = the historical
 * lossless radio. A weak real link (-80 dBm) loses a meaningful share. */
static unsigned idrop_pct;
/* Start every peer clock at STRANGER trust (0.05) instead of the estimator's
 * 1.0 default. The firmware default is 1.0 until the first group tick sets
 * a real weight; whether the first clock update runs before that tick is a
 * thread race on the device, and the lucky order snaps a cold node onto the
 * first voice it hears. This models the unlucky order, which is the one
 * seen in the field on 2026-09-29. */
static bool istranger_start;

static int64_t local_us_of(const struct inode *n, int64_t truth_us)
{
	return truth_us + n->offset_us +
	       (int64_t)((double)truth_us * n->ppm * 1e-6);
}

static void iboot(struct inode *n)
{
	group_engine_init(&n->engine, n->node_id, n->seed);
	clock_estimator_init(&n->clk, local_us_of(n, now_ms * 1000));
	for (int p = 0; p < MAXN; p++) {
		clock_peer_estimator_init(&n->cpeer[p]);
		if (istranger_start)
			clock_peer_estimator_set_weight(&n->cpeer[p], 0.05f);
		n->last_any_ms[p] = -1000000;
		n->last_direct_ms[p] = -1000000;
	}
	n->next_local_ms = now_ms;
	n->next_realm_ms = now_ms;
	n->next_update_ms = now_ms;
	n->next_engine_ms = now_ms;
}

static void iinit(int count, uint32_t seed)
{
	memset(N, 0, sizeof(N));
	COUNT = count;
	now_ms = 0;
	grng = seed;
	uint32_t cand = 0x5000 + seed % 913U;

	for (int i = 0; i < count; i++) {
		for (;;) {
			cand += 0x2e7;
			struct group_engine probe;

			group_engine_init(&probe, cand, cand ^ 0x5eed);
			bool clash = false;

			for (int j = 0; j < i; j++)
				if (N[j].engine.my_sid == probe.my_sid)
					clash = true;
			if (!clash)
				break;
		}
		N[i].node_id = cand;
		N[i].seed = cand ^ 0x5eed;
		N[i].alive = true;
		N[i].ppm = -300.0 + (double)(rnd() % 600U);
		N[i].offset_us = -500000 + (int64_t)(rnd() % 1000000U);
		N[i].rng = cand;
		iboot(&N[i]);
	}
}

static void istep_ms(void)
{
	now_ms++;
	int64_t truth = now_ms * 1000;

	/* beacons */
	for (int tx = 0; tx < COUNT; tx++) {
		struct inode *s = &N[tx];

		if (!s->alive)
			continue;
		bool local = now_ms >= s->next_local_ms;
		bool realm = now_ms >= s->next_realm_ms;

		if (!local && !realm)
			continue;
		if (local)
			s->next_local_ms = now_ms + sync_schedule_delay_ms(
				SYNC_SCHEDULE_SCALABLE, SYNC_SCHEDULE_LOCAL,
				(s->rng = s->rng * 1664525U + 1013904223U));
		if (realm)
			s->next_realm_ms = now_ms + sync_schedule_delay_ms(
				SYNC_SCHEDULE_SCALABLE, SYNC_SCHEDULE_REALM,
				(s->rng = s->rng * 1664525U + 1013904223U));
		int64_t s_local = local_us_of(s, truth);
		double s_global = s->rogue
			? (double)truth + (double)s->lie_us
			: clock_estimator_global_at(&s->clk, s_local);

		for (int rx = 0; rx < COUNT; rx++) {
			struct inode *r = &N[rx];

			if (rx == tx || !r->alive || r->rogue)
				continue;
			if (idrop_pct && rnd() % 100U < idrop_pct)
				continue;
			int64_t transit = 2000 + (int64_t)(rnd() % 6000U);

			if (rnd() % 20U == 0U)
				transit += 30000;
			int64_t arr = local_us_of(r, truth + transit);

			clock_peer_estimator_add_sample(
				&r->cpeer[tx], &clock_estimator_default_config,
				arr, (int64_t)s_global, s_local,
				s->rogue ? 1.0 : s->clk.h);
			r->last_any_ms[tx] = now_ms;
			if (local)
				r->last_direct_ms[tx] = now_ms;
			r->peer_sid[tx] = s->rogue ? s->engine.my_sid
						   : s->engine.my_sid;
			r->peer_view[tx] = s->engine.my_view;
			r->peer_seed[tx] = s->seed;
		}
	}

	/* estimator + engine per node */
	for (int i = 0; i < COUNT; i++) {
		struct inode *n = &N[i];

		if (!n->alive || n->rogue)
			continue;
		if (now_ms < n->next_update_ms)
			continue;
		n->next_update_ms = now_ms + 400;

		struct clock_peer_input cin[MAXN];
		size_t ccount = 0;

		for (int p = 0; p < COUNT; p++) {
			if (p == i)
				continue;
			bool fresh = now_ms - n->last_any_ms[p] < 45000;

			if (n->cpeer[p].phase_count > 0)
				cin[ccount++] = (struct clock_peer_input){
					.peer = &n->cpeer[p], .fresh = fresh };
		}
		clock_estimator_update(&n->clk,
				       &clock_estimator_default_config, cin,
				       ccount, local_us_of(n, truth), 400000);

		/* Engine at exactly 1 Hz, as on the device. The old
		 * `now_ms % 1000 >= 400` gate only lined up with the 400 ms
		 * update every third second, so the engine ticked every
		 * 1.2-2 s and every tick-counted window (join, agree,
		 * demote) ran up to twice as long as in firmware — which
		 * hid the pair deadlock of 2026-09-29. */
		if (now_ms < n->next_engine_ms)
			continue;
		n->next_engine_ms = now_ms + 1000;
		struct sync_peer sp[MAXN];
		int scount = 0;
		int map[MAXN];

		for (int p = 0; p < COUNT; p++) {
			if (p == i || now_ms - n->last_any_ms[p] > 90000)
				continue;
			map[scount] = p;
			sp[scount++] = (struct sync_peer){
				.node_id = N[p].node_id,
				.seed = n->peer_seed[p],
				.small_id = n->peer_sid[p],
				.view = n->peer_view[p],
				.fresh = now_ms - n->last_any_ms[p] < 45000,
				.direct = now_ms - n->last_direct_ms[p] < 12000,
				.age_ms = now_ms - n->last_any_ms[p],
				.delta_us = n->cpeer[p].last_delta_us,
			};
		}
		struct group_engine_events ev;

		group_engine_tick(&n->engine, now_ms, sp, scount, NULL, &ev);
		if (ev.ticked)
			for (int k = 0; k < scount; k++)
				if (n->engine.peer_weight[k] >= 0.f)
					clock_peer_estimator_set_weight(
						&n->cpeer[map[k]],
						n->engine.peer_weight[k]);
	}
}

static void irun_s(int secs)
{
	for (int t = 0; t < secs * 1000; t++)
		istep_ms();
}

static int ilit(int lo, int hi)
{
	int lit = 0;

	for (int i = lo; i <= hi && i < COUNT; i++)
		if (N[i].alive && !N[i].rogue && group_engine_lit(&N[i].engine))
			lit++;
	return lit;
}

/* I1: cold newcomer joins a converged crowd — does the crowd dip dark? */
static void i1_join_lock_dip(void)
{
	printf("\n[I1] cold-clock newcomer joins converged 5-crowd\n");
	iinit(6, 0xD100);
	N[5].alive = false;
	irun_s(120);
	printf("  pre: lit=%d size=%d worst=%lldms\n", ilit(0, 4),
	       group_engine_member_count(&N[0].engine),
	       (long long)(N[0].engine.lock_worst_us / 1000));
	N[5].alive = true;
	N[5].offset_us = 700000;   /* wildly cold clock */
	iboot(&N[5]);
	int dark = 0, six_at = -1;

	for (int t = 0; t < 300; t++) {
		irun_s(1);
		if (ilit(0, 4) == 0)
			dark++;
		if (six_at < 0 && ilit(0, 5) == 6 &&
		    group_engine_member_count(&N[0].engine) == 6)
			six_at = t;
	}
	printf("  after join: incumbent-all-dark seconds=%d  six-lit at=%s\n",
	       dark, six_at >= 0 ? ({ static char b[16]; sprintf(b, "%ds", six_at); b; }) : "NEVER(300s)");
}

/* I2: member clock drifts beyond compensation — demotion continuity */
static void i2_drifting_member(void)
{
	printf("\n[I2] member crystal drifts +9000ppm (beyond h clamp)\n");
	iinit(5, 0xD200);
	irun_s(120);
	printf("  pre: lit=%d size=%d\n", ilit(0, 4),
	       group_engine_member_count(&N[0].engine));
	N[4].ppm += 9000.0;
	int dark = 0, four_at = -1, back_at = -1;

	for (int t = 0; t < 400; t++) {
		irun_s(1);
		if (ilit(0, 3) == 0)
			dark++;
		if (four_at < 0 && ilit(0, 3) == 4 &&
		    group_engine_member_count(&N[0].engine) == 4)
			four_at = t;
	}
	printf("  healthy4 dark seconds=%d  re-minted-as-4 at=%s  drifter lit=%d\n",
	       dark, four_at >= 0 ? ({ static char b[16]; sprintf(b, "%ds", four_at); b; }) : "NEVER(400s)",
	       group_engine_lit(&N[4].engine));
}

/* I3: sub-threshold liar (inside the 250ms outlier bound) */
static void i3_subthreshold_liar(void)
{
	printf("\n[I3] rogue lying by 150ms (inside outlier_reject)\n");
	iinit(6, 0xD300);
	irun_s(120);
	double anchor = 0;
	int cnt = 0;

	for (int i = 0; i < 5; i++) {
		anchor += clock_estimator_global_at(&N[i].clk,
			local_us_of(&N[i], now_ms * 1000)) - now_ms * 1000.0;
		cnt++;
	}
	anchor /= cnt;
	N[5].rogue = true;
	N[5].lie_us = 150000;
	irun_s(300);
	double after = 0;

	for (int i = 0; i < 5; i++)
		after += clock_estimator_global_at(&N[i].clk,
			local_us_of(&N[i], now_ms * 1000)) - now_ms * 1000.0;
	after /= cnt;
	printf("  honest crowd moved %.1fms over 5min (toward +150ms lie); lit=%d worst=%lldms\n",
	       after - anchor, ilit(0, 4),
	       (long long)(N[0].engine.lock_worst_us / 1000));
}

/* I4: coordinated liar count boundary */
static void i4_rogue_boundary(void)
{
	printf("\n[I4] coordinated 10s liars among 6 (capture boundary)\n");
	for (int rogues = 2; rogues <= 3; rogues++) {
		iinit(6, 0xD400 + rogues);
		irun_s(90);
		for (int r = 0; r < rogues; r++) {
			N[5 - r].rogue = true;
			N[5 - r].lie_us = 10000000;
		}
		irun_s(240);
		double off = 0;
		int honest = 6 - rogues;

		for (int i = 0; i < honest; i++)
			off += clock_estimator_global_at(&N[i].clk,
				local_us_of(&N[i], now_ms * 1000)) -
				now_ms * 1000.0;
		off /= honest;
		printf("  rogues=%d/6: honest crowd offset=%.0fms (10s lie %s)  lit=%d\n",
		       rogues, off,
		       fabs(off) > 5000000 ? "CAPTURED" : "resisted",
		       ilit(0, honest - 1));
	}
}

/* I5: liars from BOOT — never honest, never reciprocating. This is the
 * actual G1 shape: visibility-membership alone must not grant clock
 * trust, so even a lying MAJORITY of members stays voiceless. (I4's
 * turned incumbents keep their earned trust and can still capture — an
 * inherent bound of leaderless trust, documented in the gap report.) */
static double honest_offset(int count)
{
	double off = 0;

	for (int i = 0; i < count; i++)
		off += clock_estimator_global_at(&N[i].clk,
			local_us_of(&N[i], now_ms * 1000)) - now_ms * 1000.0;
	return off / count / 1000.0;   /* ms */
}

static void i5_stranger_liars_arrive(void)
{
	printf("\n[I5] 3 lying strangers arrive at a converged 5-crowd\n");
	iinit(8, 0xD500);
	for (int r = 5; r < 8; r++)
		N[r].alive = false;
	irun_s(120);
	double before = honest_offset(5);

	/* liars from boot: never reciprocal, so membership-by-visibility is
	 * all the trust they can ever earn */
	for (int r = 5; r < 8; r++) {
		N[r].alive = true;
		N[r].rogue = true;
		N[r].lie_us = 10000000;
		iboot(&N[r]);
	}
	irun_s(300);
	double pull = honest_offset(5) - before;

	printf("  5min pull toward +10s lie: %.1fms (%s)  lit=%d size=%d\n",
	       pull, fabs(pull) > 5000.0 ? "CAPTURED" : "resisted",
	       ilit(0, 4), group_engine_member_count(&N[0].engine));

	/* yank test: the liars jump their story by another 10 s — an
	 * established crowd must not follow */
	before = honest_offset(5);
	for (int r = 5; r < 8; r++)
		N[r].lie_us = 20000000;
	irun_s(120);
	printf("  yank +10s more: crowd moved %.1fms in 2min (%s)\n",
	       honest_offset(5) - before,
	       fabs(honest_offset(5) - before) > 1000.0 ? "FOLLOWED"
							: "held");
}

/* I6: member whose estimator dies (broadcast clock runs away at raw
 * crystal rate, no self-correction) — the sustained-bad-timing shape
 * chronic-demotion escalation (G4) exists for. The crowd should demote,
 * then EXPEL it, and settle as a stable smaller scene. */
static void i6_dead_estimator_member(void)
{
	printf("\n[I6] member estimator dies; drift 20ms/s uncorrected\n");
	iinit(5, 0xD600);
	irun_s(120);
	printf("  pre: lit=%d size=%d\n", ilit(0, 4),
	       group_engine_member_count(&N[0].engine));
	/* model the fault as a rogue whose lie grows at 20 ms/s */
	N[4].rogue = true;
	N[4].lie_us = 0;
	int four_at = -1, dark = 0;

	for (int t = 0; t < 900; t++) {
		N[4].lie_us += 20000;
		irun_s(1);
		if (ilit(0, 3) == 0)
			dark++;
		if (four_at < 0 &&
		    group_engine_member_count(&N[0].engine) == 4 &&
		    ilit(0, 3) == 4)
			four_at = t;
	}
	printf("  healthy dark=%ds  settled-as-4 at=%s\n", dark,
	       four_at >= 0 ? ({ static char b[16]; sprintf(b, "%ds", four_at); b; }) : "NEVER(900s)");
}

int main(void)
{
	i1_join_lock_dip();
	i2_drifting_member();
	i3_subthreshold_liar();
	i4_rogue_boundary();
	i5_stranger_liars_arrive();
	i6_dead_estimator_member();
	return 0;
}
