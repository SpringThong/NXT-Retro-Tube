/* Keep the program on the C library the handhelds ship (glibc 2.28 and older):
 * glibc 2.29 gave exp/exp2/log/pow new versions. Every call (ours and FFmpeg's) is routed through
 * -Wl,--wrap=… to the aarch64 originals (GLIBC_2.17), which exist on every device.
 * SPDX-License-Identifier: GPL-3.0-only */
double exp_old(double), exp2_old(double), log_old(double), pow_old(double, double);
__asm__(".symver exp_old,exp@GLIBC_2.17");
__asm__(".symver exp2_old,exp2@GLIBC_2.17");
__asm__(".symver log_old,log@GLIBC_2.17");
__asm__(".symver pow_old,pow@GLIBC_2.17");
double __wrap_exp(double x) { return exp_old(x); }
double __wrap_exp2(double x) { return exp2_old(x); }
double __wrap_log(double x) { return log_old(x); }
double __wrap_pow(double x, double y) { return pow_old(x, y); }
