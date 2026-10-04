#define main integrated_main
#include "integrated.c"
#undef main
int main(void)
{
	printf("[I2b] chronic-drift member: scene churn quantified (600s)\n");
	iinit(5, 0xD2B0);
	irun_s(120);
	N[4].ppm += 9000.0;
	int mints = 0, size_changes = 0, last = group_engine_member_count(&N[0].engine);

	for (int t = 0; t < 600; t++) {
		for (int ms = 0; ms < 1000; ms++)
			istep_ms();
		/* count mints via seed change at observer */
		static uint32_t last_seed;
		uint32_t seed = N[0].engine.group_seed_cur;

		if (seed != last_seed) {
			mints++;
			last_seed = seed;
		}
		int m = group_engine_member_count(&N[0].engine);

		if (m != last) {
			size_changes++;
			last = m;
		}
	}
	printf("  over 600s: scene seed changes=%d size flips=%d (steady would be ~10 mints)\n",
	       mints, size_changes);
	return 0;
}
