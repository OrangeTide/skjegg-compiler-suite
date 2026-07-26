/* cc_t070_float_across_call.c : a float temp live across an xmm-clobbering call */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */
/* g uses several xmm registers, so it clobbers the caller's live float temp */
int g(int c) {
    double t0=c+0.5, t1=c+1.5, t2=c+2.5, t3=c+3.5, t4=c+4.5, t5=c+5.5, t6=c+6.5;
    return (int)(t0+t1+t2+t3+t4+t5+t6);
}
double h(double a, double b, int c) {
    return a * b + (double)g(c);   /* a*b must survive g clobbering xmm */
}
int main(void) {
    double r = h(6.0, 7.0, 0);   /* a*b=42; g(0)= 0.5+1.5+..+6.5 = 24.5 -> 24; 42+24=66 */
    return (int)r;               /* want 66 */
}
