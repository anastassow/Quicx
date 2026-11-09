#include "pmad_layout.h"

/*
The default PMAD layout. Every size class from ALIGNMENT up to
MAX_SIZE_OF_SIZE_CLASS is present, so nothing quicx or a client can ask
for gets rounded up into a wider class — every request is an exact fit.

Percentages split bytes, not blocks, and a block costs its payload plus
one BlockHeader. So the same 1% buys 2,620 blocks at 16 B and only 80 at
1024 B, which is why the split is not flat:

    64 B   12%   Task. the largest share by far — there is no cap on
                 queue depth, so this class is the queue depth
    16 B    2%   Connection. bounded by MAX_CONNECTIONS
    32 B    2%   Worker. bounded by the connections that register as one
    rest   1-2%  payload. every class keeps a 1% floor (at 0% a class is
                 carved zero blocks and every alloc of that size fails),
                 and the leftover points go to the widest classes, which
                 is what lifts the worst-off class the most

At the default pool that leaves no payload class below 124 blocks. To
tune it for a workload, raise the classes you know you send and take the
points back off the ones you do not — the total must stay at 100.
*/
void pmad_layout_default(PmadLayout* c) {
    c->class_count = MAX_PMAD_CLASSES;

    c->sizes[ 0] =   16;  c->percentages[ 0] =  2;   /* Connection */
    c->sizes[ 1] =   32;  c->percentages[ 1] =  2;   /* Worker     */
    c->sizes[ 2] =   48;  c->percentages[ 2] =  1;
    c->sizes[ 3] =   64;  c->percentages[ 3] = 12;   /* Task  <- the queue depth */
    c->sizes[ 4] =   80;  c->percentages[ 4] =  1;
    c->sizes[ 5] =   96;  c->percentages[ 5] =  1;
    c->sizes[ 6] =  112;  c->percentages[ 6] =  1;
    c->sizes[ 7] =  128;  c->percentages[ 7] =  1;
    c->sizes[ 8] =  144;  c->percentages[ 8] =  1;
    c->sizes[ 9] =  160;  c->percentages[ 9] =  1;
    c->sizes[10] =  176;  c->percentages[10] =  1;
    c->sizes[11] =  192;  c->percentages[11] =  1;
    c->sizes[12] =  208;  c->percentages[12] =  1;
    c->sizes[13] =  224;  c->percentages[13] =  1;
    c->sizes[14] =  240;  c->percentages[14] =  1;
    c->sizes[15] =  256;  c->percentages[15] =  1;
    c->sizes[16] =  272;  c->percentages[16] =  1;
    c->sizes[17] =  288;  c->percentages[17] =  1;
    c->sizes[18] =  304;  c->percentages[18] =  1;
    c->sizes[19] =  320;  c->percentages[19] =  1;
    c->sizes[20] =  336;  c->percentages[20] =  1;
    c->sizes[21] =  352;  c->percentages[21] =  1;
    c->sizes[22] =  368;  c->percentages[22] =  1;
    c->sizes[23] =  384;  c->percentages[23] =  1;
    c->sizes[24] =  400;  c->percentages[24] =  1;
    c->sizes[25] =  416;  c->percentages[25] =  1;
    c->sizes[26] =  432;  c->percentages[26] =  1;
    c->sizes[27] =  448;  c->percentages[27] =  1;
    c->sizes[28] =  464;  c->percentages[28] =  1;
    c->sizes[29] =  480;  c->percentages[29] =  1;
    c->sizes[30] =  496;  c->percentages[30] =  1;
    c->sizes[31] =  512;  c->percentages[31] =  1;
    c->sizes[32] =  528;  c->percentages[32] =  1;
    c->sizes[33] =  544;  c->percentages[33] =  1;
    c->sizes[34] =  560;  c->percentages[34] =  1;
    c->sizes[35] =  576;  c->percentages[35] =  1;
    c->sizes[36] =  592;  c->percentages[36] =  1;
    c->sizes[37] =  608;  c->percentages[37] =  1;
    c->sizes[38] =  624;  c->percentages[38] =  1;
    c->sizes[39] =  640;  c->percentages[39] =  1;
    c->sizes[40] =  656;  c->percentages[40] =  1;
    c->sizes[41] =  672;  c->percentages[41] =  2;
    c->sizes[42] =  688;  c->percentages[42] =  2;
    c->sizes[43] =  704;  c->percentages[43] =  2;
    c->sizes[44] =  720;  c->percentages[44] =  2;
    c->sizes[45] =  736;  c->percentages[45] =  2;
    c->sizes[46] =  752;  c->percentages[46] =  2;
    c->sizes[47] =  768;  c->percentages[47] =  2;
    c->sizes[48] =  784;  c->percentages[48] =  2;
    c->sizes[49] =  800;  c->percentages[49] =  2;
    c->sizes[50] =  816;  c->percentages[50] =  2;
    c->sizes[51] =  832;  c->percentages[51] =  2;
    c->sizes[52] =  848;  c->percentages[52] =  2;
    c->sizes[53] =  864;  c->percentages[53] =  2;
    c->sizes[54] =  880;  c->percentages[54] =  2;
    c->sizes[55] =  896;  c->percentages[55] =  2;
    c->sizes[56] =  912;  c->percentages[56] =  2;
    c->sizes[57] =  928;  c->percentages[57] =  2;
    c->sizes[58] =  944;  c->percentages[58] =  2;
    c->sizes[59] =  960;  c->percentages[59] =  2;
    c->sizes[60] =  976;  c->percentages[60] =  2;
    c->sizes[61] =  992;  c->percentages[61] =  2;
    c->sizes[62] = 1008;  c->percentages[62] =  2;
    c->sizes[63] = 1024;  c->percentages[63] =  2;
}
