#define main cohesion_suite_main
#include "cohesion_sim_test.c"
#undef main

int main(void)
{
	/* G2: pure-silence departure timing at an observer */
	struct sim s;

	sim_init(&s, 3, 0x6001);
	sim_run(&s, 90);
	printf("[G2] members before=%d\n",
	       group_engine_member_count(&s.n[0].engine));
	s.n[2].alive = false;
	int t = 0;

	while (t < 200 && (s.n[0].engine.my_view &
			   (1u << s.n[2].engine.my_sid))) {
		sim_second(&s);
		t++;
	}
	printf("[G2] silent member left view at %ds (want ~57, was 31)\n", t);

	/* G6: flapper up 3s per 25s — should be expelled and stay out */
	sim_init(&s, 4, 0x6002);
	sim_run(&s, 90);
	int member_secs = 0, last_member = -1;

	for (t = 0; t < 600; t++) {
		s.n[3].alive = (t % 25) < 3;
		sim_second(&s);
		bool mem = false;

		for (int i = 0; i < SYNC_MAX_PEERS; i++)
			if (s.n[0].engine.peers[i].used &&
			    s.n[0].engine.peers[i].node_id ==
				    s.n[3].node_id &&
			    s.n[0].engine.peers[i].presence.membership.state ==
				    GROUP_MEMBER)
				mem = true;
		if (mem) {
			member_secs++;
			last_member = t;
		}
	}
	printf("[G6] flapper member %ds of 600, last member at t=%d (was member ~all 600)\n",
	       member_secs, last_member);
	printf("[G6] core lit=%d size=%d\n", lit_count(&s, 0, 2),
	       group_engine_member_count(&s.n[0].engine));
	return 0;
}
