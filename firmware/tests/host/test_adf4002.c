/* Host unit test for main/adf4002.h. Expected words are derived by hand from the
 * ADF4002 data sheet Rev. C latch maps (Figures 15-19). */
#include <stdio.h>
#include "../../main/adf4002.h"

static int fails;
static void eq(const char *name, uint32_t got, uint32_t want)
{
    if (got != want) {
        printf("FAIL %s: got 0x%06X want 0x%06X\n", name, (unsigned)got, (unsigned)want);
        fails++;
    } else {
        printf("ok   %s = 0x%06X\n", name, (unsigned)got);
    }
}

int main(void)
{
    /* R latch: R<<2, LDP = DB20, control 00 */
    eq("r_latch(100, ldp5)", adf4002_r_latch(100, true), 0x100190);
    eq("r_latch(100, ldp3)", adf4002_r_latch(100, false), 0x000190);
    eq("r_latch(16383)", adf4002_r_latch(16383, false), 0x00FFFC);
    eq("r_latch reserved DB22..21", adf4002_r_latch(0xFFFF, true) & 0x600000, 0);

    /* N latch: N<<8 | 1, G1 = DB21 */
    eq("n_latch(760, G1=1)", adf4002_n_latch(760, true), 0x22F801);
    eq("n_latch(920, G1=1)", adf4002_n_latch(920, true), 0x239801);
    eq("n_latch(920, G1=0)", adf4002_n_latch(920, false), 0x039801);
    eq("n_latch(1080, G1=0)", adf4002_n_latch(1080, false), 0x043801);

    /* Function latch: CS1 625uA, CS2 2.5mA, PD negative, MUXOUT = digital lock detect */
    adf4002_function_t f = {ADF_CP_0MA625, ADF_CP_2MA5, false, false, ADF_MUX_DIGITAL_LOCK_DETECT};
    eq("function run", adf4002_function_latch(&f), 0x0C0012);
    eq("init run", adf4002_init_latch(&f), 0x0C0013);
    f.cp_three_state = true;
    eq("function CP three-state", adf4002_function_latch(&f), 0x0C0112);
    eq("init CP three-state", adf4002_init_latch(&f), 0x0C0113);
    f.cp_three_state = false;
    f.pd_positive = true;
    eq("function PD positive", adf4002_function_latch(&f), 0x0C0092);
    adf4002_function_t all = {ADF_CP_5MA, ADF_CP_5MA, true, true, 7};
    eq("no PD1/PD2/F1/F4/F5/TC bits", adf4002_function_latch(&all) & 0x207E0C, 0);

    /* Compensation bank index */
    eq("kidx Kv20 N760 (max)", fm_comp_kidx(760, 2000), 127);
    eq("kidx Kv10 N1080 (min)", fm_comp_kidx(1080, 1000), 0);
    eq("kidx Kv15 N920", fm_comp_kidx(920, 1500), 69);
    eq("kidx Kv15 N760", fm_comp_kidx(760, 1500), 92);
    eq("kidx Kv15 N1080", fm_comp_kidx(1080, 1500), 49);
    eq("kidx clamp low", fm_comp_kidx(1080, 500), 0);
    eq("kidx clamp high", fm_comp_kidx(760, 3000), 127);

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
