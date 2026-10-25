// Tables and the 32-point transform of an MPEG audio layer II decoder.
//
// Taken from PL_MPEG by Dominic Szablewski (https://github.com/phoboslab/pl_mpeg), MIT license:
// the numbers come from ISO/IEC 11172-3 and 13818-3 and the transform is kjmp2 by Martin J.
// Fiedler, as adapted there. The decoder around them (ahx.cpp) is this project's own.
//
// MIT License
//
// Copyright (c) 2019 Dominic Szablewski
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

void plm_audio_idct36(int s[32][3], int ss, float *d, int dp) {
	float t01, t02, t03, t04, t05, t06, t07, t08, t09, t10, t11, t12,
		t13, t14, t15, t16, t17, t18, t19, t20, t21, t22, t23, t24,
		t25, t26, t27, t28, t29, t30, t31, t32, t33;

	t01 = (float)(s[0][ss] + s[31][ss]); t02 = (float)(s[0][ss] - s[31][ss]) * 0.500602998235f;
	t03 = (float)(s[1][ss] + s[30][ss]); t04 = (float)(s[1][ss] - s[30][ss]) * 0.505470959898f;
	t05 = (float)(s[2][ss] + s[29][ss]); t06 = (float)(s[2][ss] - s[29][ss]) * 0.515447309923f;
	t07 = (float)(s[3][ss] + s[28][ss]); t08 = (float)(s[3][ss] - s[28][ss]) * 0.53104259109f;
	t09 = (float)(s[4][ss] + s[27][ss]); t10 = (float)(s[4][ss] - s[27][ss]) * 0.553103896034f;
	t11 = (float)(s[5][ss] + s[26][ss]); t12 = (float)(s[5][ss] - s[26][ss]) * 0.582934968206f;
	t13 = (float)(s[6][ss] + s[25][ss]); t14 = (float)(s[6][ss] - s[25][ss]) * 0.622504123036f;
	t15 = (float)(s[7][ss] + s[24][ss]); t16 = (float)(s[7][ss] - s[24][ss]) * 0.674808341455f;
	t17 = (float)(s[8][ss] + s[23][ss]); t18 = (float)(s[8][ss] - s[23][ss]) * 0.744536271002f;
	t19 = (float)(s[9][ss] + s[22][ss]); t20 = (float)(s[9][ss] - s[22][ss]) * 0.839349645416f;
	t21 = (float)(s[10][ss] + s[21][ss]); t22 = (float)(s[10][ss] - s[21][ss]) * 0.972568237862f;
	t23 = (float)(s[11][ss] + s[20][ss]); t24 = (float)(s[11][ss] - s[20][ss]) * 1.16943993343f;
	t25 = (float)(s[12][ss] + s[19][ss]); t26 = (float)(s[12][ss] - s[19][ss]) * 1.48416461631f;
	t27 = (float)(s[13][ss] + s[18][ss]); t28 = (float)(s[13][ss] - s[18][ss]) * 2.05778100995f;
	t29 = (float)(s[14][ss] + s[17][ss]); t30 = (float)(s[14][ss] - s[17][ss]) * 3.40760841847f;
	t31 = (float)(s[15][ss] + s[16][ss]); t32 = (float)(s[15][ss] - s[16][ss]) * 10.1900081235f;

	t33 = t01 + t31; t31 = (t01 - t31) * 0.502419286188f;
	t01 = t03 + t29; t29 = (t03 - t29) * 0.52249861494f;
	t03 = t05 + t27; t27 = (t05 - t27) * 0.566944034816f;
	t05 = t07 + t25; t25 = (t07 - t25) * 0.64682178336f;
	t07 = t09 + t23; t23 = (t09 - t23) * 0.788154623451f;
	t09 = t11 + t21; t21 = (t11 - t21) * 1.06067768599f;
	t11 = t13 + t19; t19 = (t13 - t19) * 1.72244709824f;
	t13 = t15 + t17; t17 = (t15 - t17) * 5.10114861869f;
	t15 = t33 + t13; t13 = (t33 - t13) * 0.509795579104f;
	t33 = t01 + t11; t01 = (t01 - t11) * 0.601344886935f;
	t11 = t03 + t09; t09 = (t03 - t09) * 0.899976223136f;
	t03 = t05 + t07; t07 = (t05 - t07) * 2.56291544774f;
	t05 = t15 + t03; t15 = (t15 - t03) * 0.541196100146f;
	t03 = t33 + t11; t11 = (t33 - t11) * 1.30656296488f;
	t33 = t05 + t03; t05 = (t05 - t03) * 0.707106781187f;
	t03 = t15 + t11; t15 = (t15 - t11) * 0.707106781187f;
	t03 += t15;
	t11 = t13 + t07; t13 = (t13 - t07) * 0.541196100146f;
	t07 = t01 + t09; t09 = (t01 - t09) * 1.30656296488f;
	t01 = t11 + t07; t07 = (t11 - t07) * 0.707106781187f;
	t11 = t13 + t09; t13 = (t13 - t09) * 0.707106781187f;
	t11 += t13; t01 += t11;
	t11 += t07; t07 += t13;
	t09 = t31 + t17; t31 = (t31 - t17) * 0.509795579104f;
	t17 = t29 + t19; t29 = (t29 - t19) * 0.601344886935f;
	t19 = t27 + t21; t21 = (t27 - t21) * 0.899976223136f;
	t27 = t25 + t23; t23 = (t25 - t23) * 2.56291544774f;
	t25 = t09 + t27; t09 = (t09 - t27) * 0.541196100146f;
	t27 = t17 + t19; t19 = (t17 - t19) * 1.30656296488f;
	t17 = t25 + t27; t27 = (t25 - t27) * 0.707106781187f;
	t25 = t09 + t19; t19 = (t09 - t19) * 0.707106781187f;
	t25 += t19;
	t09 = t31 + t23; t31 = (t31 - t23) * 0.541196100146f;
	t23 = t29 + t21; t21 = (t29 - t21) * 1.30656296488f;
	t29 = t09 + t23; t23 = (t09 - t23) * 0.707106781187f;
	t09 = t31 + t21; t31 = (t31 - t21) * 0.707106781187f;
	t09 += t31;	t29 += t09;	t09 += t23;	t23 += t31;
	t17 += t29;	t29 += t25;	t25 += t09;	t09 += t27;
	t27 += t23;	t23 += t19; t19 += t31;
	t21 = t02 + t32; t02 = (t02 - t32) * 0.502419286188f;
	t32 = t04 + t30; t04 = (t04 - t30) * 0.52249861494f;
	t30 = t06 + t28; t28 = (t06 - t28) * 0.566944034816f;
	t06 = t08 + t26; t08 = (t08 - t26) * 0.64682178336f;
	t26 = t10 + t24; t10 = (t10 - t24) * 0.788154623451f;
	t24 = t12 + t22; t22 = (t12 - t22) * 1.06067768599f;
	t12 = t14 + t20; t20 = (t14 - t20) * 1.72244709824f;
	t14 = t16 + t18; t16 = (t16 - t18) * 5.10114861869f;
	t18 = t21 + t14; t14 = (t21 - t14) * 0.509795579104f;
	t21 = t32 + t12; t32 = (t32 - t12) * 0.601344886935f;
	t12 = t30 + t24; t24 = (t30 - t24) * 0.899976223136f;
	t30 = t06 + t26; t26 = (t06 - t26) * 2.56291544774f;
	t06 = t18 + t30; t18 = (t18 - t30) * 0.541196100146f;
	t30 = t21 + t12; t12 = (t21 - t12) * 1.30656296488f;
	t21 = t06 + t30; t30 = (t06 - t30) * 0.707106781187f;
	t06 = t18 + t12; t12 = (t18 - t12) * 0.707106781187f;
	t06 += t12;
	t18 = t14 + t26; t26 = (t14 - t26) * 0.541196100146f;
	t14 = t32 + t24; t24 = (t32 - t24) * 1.30656296488f;
	t32 = t18 + t14; t14 = (t18 - t14) * 0.707106781187f;
	t18 = t26 + t24; t24 = (t26 - t24) * 0.707106781187f;
	t18 += t24; t32 += t18;
	t18 += t14; t26 = t14 + t24;
	t14 = t02 + t16; t02 = (t02 - t16) * 0.509795579104f;
	t16 = t04 + t20; t04 = (t04 - t20) * 0.601344886935f;
	t20 = t28 + t22; t22 = (t28 - t22) * 0.899976223136f;
	t28 = t08 + t10; t10 = (t08 - t10) * 2.56291544774f;
	t08 = t14 + t28; t14 = (t14 - t28) * 0.541196100146f;
	t28 = t16 + t20; t20 = (t16 - t20) * 1.30656296488f;
	t16 = t08 + t28; t28 = (t08 - t28) * 0.707106781187f;
	t08 = t14 + t20; t20 = (t14 - t20) * 0.707106781187f;
	t08 += t20;
	t14 = t02 + t10; t02 = (t02 - t10) * 0.541196100146f;
	t10 = t04 + t22; t22 = (t04 - t22) * 1.30656296488f;
	t04 = t14 + t10; t10 = (t14 - t10) * 0.707106781187f;
	t14 = t02 + t22; t02 = (t02 - t22) * 0.707106781187f;
	t14 += t02;	t04 += t14;	t14 += t10;	t10 += t02;
	t16 += t04;	t04 += t08;	t08 += t14;	t14 += t28;
	t28 += t10;	t10 += t20;	t20 += t02;	t21 += t16;
	t16 += t32;	t32 += t04;	t04 += t06;	t06 += t08;
	t08 += t18;	t18 += t14;	t14 += t30;	t30 += t28;
	t28 += t26;	t26 += t10;	t10 += t12;	t12 += t20;
	t20 += t24;	t24 += t02;

	d[dp + 48] = -t33;
	d[dp + 49] = d[dp + 47] = -t21;
	d[dp + 50] = d[dp + 46] = -t17;
	d[dp + 51] = d[dp + 45] = -t16;
	d[dp + 52] = d[dp + 44] = -t01;
	d[dp + 53] = d[dp + 43] = -t32;
	d[dp + 54] = d[dp + 42] = -t29;
	d[dp + 55] = d[dp + 41] = -t04;
	d[dp + 56] = d[dp + 40] = -t03;
	d[dp + 57] = d[dp + 39] = -t06;
	d[dp + 58] = d[dp + 38] = -t25;
	d[dp + 59] = d[dp + 37] = -t08;
	d[dp + 60] = d[dp + 36] = -t11;
	d[dp + 61] = d[dp + 35] = -t18;
	d[dp + 62] = d[dp + 34] = -t09;
	d[dp + 63] = d[dp + 33] = -t14;
	d[dp + 32] = -t05;
	d[dp + 0] = t05; d[dp + 31] = -t30;
	d[dp + 1] = t30; d[dp + 30] = -t27;
	d[dp + 2] = t27; d[dp + 29] = -t28;
	d[dp + 3] = t28; d[dp + 28] = -t07;
	d[dp + 4] = t07; d[dp + 27] = -t26;
	d[dp + 5] = t26; d[dp + 26] = -t23;
	d[dp + 6] = t23; d[dp + 25] = -t10;
	d[dp + 7] = t10; d[dp + 24] = -t15;
	d[dp + 8] = t15; d[dp + 23] = -t12;
	d[dp + 9] = t12; d[dp + 22] = -t19;
	d[dp + 10] = t19; d[dp + 21] = -t20;
	d[dp + 11] = t20; d[dp + 20] = -t13;
	d[dp + 12] = t13; d[dp + 19] = -t24;
	d[dp + 13] = t24; d[dp + 18] = -t31;
	d[dp + 14] = t31; d[dp + 17] = -t02;
	d[dp + 15] = t02; d[dp + 16] = 0.0;
}
