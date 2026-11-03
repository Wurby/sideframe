// disctest — prints the Sideframe Macs found on the network for a few seconds. gcc -O2 -I../src -o disctest disctest.c
#define _GNU_SOURCE
#include "../src/discovery.h"
int main(void) {
    double t0 = disc_now(), last = 0;
    while (disc_now() - t0 < 7) {
        disc_poll(); usleep(50000);
        if (disc_now() - last > 1.5) { last = disc_now(); int n = 0; for (int k = 0; k < DISC_MAX; k++) if (disc_macs[k].used) { n++;
            printf("[%.1fs] found: \"%s\"  id %s  %s:%d  host %s\n", disc_now() - t0, disc_macs[k].name, disc_macs[k].id, disc_macs[k].ip, disc_macs[k].port, disc_macs[k].host); }
            if (!n) printf("[%.1fs] (none yet)\n", disc_now() - t0); }
    }
    return 0;
}
