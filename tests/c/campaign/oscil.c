#define main integrated_main
#include "integrated.c"
#undef main
int main(void)
{
	/* dusty model: 6th member's clock error square-waves so its median
	 * hovers across the 80 ms bar. Old firmware: never demoted, crowd
	 * gated forever. Want: demoted and STAYS out; crowd locks. */
	iinit(6, 0xDD01);
	irun_s(120);
	printf("pre: lit=%d\n", ilit(0, 5));
	N[5].rogue = true;
	N[5].lie_us = 30000;
	int lit5 = 0, drop_at = -1;

	for (int t = 0; t < 400; t++) {
		N[5].lie_us = (t / 10) % 2 ? 130000 : 30000;
		irun_s(1);
		if (ilit(0, 4) == 5)
			lit5++;
		bool in_view = (N[0].engine.my_view >>
				N[5].engine.my_sid) & 1U;

		if (drop_at < 0 && !in_view)
			drop_at = t;
	}
	printf("oscillator dropped from view at=%s  healthy5 lit %d/400s\n",
	       drop_at >= 0 ? ({ static char b[16]; sprintf(b, "%ds", drop_at); b; }) : "NEVER",
	       lit5);
	return 0;
}
