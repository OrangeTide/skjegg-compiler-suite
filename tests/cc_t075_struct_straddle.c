/* cc_t075_struct_straddle.c : a 2-eightbyte struct arg after enough scalars
   that its slots do not all fit in registers must go WHOLLY to memory, not
   split (SysV / AAPCS64 all-or-nothing).  psABI targets only. */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */
struct TS { long a; long b; };
long strad(long p1,long p2,long p3,long p4,long p5,struct TS t){
    return p1*100000+p2*10000+p3*1000+p4*100+p5*10+t.a-t.b;
}
int main(void){ struct TS t={7,3}; return (strad(1,2,3,4,5,t)==123454)?42:1; }
