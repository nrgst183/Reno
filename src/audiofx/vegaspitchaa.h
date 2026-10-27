// Pitch Shift's anti-alias filter (sfppack1_x64.dll, CSfPitchs): used when the pitch goes up by more than 1.1 times and
// "Apply an anti-alias filter during pitch shift" is ticked. A low-pass filter of eight second-order sections is made
// from a built-in prototype (its poles) for the cut-off 0.5 / ratio, and every input block is filtered before it is
// resampled.
//
// The design routine (two functions, about 2000 instructions of complex-number arithmetic: the plug-in's own code at
// RVA 0x1aed0 and 0x19570) is carried over instruction by instruction, with the SSE2 registers, the flags and the stack
// as plain variables, so that every double comes out exactly as the DLL makes it. It was produced mechanically from the
// DLL's code; do not edit it by hand. The per-sample section (RVA 0x1b670 / 0x1bc90) is written out below.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace reno {
namespace vegpack1 {
namespace aa {

union XR {
    double d[2];
    uint64_t u[2];
};
struct Flags {
    bool zf = false, sf = false, cf = false, of = false, pf = false;
    // kind: 0 logic, 1 add, 2 sub (or compare), 3 inc, 4 shift; w: width in bits; r: the result; a, b: the operands
    void set(int kind, int w, uint64_t r, uint64_t a, uint64_t b)
    {
        const uint64_t m = w == 64 ? ~0ull : ((1ull << w) - 1);
        const uint64_t top = 1ull << (w - 1);
        r &= m;
        a &= m;
        b &= m;
        zf = r == 0;
        sf = (r & top) != 0;
        pf = false;
        if (kind == 1 || kind == 3) {
            if (kind == 1) cf = r < a;
            of = ((a ^ r) & (b ^ r) & top) != 0;
        } else if (kind == 2) {
            cf = a < b;
            of = ((a ^ b) & (a ^ r) & top) != 0;
        } else {
            if (kind == 0) cf = false;
            of = false;
        }
    }
    void ucom(double a, double b)
    {
        of = sf = false;
        if (a != a || b != b) zf = pf = cf = true;
        else if (a > b) zf = pf = cf = false;
        else if (a < b) { zf = pf = false; cf = true; }
        else { zf = true; pf = cf = false; }
    }
};
struct Regs {
    uint64_t g[16];
    XR x[16];
    Flags f;
};

inline uint64_t RD64(uint64_t a) { uint64_t v; std::memcpy(&v, (const void*)(uintptr_t)a, 8); return v; }
inline uint32_t RD32(uint64_t a) { uint32_t v; std::memcpy(&v, (const void*)(uintptr_t)a, 4); return v; }
inline uint16_t RD16(uint64_t a) { uint16_t v; std::memcpy(&v, (const void*)(uintptr_t)a, 2); return v; }
inline uint8_t RD8(uint64_t a) { return *(const uint8_t*)(uintptr_t)a; }
inline void WR64(uint64_t a, uint64_t v) { std::memcpy((void*)(uintptr_t)a, &v, 8); }
inline void WR32(uint64_t a, uint64_t v) { const uint32_t w = (uint32_t)v; std::memcpy((void*)(uintptr_t)a, &w, 4); }
inline void WR16(uint64_t a, uint64_t v) { const uint16_t w = (uint16_t)v; std::memcpy((void*)(uintptr_t)a, &w, 2); }
inline void WR8(uint64_t a, uint64_t v) { *(uint8_t*)(uintptr_t)a = (uint8_t)v; }
inline double DBL(uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }
inline uint64_t BITS(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }
inline uint64_t KONST(uint64_t u) { return u; }
inline double KD(uint64_t u) { return DBL(u); }

inline void aaDesign19570(Regs& R);

inline void aaDesign19570(Regs& R)
{
    uint64_t* G = R.g; XR* X = R.x; Flags& F = R.f;
    // 180019570: mov rax, rsp
    G[0]=G[4];
    // 180019573: mov dword ptr [rax + 0x10], edx
    WR32((uint64_t)(0 + G[0] + 16ull),(G[2]&0xFFFFFFFFull));
    // 180019576: mov qword ptr [rax + 0x8], rcx
    WR64((uint64_t)(0 + G[0] + 8ull),G[1]);
    // 18001957a: push r12
    G[4]-=8; WR64(G[4],G[12]);
    // 18001957c: sub rsp, 0x1e0
    { uint64_t a=G[4], b=480ull, r=(((G[4])-(480ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[4]=r;
    }
    // 180019583: mov qword ptr [rax + 0x18], rbx
    WR64((uint64_t)(0 + G[0] + 24ull),G[3]);
    // 180019587: mov qword ptr [rax - 0x10], rbp
    WR64((uint64_t)(0 + G[0] - 16ull),G[5]);
    // 18001958b: mov qword ptr [rax - 0x18], rsi
    WR64((uint64_t)(0 + G[0] - 24ull),G[6]);
    // 18001958f: mov qword ptr [rax - 0x20], rdi
    WR64((uint64_t)(0 + G[0] - 32ull),G[7]);
    // 180019593: mov qword ptr [rax - 0x28], r13
    WR64((uint64_t)(0 + G[0] - 40ull),G[13]);
    // 180019597: mov qword ptr [rax - 0x30], r14
    WR64((uint64_t)(0 + G[0] - 48ull),G[14]);
    // 18001959b: mov qword ptr [rax - 0x38], r15
    WR64((uint64_t)(0 + G[0] - 56ull),G[15]);
    // 18001959f: movsxd rbp, r9d
    G[5]=(uint64_t)(int64_t)(int32_t)((G[9]&0xFFFFFFFFull));
    // 1800195a2: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 1800195a7: movaps xmmword ptr [rax - 0x48], xmm6
    WR64((uint64_t)(0 + G[0] - 72ull),X[6].u[0]); WR64((uint64_t)(0 + G[0] - 72ull)+8,X[6].u[1]);
    // 1800195ab: movaps xmmword ptr [rax - 0x58], xmm7
    WR64((uint64_t)(0 + G[0] - 88ull),X[7].u[0]); WR64((uint64_t)(0 + G[0] - 88ull)+8,X[7].u[1]);
    // 1800195af: xorpd xmm7, xmm7
    X[7].u[0]^=X[7].u[0]; X[7].u[1]^=X[7].u[1];
    // 1800195b3: movsd qword ptr [rsp + 0x40], xmm7
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[7].d[0]));
    // 1800195b9: movsd qword ptr [rsp + 0x48], xmm7
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[7].d[0]));
    // 1800195bf: movsd qword ptr [rsp + 0x30], xmm7
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[7].d[0]));
    // 1800195c5: lea r11, [rsp + 0x80]
    G[11]=(uint64_t)(0 + G[4] + 128ull);
    // 1800195cd: movsxd r10, edx
    G[10]=(uint64_t)(int64_t)(int32_t)((G[2]&0xFFFFFFFFull));
    // 1800195d0: mov rdx, rcx
    G[2]=G[1];
    // 1800195d3: movaps xmmword ptr [rax - 0x68], xmm8
    WR64((uint64_t)(0 + G[0] - 104ull),X[8].u[0]); WR64((uint64_t)(0 + G[0] - 104ull)+8,X[8].u[1]);
    // 1800195d8: movaps xmmword ptr [rax - 0xa8], xmm12
    WR64((uint64_t)(0 + G[0] - 168ull),X[12].u[0]); WR64((uint64_t)(0 + G[0] - 168ull)+8,X[12].u[1]);
    // 1800195e0: movaps xmmword ptr [rax - 0xd8], xmm15
    WR64((uint64_t)(0 + G[0] - 216ull),X[15].u[0]); WR64((uint64_t)(0 + G[0] - 216ull)+8,X[15].u[1]);
    // 1800195e8: movsd qword ptr [rsp + 0x38], xmm7
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[7].d[0]));
    // 1800195ee: movsd qword ptr [rsp + 0x20], xmm7
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[7].d[0]));
    // 1800195f4: movsd qword ptr [rsp + 0x28], xmm7
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[7].d[0]));
    // 1800195fa: lea rcx, [rsp + 0x40]
    G[1]=(uint64_t)(0 + G[4] + 64ull);
    // 1800195ff: mov r13, r8
    G[13]=G[8];
    // 180019602: mov r8, qword ptr [rsp + 0x220]
    G[8]=RD64((uint64_t)(0 + G[4] + 544ull));
    // 18001960a: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001960d: mov qword ptr [r8], rax
    WR64((uint64_t)(0 + G[8]),G[0]);
    // 180019610: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 180019614: mov rcx, qword ptr [rsp + 0x228]
    G[1]=RD64((uint64_t)(0 + G[4] + 552ull));
    // 18001961c: movsd qword ptr [rsp + 0x40], xmm7
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[7].d[0]));
    // 180019622: movsd qword ptr [rsp + 0x48], xmm7
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[7].d[0]));
    // 180019628: mov qword ptr [r8 + 0x8], rax
    WR64((uint64_t)(0 + G[8] + 8ull),G[0]);
    // 18001962c: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001962f: lea rdi, [rcx + 0x20]
    G[7]=(uint64_t)(0 + G[1] + 32ull);
    // 180019633: mov qword ptr [rcx], rax
    WR64((uint64_t)(0 + G[1]),G[0]);
    // 180019636: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 18001963a: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 18001963f: movsd qword ptr [rsp + 0x30], xmm7
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[7].d[0]));
    // 180019645: movsd qword ptr [rsp + 0x38], xmm7
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[7].d[0]));
    // 18001964b: mov qword ptr [rcx + 0x8], rax
    WR64((uint64_t)(0 + G[1] + 8ull),G[0]);
    // 18001964f: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 180019652: mov qword ptr [r11], rax
    WR64((uint64_t)(0 + G[11]),G[0]);
    // 180019655: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 180019659: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 18001965e: movsd qword ptr [rsp + 0x20], xmm7
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[7].d[0]));
    // 180019664: movsd qword ptr [rsp + 0x28], xmm7
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[7].d[0]));
    // 18001966a: mov qword ptr [r11 + 0x8], rax
    WR64((uint64_t)(0 + G[11] + 8ull),G[0]);
    // 18001966e: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 180019671: lea r11, [rsp + 0x90]
    G[11]=(uint64_t)(0 + G[4] + 144ull);
    // 180019679: mov qword ptr [r8 + 0x10], rax
    WR64((uint64_t)(0 + G[8] + 16ull),G[0]);
    // 18001967d: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 180019681: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 180019686: movsd qword ptr [rsp + 0x40], xmm7
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[7].d[0]));
    // 18001968c: movsd qword ptr [rsp + 0x48], xmm7
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[7].d[0]));
    // 180019692: mov qword ptr [r8 + 0x18], rax
    WR64((uint64_t)(0 + G[8] + 24ull),G[0]);
    // 180019696: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 180019699: mov qword ptr [rcx + 0x10], rax
    WR64((uint64_t)(0 + G[1] + 16ull),G[0]);
    // 18001969d: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 1800196a1: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 1800196a6: movsd qword ptr [rsp + 0x30], xmm7
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[7].d[0]));
    // 1800196ac: movsd qword ptr [rsp + 0x38], xmm7
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[7].d[0]));
    // 1800196b2: mov qword ptr [rcx + 0x18], rax
    WR64((uint64_t)(0 + G[1] + 24ull),G[0]);
    // 1800196b6: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 1800196b9: mov qword ptr [r11], rax
    WR64((uint64_t)(0 + G[11]),G[0]);
    // 1800196bc: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 1800196c0: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 1800196c5: movsd qword ptr [rsp + 0x20], xmm7
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[7].d[0]));
    // 1800196cb: movsd qword ptr [rsp + 0x28], xmm7
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[7].d[0]));
    // 1800196d1: mov qword ptr [r11 + 0x8], rax
    WR64((uint64_t)(0 + G[11] + 8ull),G[0]);
    // 1800196d5: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 1800196d8: lea r11, [rsp + 0xa0]
    G[11]=(uint64_t)(0 + G[4] + 160ull);
    // 1800196e0: mov qword ptr [r8 + 0x20], rax
    WR64((uint64_t)(0 + G[8] + 32ull),G[0]);
    // 1800196e4: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 1800196e8: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 1800196ed: mov qword ptr [r8 + 0x28], rax
    WR64((uint64_t)(0 + G[8] + 40ull),G[0]);
    // 1800196f1: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 1800196f4: mov qword ptr [rdi], rax
    WR64((uint64_t)(0 + G[7]),G[0]);
    // 1800196f7: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 1800196fb: mov qword ptr [rdi + 0x8], rax
    WR64((uint64_t)(0 + G[7] + 8ull),G[0]);
    // 1800196ff: movsd qword ptr [rsp + 0x40], xmm7
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[7].d[0]));
    // 180019705: movsd qword ptr [rsp + 0x48], xmm7
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[7].d[0]));
    // 18001970b: movsd qword ptr [rsp + 0x30], xmm7
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[7].d[0]));
    // 180019711: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 180019716: lea rbx, [r8 + 0x30]
    G[3]=(uint64_t)(0 + G[8] + 48ull);
    // 18001971a: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001971d: mov qword ptr [r11], rax
    WR64((uint64_t)(0 + G[11]),G[0]);
    // 180019720: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 180019724: movsd qword ptr [rsp + 0x38], xmm7
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[7].d[0]));
    // 18001972a: mov qword ptr [r11 + 0x8], rax
    WR64((uint64_t)(0 + G[11] + 8ull),G[0]);
    // 18001972e: movsd qword ptr [rsp + 0x20], xmm7
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[7].d[0]));
    // 180019734: movsd qword ptr [rsp + 0x28], xmm7
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[7].d[0]));
    // 18001973a: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 18001973f: lea r11, [rsp + 0xb0]
    G[11]=(uint64_t)(0 + G[4] + 176ull);
    // 180019747: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001974a: mov qword ptr [rbx], rax
    WR64((uint64_t)(0 + G[3]),G[0]);
    // 18001974d: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 180019751: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 180019756: movsd qword ptr [rsp + 0x40], xmm7
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[7].d[0]));
    // 18001975c: movsd qword ptr [rsp + 0x48], xmm7
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[7].d[0]));
    // 180019762: mov qword ptr [rbx + 0x8], rax
    WR64((uint64_t)(0 + G[3] + 8ull),G[0]);
    // 180019766: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 180019769: mov qword ptr [rcx + 0x30], rax
    WR64((uint64_t)(0 + G[1] + 48ull),G[0]);
    // 18001976d: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 180019771: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 180019776: movsd qword ptr [rsp + 0x30], xmm7
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[7].d[0]));
    // 18001977c: movsd qword ptr [rsp + 0x38], xmm7
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[7].d[0]));
    // 180019782: mov qword ptr [rcx + 0x38], rax
    WR64((uint64_t)(0 + G[1] + 56ull),G[0]);
    // 180019786: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 180019789: mov qword ptr [r11], rax
    WR64((uint64_t)(0 + G[11]),G[0]);
    // 18001978c: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 180019790: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 180019795: movsd qword ptr [rsp + 0x20], xmm7
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[7].d[0]));
    // 18001979b: movsd qword ptr [rsp + 0x28], xmm7
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[7].d[0]));
    // 1800197a1: mov qword ptr [r11 + 0x8], rax
    WR64((uint64_t)(0 + G[11] + 8ull),G[0]);
    // 1800197a5: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 1800197a8: lea r11, [rsp + 0xc0]
    G[11]=(uint64_t)(0 + G[4] + 192ull);
    // 1800197b0: mov qword ptr [r8 + 0x40], rax
    WR64((uint64_t)(0 + G[8] + 64ull),G[0]);
    // 1800197b4: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 1800197b8: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 1800197bd: movsd qword ptr [rsp + 0x40], xmm7
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[7].d[0]));
    // 1800197c3: movsd qword ptr [rsp + 0x48], xmm7
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[7].d[0]));
    // 1800197c9: mov qword ptr [r8 + 0x48], rax
    WR64((uint64_t)(0 + G[8] + 72ull),G[0]);
    // 1800197cd: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 1800197d0: mov qword ptr [rcx + 0x40], rax
    WR64((uint64_t)(0 + G[1] + 64ull),G[0]);
    // 1800197d4: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 1800197d8: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 1800197dd: movsd qword ptr [rsp + 0x30], xmm7
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[7].d[0]));
    // 1800197e3: movsd qword ptr [rsp + 0x38], xmm7
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[7].d[0]));
    // 1800197e9: mov qword ptr [rcx + 0x48], rax
    WR64((uint64_t)(0 + G[1] + 72ull),G[0]);
    // 1800197ed: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 1800197f0: mov qword ptr [r11], rax
    WR64((uint64_t)(0 + G[11]),G[0]);
    // 1800197f3: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 1800197f7: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 1800197fc: movsd qword ptr [rsp + 0x20], xmm7
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[7].d[0]));
    // 180019802: movsd qword ptr [rsp + 0x28], xmm7
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[7].d[0]));
    // 180019808: mov qword ptr [r11 + 0x8], rax
    WR64((uint64_t)(0 + G[11] + 8ull),G[0]);
    // 18001980c: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001980f: lea r11, [rsp + 0xd0]
    G[11]=(uint64_t)(0 + G[4] + 208ull);
    // 180019817: mov qword ptr [r8 + 0x50], rax
    WR64((uint64_t)(0 + G[8] + 80ull),G[0]);
    // 18001981b: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 18001981f: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 180019824: movsd qword ptr [rsp + 0x40], xmm7
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[7].d[0]));
    // 18001982a: movsd qword ptr [rsp + 0x48], xmm7
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[7].d[0]));
    // 180019830: mov qword ptr [r8 + 0x58], rax
    WR64((uint64_t)(0 + G[8] + 88ull),G[0]);
    // 180019834: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 180019837: mov qword ptr [rcx + 0x50], rax
    WR64((uint64_t)(0 + G[1] + 80ull),G[0]);
    // 18001983b: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 18001983f: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 180019844: movsd qword ptr [rsp + 0x30], xmm7
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[7].d[0]));
    // 18001984a: movsd qword ptr [rsp + 0x38], xmm7
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[7].d[0]));
    // 180019850: mov qword ptr [rcx + 0x58], rax
    WR64((uint64_t)(0 + G[1] + 88ull),G[0]);
    // 180019854: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 180019857: mov qword ptr [r11], rax
    WR64((uint64_t)(0 + G[11]),G[0]);
    // 18001985a: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 18001985e: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 180019863: mov qword ptr [r11 + 0x8], rax
    WR64((uint64_t)(0 + G[11] + 8ull),G[0]);
    // 180019867: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001986a: mov qword ptr [r8 + 0x60], rax
    WR64((uint64_t)(0 + G[8] + 96ull),G[0]);
    // 18001986e: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 180019872: mov qword ptr [r8 + 0x68], rax
    WR64((uint64_t)(0 + G[8] + 104ull),G[0]);
    // 180019876: movsd xmm6, qword ptr [rip + 0x258b02] # 0x180272380
    X[6].d[0]=KD(0x3ff0000000000000ull); X[6].d[1]=0.0;
    // 18001987e: movsd xmm12, qword ptr [rsp + 0x218]
    X[12].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 536ull))); X[12].d[1]=0.0;
    // 180019888: movsd xmm15, qword ptr [rip + 0x25989f] # 0x180273130
    X[15].d[0]=KD(0x4000000000000000ull); X[15].d[1]=0.0;
    // 180019891: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 180019896: lea r11, [rsp + 0xe0]
    G[11]=(uint64_t)(0 + G[4] + 224ull);
    // 18001989e: mov r12, r10
    G[12]=G[10];
    // 1800198a1: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 1800198a4: mov qword ptr [rcx + 0x60], rax
    WR64((uint64_t)(0 + G[1] + 96ull),G[0]);
    // 1800198a8: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 1800198ac: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 1800198b1: movsd qword ptr [rsp + 0x20], xmm7
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[7].d[0]));
    // 1800198b7: movsd qword ptr [rsp + 0x28], xmm7
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[7].d[0]));
    // 1800198bd: movsd qword ptr [rsp + 0x40], xmm7
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[7].d[0]));
    // 1800198c3: movsd qword ptr [rsp + 0x48], xmm7
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[7].d[0]));
    // 1800198c9: movsd qword ptr [rsp + 0x30], xmm7
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[7].d[0]));
    // 1800198cf: movsd qword ptr [rsp + 0x38], xmm7
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[7].d[0]));
    // 1800198d5: mov qword ptr [rcx + 0x68], rax
    WR64((uint64_t)(0 + G[1] + 104ull),G[0]);
    // 1800198d9: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 1800198dc: movsd qword ptr [rsp + 0x50], xmm6
    WR64((uint64_t)(0 + G[4] + 80ull),BITS(X[6].d[0]));
    // 1800198e2: movsd qword ptr [rsp + 0x58], xmm7
    WR64((uint64_t)(0 + G[4] + 88ull),BITS(X[7].d[0]));
    // 1800198e8: movapd xmm8, xmm6
    X[8]=X[6];
    // 1800198ed: mov qword ptr [r11], rax
    WR64((uint64_t)(0 + G[11]),G[0]);
    // 1800198f0: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 1800198f4: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 1800198f9: movaps xmm0, xmmword ptr [rsp + 0x50]
    X[0].u[0]=RD64((uint64_t)(0 + G[4] + 80ull)); X[0].u[1]=RD64((uint64_t)(0 + G[4] + 80ull)+8);
    // 1800198fe: movdqa xmmword ptr [rsp + 0x50], xmm0
    WR64((uint64_t)(0 + G[4] + 80ull),X[0].u[0]); WR64((uint64_t)(0 + G[4] + 80ull)+8,X[0].u[1]);
    // 180019904: movsd qword ptr [rsp + 0x20], xmm7
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[7].d[0]));
    // 18001990a: movsd qword ptr [rsp + 0x28], xmm7
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[7].d[0]));
    // 180019910: mov qword ptr [r11 + 0x8], rax
    WR64((uint64_t)(0 + G[11] + 8ull),G[0]);
    // 180019914: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 180019917: lea r11, [rsp + 0xf0]
    G[11]=(uint64_t)(0 + G[4] + 240ull);
    // 18001991f: mov qword ptr [r8 + 0x70], rax
    WR64((uint64_t)(0 + G[8] + 112ull),G[0]);
    // 180019923: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 180019927: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 18001992c: movsd qword ptr [rsp + 0x40], xmm7
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[7].d[0]));
    // 180019932: movsd qword ptr [rsp + 0x48], xmm7
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[7].d[0]));
    // 180019938: mov qword ptr [r8 + 0x78], rax
    WR64((uint64_t)(0 + G[8] + 120ull),G[0]);
    // 18001993c: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001993f: mov qword ptr [rcx + 0x70], rax
    WR64((uint64_t)(0 + G[1] + 112ull),G[0]);
    // 180019943: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 180019947: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 18001994c: movsd qword ptr [rsp + 0x30], xmm7
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[7].d[0]));
    // 180019952: movsd qword ptr [rsp + 0x38], xmm7
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[7].d[0]));
    // 180019958: mov qword ptr [rcx + 0x78], rax
    WR64((uint64_t)(0 + G[1] + 120ull),G[0]);
    // 18001995c: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001995f: mov qword ptr [r11], rax
    WR64((uint64_t)(0 + G[11]),G[0]);
    // 180019962: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 180019966: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 18001996b: movsd qword ptr [rsp + 0x20], xmm7
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[7].d[0]));
    // 180019971: movsd qword ptr [rsp + 0x28], xmm7
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[7].d[0]));
    // 180019977: mov qword ptr [r11 + 0x8], rax
    WR64((uint64_t)(0 + G[11] + 8ull),G[0]);
    // 18001997b: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001997e: lea r11, [rsp + 0x100]
    G[11]=(uint64_t)(0 + G[4] + 256ull);
    // 180019986: mov qword ptr [r8 + 0x80], rax
    WR64((uint64_t)(0 + G[8] + 128ull),G[0]);
    // 18001998d: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 180019991: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 180019996: movsd qword ptr [rsp + 0x40], xmm7
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[7].d[0]));
    // 18001999c: movsd qword ptr [rsp + 0x48], xmm7
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[7].d[0]));
    // 1800199a2: mov qword ptr [r8 + 0x88], rax
    WR64((uint64_t)(0 + G[8] + 136ull),G[0]);
    // 1800199a9: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 1800199ac: mov qword ptr [rcx + 0x80], rax
    WR64((uint64_t)(0 + G[1] + 128ull),G[0]);
    // 1800199b3: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 1800199b7: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 1800199bc: movsd qword ptr [rsp + 0x30], xmm7
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[7].d[0]));
    // 1800199c2: movsd qword ptr [rsp + 0x38], xmm7
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[7].d[0]));
    // 1800199c8: mov qword ptr [rcx + 0x88], rax
    WR64((uint64_t)(0 + G[1] + 136ull),G[0]);
    // 1800199cf: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 1800199d2: mov qword ptr [r11], rax
    WR64((uint64_t)(0 + G[11]),G[0]);
    // 1800199d5: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 1800199d9: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 1800199de: mov qword ptr [r11 + 0x8], rax
    WR64((uint64_t)(0 + G[11] + 8ull),G[0]);
    // 1800199e2: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 1800199e5: mov qword ptr [r8 + 0x90], rax
    WR64((uint64_t)(0 + G[8] + 144ull),G[0]);
    // 1800199ec: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 1800199f0: mov qword ptr [r8 + 0x98], rax
    WR64((uint64_t)(0 + G[8] + 152ull),G[0]);
    // 1800199f7: lea r8, [rsp + 0x30]
    G[8]=(uint64_t)(0 + G[4] + 48ull);
    // 1800199fc: mov rax, qword ptr [r8]
    G[0]=RD64((uint64_t)(0 + G[8]));
    // 1800199ff: mov qword ptr [rcx + 0x90], rax
    WR64((uint64_t)(0 + G[1] + 144ull),G[0]);
    // 180019a06: mov rax, qword ptr [r8 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[8] + 8ull));
    // 180019a0a: mov r8d, 0x1
    G[8]=(uint64_t)(uint32_t)(1ull);
    // 180019a10: mov qword ptr [rcx + 0x98], rax
    WR64((uint64_t)(0 + G[1] + 152ull),G[0]);
    // 180019a17: cmp r10, 0x4
    { uint64_t a=G[10], b=4ull, r=(((G[10])-(4ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
    }
    // 180019a1b: jl 0x180019c3d
    if ((F.sf!=F.of)) goto L_180019c3d;
    // 180019a21: lea rcx, [rdx + 0x30]
    G[1]=(uint64_t)(0 + G[2] + 48ull);
    // 180019a25: mov rdx, r10
    G[2]=G[10];
    // 180019a28: shr rdx, 0x2
    { uint64_t a=G[2], b=2ull, r=(((G[2])>>(2ull))&0xFFFFFFFFFFFFFFFFull); F.set(4,64,r,a,b);
      G[2]=r;
    }
    // 180019a2c: lea r8, [4*rdx + 0x1]
    G[8]=(uint64_t)(0 + (G[2]*4) + 1ull);
    // 180019a34: nop word ptr [rax + rax]
L_180019a40:;
    // 180019a40: mov rax, qword ptr [rcx - 0x20]
    G[0]=RD64((uint64_t)(0 + G[1] - 32ull));
    // 180019a44: movsd xmm2, qword ptr [rsp + 0x58]
    X[2].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 88ull))); X[2].d[1]=0.0;
    // 180019a4a: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 180019a4f: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 180019a52: mov rax, qword ptr [rcx - 0x18]
    G[0]=RD64((uint64_t)(0 + G[1] - 24ull));
    // 180019a56: movapd xmm3, xmm15
    X[3]=X[15];
    // 180019a5b: movapd xmm4, xmm7
    X[4]=X[7];
    // 180019a5f: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 180019a63: mov rax, qword ptr [rcx - 0x10]
    G[0]=RD64((uint64_t)(0 + G[1] - 16ull));
    // 180019a67: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 180019a6c: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 180019a72: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 180019a78: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 180019a7b: mov rax, qword ptr [rcx - 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] - 8ull));
    // 180019a7f: mulsd xmm0, xmm12
    X[0].d[0]=X[0].d[0] * X[12].d[0];
    // 180019a84: mulsd xmm1, xmm12
    X[1].d[0]=X[1].d[0] * X[12].d[0];
    // 180019a89: subsd xmm3, xmm0
    X[3].d[0]=X[3].d[0] - X[0].d[0];
    // 180019a8d: movapd xmm0, xmm2
    X[0]=X[2];
    // 180019a91: mulsd xmm2, xmm3
    X[2].d[0]=X[2].d[0] * X[3].d[0];
    // 180019a95: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 180019a99: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 180019a9c: subsd xmm4, xmm1
    X[4].d[0]=X[4].d[0] - X[1].d[0];
    // 180019aa0: movsd xmm1, qword ptr [rsp + 0x50]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 80ull))); X[1].d[1]=0.0;
    // 180019aa6: movapd xmm5, xmm3
    X[5]=X[3];
    // 180019aaa: movapd xmm3, xmm7
    X[3]=X[7];
    // 180019aae: mulsd xmm0, xmm4
    X[0].d[0]=X[0].d[0] * X[4].d[0];
    // 180019ab2: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 180019ab7: mulsd xmm4, xmm1
    X[4].d[0]=X[4].d[0] * X[1].d[0];
    // 180019abb: mulsd xmm5, xmm1
    X[5].d[0]=X[5].d[0] * X[1].d[0];
    // 180019abf: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 180019ac5: subsd xmm5, xmm0
    X[5].d[0]=X[5].d[0] - X[0].d[0];
    // 180019ac9: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 180019acf: mulsd xmm0, xmm12
    X[0].d[0]=X[0].d[0] * X[12].d[0];
    // 180019ad4: mulsd xmm1, xmm12
    X[1].d[0]=X[1].d[0] * X[12].d[0];
    // 180019ad9: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 180019adc: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 180019ae0: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 180019ae4: mov rax, qword ptr [rcx + 0x10]
    G[0]=RD64((uint64_t)(0 + G[1] + 16ull));
    // 180019ae8: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 180019aed: addsd xmm4, xmm2
    X[4].d[0]=X[4].d[0] + X[2].d[0];
    // 180019af1: movsd qword ptr [rsp + 0x30], xmm5
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[5].d[0]));
    // 180019af7: subsd xmm3, xmm1
    X[3].d[0]=X[3].d[0] - X[1].d[0];
    // 180019afb: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 180019b01: movsd qword ptr [rsp + 0x38], xmm4
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[4].d[0]));
    // 180019b07: movapd xmm2, xmm15
    X[2]=X[15];
    // 180019b0c: mulsd xmm4, xmm3
    X[4].d[0]=X[4].d[0] * X[3].d[0];
    // 180019b10: mulsd xmm1, xmm12
    X[1].d[0]=X[1].d[0] * X[12].d[0];
    // 180019b15: subsd xmm2, xmm0
    X[2].d[0]=X[2].d[0] - X[0].d[0];
    // 180019b19: movaps xmm0, xmmword ptr [rsp + 0x30]
    X[0].u[0]=RD64((uint64_t)(0 + G[4] + 48ull)); X[0].u[1]=RD64((uint64_t)(0 + G[4] + 48ull)+8);
    // 180019b1e: movdqa xmmword ptr [rsp + 0x40], xmm0
    WR64((uint64_t)(0 + G[4] + 64ull),X[0].u[0]); WR64((uint64_t)(0 + G[4] + 64ull)+8,X[0].u[1]);
    // 180019b24: movsd xmm0, qword ptr [rsp + 0x48]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 72ull))); X[0].d[1]=0.0;
    // 180019b2a: mulsd xmm3, qword ptr [rsp + 0x40]
    X[3].d[0]=X[3].d[0] * DBL(RD64((uint64_t)(0 + G[4] + 64ull)));
    // 180019b30: mulsd xmm5, xmm2
    X[5].d[0]=X[5].d[0] * X[2].d[0];
    // 180019b34: subsd xmm5, xmm4
    X[5].d[0]=X[5].d[0] - X[4].d[0];
    // 180019b38: movapd xmm4, xmm7
    X[4]=X[7];
    // 180019b3c: mulsd xmm0, xmm2
    X[0].d[0]=X[0].d[0] * X[2].d[0];
    // 180019b40: movsd qword ptr [rsp + 0x30], xmm5
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[5].d[0]));
    // 180019b46: subsd xmm4, xmm1
    X[4].d[0]=X[4].d[0] - X[1].d[0];
    // 180019b4a: movapd xmm2, xmm15
    X[2]=X[15];
    // 180019b4f: addsd xmm3, xmm0
    X[3].d[0]=X[3].d[0] + X[0].d[0];
    // 180019b53: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 180019b59: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 180019b5c: mov rax, qword ptr [rcx + 0x18]
    G[0]=RD64((uint64_t)(0 + G[1] + 24ull));
    // 180019b60: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 180019b64: mulsd xmm0, xmm12
    X[0].d[0]=X[0].d[0] * X[12].d[0];
    // 180019b69: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 180019b6f: subsd xmm2, xmm0
    X[2].d[0]=X[2].d[0] - X[0].d[0];
    // 180019b73: movsd qword ptr [rsp + 0x38], xmm3
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[3].d[0]));
    // 180019b79: mulsd xmm3, xmm4
    X[3].d[0]=X[3].d[0] * X[4].d[0];
    // 180019b7d: movaps xmm0, xmmword ptr [rsp + 0x30]
    X[0].u[0]=RD64((uint64_t)(0 + G[4] + 48ull)); X[0].u[1]=RD64((uint64_t)(0 + G[4] + 48ull)+8);
    // 180019b82: movdqa xmmword ptr [rsp + 0x40], xmm0
    WR64((uint64_t)(0 + G[4] + 64ull),X[0].u[0]); WR64((uint64_t)(0 + G[4] + 64ull)+8,X[0].u[1]);
    // 180019b88: mulsd xmm4, qword ptr [rsp + 0x40]
    X[4].d[0]=X[4].d[0] * DBL(RD64((uint64_t)(0 + G[4] + 64ull)));
    // 180019b8e: mulsd xmm5, xmm2
    X[5].d[0]=X[5].d[0] * X[2].d[0];
    // 180019b92: movsd xmm0, qword ptr [rsp + 0x48]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 72ull))); X[0].d[1]=0.0;
    // 180019b98: subsd xmm5, xmm3
    X[5].d[0]=X[5].d[0] - X[3].d[0];
    // 180019b9c: mulsd xmm0, xmm2
    X[0].d[0]=X[0].d[0] * X[2].d[0];
    // 180019ba0: movsd qword ptr [rsp + 0x30], xmm5
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[5].d[0]));
    // 180019ba6: addsd xmm4, xmm0
    X[4].d[0]=X[4].d[0] + X[0].d[0];
    // 180019baa: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 180019bb0: mulsd xmm0, xmm12
    X[0].d[0]=X[0].d[0] * X[12].d[0];
    // 180019bb5: movsd qword ptr [rsp + 0x38], xmm4
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[4].d[0]));
    // 180019bbb: mulsd xmm8, xmm12
    X[8].d[0]=X[8].d[0] * X[12].d[0];
    // 180019bc0: movapd xmm2, xmm15
    X[2]=X[15];
    // 180019bc5: mulsd xmm1, xmm12
    X[1].d[0]=X[1].d[0] * X[12].d[0];
    // 180019bca: subsd xmm2, xmm0
    X[2].d[0]=X[2].d[0] - X[0].d[0];
    // 180019bce: movapd xmm3, xmm7
    X[3]=X[7];
    // 180019bd2: add rcx, 0x40
    { uint64_t a=G[1], b=64ull, r=(((G[1])+(64ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[1]=r;
    }
    // 180019bd6: sub rdx, 0x1
    { uint64_t a=G[2], b=1ull, r=(((G[2])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[2]=r;
    }
    // 180019bda: movaps xmm0, xmmword ptr [rsp + 0x30]
    X[0].u[0]=RD64((uint64_t)(0 + G[4] + 48ull)); X[0].u[1]=RD64((uint64_t)(0 + G[4] + 48ull)+8);
    // 180019bdf: movdqa xmmword ptr [rsp + 0x40], xmm0
    WR64((uint64_t)(0 + G[4] + 64ull),X[0].u[0]); WR64((uint64_t)(0 + G[4] + 64ull)+8,X[0].u[1]);
    // 180019be5: mulsd xmm8, xmm12
    X[8].d[0]=X[8].d[0] * X[12].d[0];
    // 180019bea: mulsd xmm5, xmm2
    X[5].d[0]=X[5].d[0] * X[2].d[0];
    // 180019bee: movsd xmm0, qword ptr [rsp + 0x48]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 72ull))); X[0].d[1]=0.0;
    // 180019bf4: subsd xmm3, xmm1
    X[3].d[0]=X[3].d[0] - X[1].d[0];
    // 180019bf8: mulsd xmm4, xmm3
    X[4].d[0]=X[4].d[0] * X[3].d[0];
    // 180019bfc: mulsd xmm3, qword ptr [rsp + 0x40]
    X[3].d[0]=X[3].d[0] * DBL(RD64((uint64_t)(0 + G[4] + 64ull)));
    // 180019c02: mulsd xmm0, xmm2
    X[0].d[0]=X[0].d[0] * X[2].d[0];
    // 180019c06: subsd xmm5, xmm4
    X[5].d[0]=X[5].d[0] - X[4].d[0];
    // 180019c0a: addsd xmm3, xmm0
    X[3].d[0]=X[3].d[0] + X[0].d[0];
    // 180019c0e: movsd qword ptr [rsp + 0x30], xmm5
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[5].d[0]));
    // 180019c14: mulsd xmm8, xmm12
    X[8].d[0]=X[8].d[0] * X[12].d[0];
    // 180019c19: movsd qword ptr [rsp + 0x38], xmm3
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[3].d[0]));
    // 180019c1f: movaps xmm0, xmmword ptr [rsp + 0x30]
    X[0].u[0]=RD64((uint64_t)(0 + G[4] + 48ull)); X[0].u[1]=RD64((uint64_t)(0 + G[4] + 48ull)+8);
    // 180019c24: mulsd xmm8, xmm12
    X[8].d[0]=X[8].d[0] * X[12].d[0];
    // 180019c29: movdqa xmmword ptr [rsp + 0x50], xmm0
    WR64((uint64_t)(0 + G[4] + 80ull),X[0].u[0]); WR64((uint64_t)(0 + G[4] + 80ull)+8,X[0].u[1]);
    // 180019c2f: jne 0x180019a40
    if (!F.zf) goto L_180019a40;
    // 180019c35: mov rdx, qword ptr [rsp + 0x1f0]
    G[2]=RD64((uint64_t)(0 + G[4] + 496ull));
L_180019c3d:;
    // 180019c3d: cmp r8, r10
    { uint64_t a=G[8], b=G[10], r=(((G[8])-(G[10]))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
    }
    // 180019c40: jg 0x180019ce9
    if ((!F.zf&&F.sf==F.of)) goto L_180019ce9;
    // 180019c46: mov rcx, r8
    G[1]=G[8];
    // 180019c49: shl rcx, 0x4
    { uint64_t a=G[1], b=4ull, r=(((G[1])<<(4ull))&0xFFFFFFFFFFFFFFFFull); F.set(4,64,r,a,b);
      G[1]=r;
    }
    // 180019c4d: add rcx, rdx
    { uint64_t a=G[1], b=G[2], r=(((G[1])+(G[2]))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[1]=r;
    }
    // 180019c50: mov rdx, r10
    G[2]=G[10];
    // 180019c53: sub rdx, r8
    { uint64_t a=G[2], b=G[8], r=(((G[2])-(G[8]))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[2]=r;
    }
    // 180019c56: inc rdx
    { uint64_t a=G[2], b=1ull, r=(((G[2])+(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(3,64,r,a,b);
      G[2]=r;
    }
L_180019c59:;
    // 180019c59: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 180019c5c: movsd xmm3, qword ptr [rsp + 0x58]
    X[3].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 88ull))); X[3].d[1]=0.0;
    // 180019c62: lea r8, [rsp + 0x20]
    G[8]=(uint64_t)(0 + G[4] + 32ull);
    // 180019c67: mov qword ptr [r8], rax
    WR64((uint64_t)(0 + G[8]),G[0]);
    // 180019c6a: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 180019c6e: movapd xmm4, xmm15
    X[4]=X[15];
    // 180019c73: mulsd xmm8, xmm12
    X[8].d[0]=X[8].d[0] * X[12].d[0];
    // 180019c78: movapd xmm5, xmm7
    X[5]=X[7];
    // 180019c7c: mov qword ptr [r8 + 0x8], rax
    WR64((uint64_t)(0 + G[8] + 8ull),G[0]);
    // 180019c80: add rcx, 0x10
    { uint64_t a=G[1], b=16ull, r=(((G[1])+(16ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[1]=r;
    }
    // 180019c84: sub rdx, 0x1
    { uint64_t a=G[2], b=1ull, r=(((G[2])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[2]=r;
    }
    // 180019c88: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 180019c8e: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 180019c94: mulsd xmm0, xmm12
    X[0].d[0]=X[0].d[0] * X[12].d[0];
    // 180019c99: mulsd xmm1, xmm12
    X[1].d[0]=X[1].d[0] * X[12].d[0];
    // 180019c9e: subsd xmm4, xmm0
    X[4].d[0]=X[4].d[0] - X[0].d[0];
    // 180019ca2: subsd xmm5, xmm1
    X[5].d[0]=X[5].d[0] - X[1].d[0];
    // 180019ca6: movsd xmm1, qword ptr [rsp + 0x50]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 80ull))); X[1].d[1]=0.0;
    // 180019cac: movapd xmm0, xmm3
    X[0]=X[3];
    // 180019cb0: movapd xmm2, xmm4
    X[2]=X[4];
    // 180019cb4: mulsd xmm3, xmm4
    X[3].d[0]=X[3].d[0] * X[4].d[0];
    // 180019cb8: mulsd xmm0, xmm5
    X[0].d[0]=X[0].d[0] * X[5].d[0];
    // 180019cbc: mulsd xmm2, xmm1
    X[2].d[0]=X[2].d[0] * X[1].d[0];
    // 180019cc0: mulsd xmm5, xmm1
    X[5].d[0]=X[5].d[0] * X[1].d[0];
    // 180019cc4: subsd xmm2, xmm0
    X[2].d[0]=X[2].d[0] - X[0].d[0];
    // 180019cc8: addsd xmm5, xmm3
    X[5].d[0]=X[5].d[0] + X[3].d[0];
    // 180019ccc: movsd qword ptr [rsp + 0x30], xmm2
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[2].d[0]));
    // 180019cd2: movsd qword ptr [rsp + 0x38], xmm5
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[5].d[0]));
    // 180019cd8: movaps xmm0, xmmword ptr [rsp + 0x30]
    X[0].u[0]=RD64((uint64_t)(0 + G[4] + 48ull)); X[0].u[1]=RD64((uint64_t)(0 + G[4] + 48ull)+8);
    // 180019cdd: movdqa xmmword ptr [rsp + 0x50], xmm0
    WR64((uint64_t)(0 + G[4] + 80ull),X[0].u[0]); WR64((uint64_t)(0 + G[4] + 80ull)+8,X[0].u[1]);
    // 180019ce3: jne 0x180019c59
    if (!F.zf) goto L_180019c59;
L_180019ce9:;
    // 180019ce9: mulsd xmm8, qword ptr [rsp + 0x210]
    X[8].d[0]=X[8].d[0] * DBL(RD64((uint64_t)(0 + G[4] + 528ull)));
    // 180019cf3: xor r15d, r15d
    G[15]=(uint64_t)(uint32_t)(0); F.set(0,32,0ull,0,0);
    // 180019cf6: lea rcx, [rsp + 0x50]
    G[1]=(uint64_t)(0 + G[4] + 80ull);
    // 180019cfb: movsd qword ptr [rsp + 0x58], xmm7
    WR64((uint64_t)(0 + G[4] + 88ull),BITS(X[7].d[0]));
    // 180019d01: lea rdx, [rsp + 0x70]
    G[2]=(uint64_t)(0 + G[4] + 112ull);
    // 180019d06: mov esi, r15d
    G[6]=(uint64_t)(uint32_t)((G[15]&0xFFFFFFFFull));
    // 180019d09: divsd xmm8, qword ptr [rsp + 0x50]
    X[8].d[0]=X[8].d[0] / DBL(RD64((uint64_t)(0 + G[4] + 80ull)));
    // 180019d10: mov r9, r15
    G[9]=G[15];
    // 180019d13: movsd qword ptr [rsp + 0x50], xmm6
    WR64((uint64_t)(0 + G[4] + 80ull),BITS(X[6].d[0]));
    // 180019d19: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 180019d1c: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 180019d1f: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 180019d23: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 180019d27: mov eax, r10d
    G[0]=(uint64_t)(uint32_t)((G[10]&0xFFFFFFFFull));
    // 180019d2a: sub eax, ebp
    { uint64_t a=(G[0]&0xFFFFFFFFull), b=(G[5]&0xFFFFFFFFull), r=((((G[0]&0xFFFFFFFFull))-((G[5]&0xFFFFFFFFull)))&0xFFFFFFFFull); F.set(2,32,r,a,b);
      G[0]=(uint64_t)(uint32_t)(r);
    }
    // 180019d2c: cmp eax, 0x1
    { uint64_t a=(G[0]&0xFFFFFFFFull), b=1ull, r=((((G[0]&0xFFFFFFFFull))-(1ull))&0xFFFFFFFFull); F.set(2,32,r,a,b);
    }
    // 180019d2f: jl 0x180019f90
    if ((F.sf!=F.of)) goto L_180019f90;
    // 180019d35: lea r10, [rsp + 0x70]
    G[10]=(uint64_t)(0 + G[4] + 112ull);
    // 180019d3a: mov esi, eax
    G[6]=(uint64_t)(uint32_t)((G[0]&0xFFFFFFFFull));
    // 180019d3c: mov r11d, eax
    G[11]=(uint64_t)(uint32_t)((G[0]&0xFFFFFFFFull));
    // 180019d3f: sub r10, 0x20
    { uint64_t a=G[10], b=32ull, r=(((G[10])-(32ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[10]=r;
    }
    // 180019d43: nop word ptr [rax + rax]
L_180019d50:;
    // 180019d50: inc r9
    { uint64_t a=G[9], b=1ull, r=(((G[9])+(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(3,64,r,a,b);
      G[9]=r;
    }
    // 180019d53: add r10, 0x10
    { uint64_t a=G[10], b=16ull, r=(((G[10])+(16ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[10]=r;
    }
    // 180019d57: cmp r9, 0x4
    { uint64_t a=G[9], b=4ull, r=(((G[9])-(4ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
    }
    // 180019d5b: mov r8, r9
    G[8]=G[9];
    // 180019d5e: jl 0x180019f0b
    if ((F.sf!=F.of)) goto L_180019f0b;
    // 180019d64: mov rdx, r9
    G[2]=G[9];
    // 180019d67: mov rcx, r10
    G[1]=G[10];
    // 180019d6a: shr rdx, 0x2
    { uint64_t a=G[2], b=2ull, r=(((G[2])>>(2ull))&0xFFFFFFFFFFFFFFFFull); F.set(4,64,r,a,b);
      G[2]=r;
    }
    // 180019d6e: lea rax, [4*rdx]
    G[0]=(uint64_t)(0 + (G[2]*4));
    // 180019d76: sub r8, rax
    { uint64_t a=G[8], b=G[0], r=(((G[8])-(G[0]))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[8]=r;
    }
    // 180019d79: nop dword ptr [rax]
L_180019d80:;
    // 180019d80: mov rax, qword ptr [rcx + 0x10]
    G[0]=RD64((uint64_t)(0 + G[1] + 16ull));
    // 180019d84: lea r14, [rsp + 0x20]
    G[14]=(uint64_t)(0 + G[4] + 32ull);
    // 180019d89: mov qword ptr [r14], rax
    WR64((uint64_t)(0 + G[14]),G[0]);
    // 180019d8c: mov rax, qword ptr [rcx + 0x18]
    G[0]=RD64((uint64_t)(0 + G[1] + 24ull));
    // 180019d90: mov qword ptr [r14 + 0x8], rax
    WR64((uint64_t)(0 + G[14] + 8ull),G[0]);
    // 180019d94: mov rax, qword ptr [rcx + 0x20]
    G[0]=RD64((uint64_t)(0 + G[1] + 32ull));
    // 180019d98: lea r14, [rsp + 0x30]
    G[14]=(uint64_t)(0 + G[4] + 48ull);
    // 180019d9d: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 180019da3: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 180019da9: mov qword ptr [r14], rax
    WR64((uint64_t)(0 + G[14]),G[0]);
    // 180019dac: mov rax, qword ptr [rcx + 0x28]
    G[0]=RD64((uint64_t)(0 + G[1] + 40ull));
    // 180019db0: mov qword ptr [r14 + 0x8], rax
    WR64((uint64_t)(0 + G[14] + 8ull),G[0]);
    // 180019db4: lea r14, [rsp + 0x40]
    G[14]=(uint64_t)(0 + G[4] + 64ull);
    // 180019db9: addsd xmm0, qword ptr [rsp + 0x30]
    X[0].d[0]=X[0].d[0] + DBL(RD64((uint64_t)(0 + G[4] + 48ull)));
    // 180019dbf: addsd xmm1, qword ptr [rsp + 0x38]
    X[1].d[0]=X[1].d[0] + DBL(RD64((uint64_t)(0 + G[4] + 56ull)));
    // 180019dc5: movsd qword ptr [rsp + 0x40], xmm0
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[0].d[0]));
    // 180019dcb: movsd qword ptr [rsp + 0x48], xmm1
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[1].d[0]));
    // 180019dd1: mov rax, qword ptr [r14]
    G[0]=RD64((uint64_t)(0 + G[14]));
    // 180019dd4: mov qword ptr [rcx + 0x20], rax
    WR64((uint64_t)(0 + G[1] + 32ull),G[0]);
    // 180019dd8: mov rax, qword ptr [r14 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[14] + 8ull));
    // 180019ddc: lea r14, [rsp + 0x20]
    G[14]=(uint64_t)(0 + G[4] + 32ull);
    // 180019de1: mov qword ptr [rcx + 0x28], rax
    WR64((uint64_t)(0 + G[1] + 40ull),G[0]);
    // 180019de5: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 180019de8: mov qword ptr [r14], rax
    WR64((uint64_t)(0 + G[14]),G[0]);
    // 180019deb: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 180019def: mov qword ptr [r14 + 0x8], rax
    WR64((uint64_t)(0 + G[14] + 8ull),G[0]);
    // 180019df3: mov rax, qword ptr [rcx + 0x10]
    G[0]=RD64((uint64_t)(0 + G[1] + 16ull));
    // 180019df7: lea r14, [rsp + 0x30]
    G[14]=(uint64_t)(0 + G[4] + 48ull);
    // 180019dfc: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 180019e02: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 180019e08: mov qword ptr [r14], rax
    WR64((uint64_t)(0 + G[14]),G[0]);
    // 180019e0b: mov rax, qword ptr [rcx + 0x18]
    G[0]=RD64((uint64_t)(0 + G[1] + 24ull));
    // 180019e0f: mov qword ptr [r14 + 0x8], rax
    WR64((uint64_t)(0 + G[14] + 8ull),G[0]);
    // 180019e13: lea r14, [rsp + 0x40]
    G[14]=(uint64_t)(0 + G[4] + 64ull);
    // 180019e18: addsd xmm0, qword ptr [rsp + 0x30]
    X[0].d[0]=X[0].d[0] + DBL(RD64((uint64_t)(0 + G[4] + 48ull)));
    // 180019e1e: addsd xmm1, qword ptr [rsp + 0x38]
    X[1].d[0]=X[1].d[0] + DBL(RD64((uint64_t)(0 + G[4] + 56ull)));
    // 180019e24: movsd qword ptr [rsp + 0x40], xmm0
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[0].d[0]));
    // 180019e2a: movsd qword ptr [rsp + 0x48], xmm1
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[1].d[0]));
    // 180019e30: mov rax, qword ptr [r14]
    G[0]=RD64((uint64_t)(0 + G[14]));
    // 180019e33: mov qword ptr [rcx + 0x10], rax
    WR64((uint64_t)(0 + G[1] + 16ull),G[0]);
    // 180019e37: mov rax, qword ptr [r14 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[14] + 8ull));
    // 180019e3b: lea r14, [rsp + 0x20]
    G[14]=(uint64_t)(0 + G[4] + 32ull);
    // 180019e40: mov qword ptr [rcx + 0x18], rax
    WR64((uint64_t)(0 + G[1] + 24ull),G[0]);
    // 180019e44: mov rax, qword ptr [rcx - 0x10]
    G[0]=RD64((uint64_t)(0 + G[1] - 16ull));
    // 180019e48: mov qword ptr [r14], rax
    WR64((uint64_t)(0 + G[14]),G[0]);
    // 180019e4b: mov rax, qword ptr [rcx - 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] - 8ull));
    // 180019e4f: mov qword ptr [r14 + 0x8], rax
    WR64((uint64_t)(0 + G[14] + 8ull),G[0]);
    // 180019e53: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 180019e56: lea r14, [rsp + 0x30]
    G[14]=(uint64_t)(0 + G[4] + 48ull);
    // 180019e5b: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 180019e61: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 180019e67: mov qword ptr [r14], rax
    WR64((uint64_t)(0 + G[14]),G[0]);
    // 180019e6a: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 180019e6e: mov qword ptr [r14 + 0x8], rax
    WR64((uint64_t)(0 + G[14] + 8ull),G[0]);
    // 180019e72: lea r14, [rsp + 0x40]
    G[14]=(uint64_t)(0 + G[4] + 64ull);
    // 180019e77: addsd xmm0, qword ptr [rsp + 0x30]
    X[0].d[0]=X[0].d[0] + DBL(RD64((uint64_t)(0 + G[4] + 48ull)));
    // 180019e7d: addsd xmm1, qword ptr [rsp + 0x38]
    X[1].d[0]=X[1].d[0] + DBL(RD64((uint64_t)(0 + G[4] + 56ull)));
    // 180019e83: movsd qword ptr [rsp + 0x40], xmm0
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[0].d[0]));
    // 180019e89: movsd qword ptr [rsp + 0x48], xmm1
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[1].d[0]));
    // 180019e8f: mov rax, qword ptr [r14]
    G[0]=RD64((uint64_t)(0 + G[14]));
    // 180019e92: mov qword ptr [rcx], rax
    WR64((uint64_t)(0 + G[1]),G[0]);
    // 180019e95: mov rax, qword ptr [r14 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[14] + 8ull));
    // 180019e99: lea r14, [rsp + 0x20]
    G[14]=(uint64_t)(0 + G[4] + 32ull);
    // 180019e9e: mov qword ptr [rcx + 0x8], rax
    WR64((uint64_t)(0 + G[1] + 8ull),G[0]);
    // 180019ea2: mov rax, qword ptr [rcx - 0x20]
    G[0]=RD64((uint64_t)(0 + G[1] - 32ull));
    // 180019ea6: mov qword ptr [r14], rax
    WR64((uint64_t)(0 + G[14]),G[0]);
    // 180019ea9: mov rax, qword ptr [rcx - 0x18]
    G[0]=RD64((uint64_t)(0 + G[1] - 24ull));
    // 180019ead: mov qword ptr [r14 + 0x8], rax
    WR64((uint64_t)(0 + G[14] + 8ull),G[0]);
    // 180019eb1: mov rax, qword ptr [rcx - 0x10]
    G[0]=RD64((uint64_t)(0 + G[1] - 16ull));
    // 180019eb5: lea r14, [rsp + 0x30]
    G[14]=(uint64_t)(0 + G[4] + 48ull);
    // 180019eba: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 180019ec0: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 180019ec6: mov qword ptr [r14], rax
    WR64((uint64_t)(0 + G[14]),G[0]);
    // 180019ec9: mov rax, qword ptr [rcx - 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] - 8ull));
    // 180019ecd: mov qword ptr [r14 + 0x8], rax
    WR64((uint64_t)(0 + G[14] + 8ull),G[0]);
    // 180019ed1: lea r14, [rsp + 0x40]
    G[14]=(uint64_t)(0 + G[4] + 64ull);
    // 180019ed6: addsd xmm0, qword ptr [rsp + 0x30]
    X[0].d[0]=X[0].d[0] + DBL(RD64((uint64_t)(0 + G[4] + 48ull)));
    // 180019edc: addsd xmm1, qword ptr [rsp + 0x38]
    X[1].d[0]=X[1].d[0] + DBL(RD64((uint64_t)(0 + G[4] + 56ull)));
    // 180019ee2: movsd qword ptr [rsp + 0x40], xmm0
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[0].d[0]));
    // 180019ee8: movsd qword ptr [rsp + 0x48], xmm1
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[1].d[0]));
    // 180019eee: mov rax, qword ptr [r14]
    G[0]=RD64((uint64_t)(0 + G[14]));
    // 180019ef1: mov qword ptr [rcx - 0x10], rax
    WR64((uint64_t)(0 + G[1] - 16ull),G[0]);
    // 180019ef5: mov rax, qword ptr [r14 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[14] + 8ull));
    // 180019ef9: sub rcx, 0x40
    { uint64_t a=G[1], b=64ull, r=(((G[1])-(64ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[1]=r;
    }
    // 180019efd: sub rdx, 0x1
    { uint64_t a=G[2], b=1ull, r=(((G[2])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[2]=r;
    }
    // 180019f01: mov qword ptr [rcx + 0x38], rax
    WR64((uint64_t)(0 + G[1] + 56ull),G[0]);
    // 180019f05: jne 0x180019d80
    if (!F.zf) goto L_180019d80;
L_180019f0b:;
    // 180019f0b: cmp r8, 0x1
    { uint64_t a=G[8], b=1ull, r=(((G[8])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
    }
    // 180019f0f: jl 0x180019f86
    if ((F.sf!=F.of)) goto L_180019f86;
    // 180019f11: mov rax, r8
    G[0]=G[8];
    // 180019f14: add rax, rax
    { uint64_t a=G[0], b=G[0], r=(((G[0])+(G[0]))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[0]=r;
    }
    // 180019f17: lea rdx, [rsp + 8*rax + 0x70]
    G[2]=(uint64_t)(0 + G[4] + (G[0]*8) + 112ull);
L_180019f1c:;
    // 180019f1c: mov rax, qword ptr [rdx - 0x10]
    G[0]=RD64((uint64_t)(0 + G[2] - 16ull));
    // 180019f20: lea r14, [rsp + 0x20]
    G[14]=(uint64_t)(0 + G[4] + 32ull);
    // 180019f25: add rdx, -0x10
    { uint64_t a=G[2], b=18446744073709551600ull, r=(((G[2])+(18446744073709551600ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[2]=r;
    }
    // 180019f29: sub r8, 0x1
    { uint64_t a=G[8], b=1ull, r=(((G[8])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[8]=r;
    }
    // 180019f2d: mov qword ptr [r14], rax
    WR64((uint64_t)(0 + G[14]),G[0]);
    // 180019f30: mov rax, qword ptr [rdx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[2] + 8ull));
    // 180019f34: mov qword ptr [r14 + 0x8], rax
    WR64((uint64_t)(0 + G[14] + 8ull),G[0]);
    // 180019f38: mov rax, qword ptr [rdx + 0x10]
    G[0]=RD64((uint64_t)(0 + G[2] + 16ull));
    // 180019f3c: lea r14, [rsp + 0x30]
    G[14]=(uint64_t)(0 + G[4] + 48ull);
    // 180019f41: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 180019f47: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 180019f4d: mov qword ptr [r14], rax
    WR64((uint64_t)(0 + G[14]),G[0]);
    // 180019f50: mov rax, qword ptr [rdx + 0x18]
    G[0]=RD64((uint64_t)(0 + G[2] + 24ull));
    // 180019f54: mov qword ptr [r14 + 0x8], rax
    WR64((uint64_t)(0 + G[14] + 8ull),G[0]);
    // 180019f58: lea r14, [rsp + 0x40]
    G[14]=(uint64_t)(0 + G[4] + 64ull);
    // 180019f5d: addsd xmm0, qword ptr [rsp + 0x30]
    X[0].d[0]=X[0].d[0] + DBL(RD64((uint64_t)(0 + G[4] + 48ull)));
    // 180019f63: addsd xmm1, qword ptr [rsp + 0x38]
    X[1].d[0]=X[1].d[0] + DBL(RD64((uint64_t)(0 + G[4] + 56ull)));
    // 180019f69: movsd qword ptr [rsp + 0x40], xmm0
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[0].d[0]));
    // 180019f6f: movsd qword ptr [rsp + 0x48], xmm1
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[1].d[0]));
    // 180019f75: mov rax, qword ptr [r14]
    G[0]=RD64((uint64_t)(0 + G[14]));
    // 180019f78: mov qword ptr [rdx + 0x10], rax
    WR64((uint64_t)(0 + G[2] + 16ull),G[0]);
    // 180019f7c: mov rax, qword ptr [r14 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[14] + 8ull));
    // 180019f80: mov qword ptr [rdx + 0x18], rax
    WR64((uint64_t)(0 + G[2] + 24ull),G[0]);
    // 180019f84: jne 0x180019f1c
    if (!F.zf) goto L_180019f1c;
L_180019f86:;
    // 180019f86: sub r11, 0x1
    { uint64_t a=G[11], b=1ull, r=(((G[11])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[11]=r;
    }
    // 180019f8a: jne 0x180019d50
    if (!F.zf) goto L_180019d50;
L_180019f90:;
    // 180019f90: cmp rbp, 0x1
    { uint64_t a=G[5], b=1ull, r=(((G[5])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
    }
    // 180019f94: movsd xmm2, qword ptr [rip + 0x2594e4] # 0x180273480
    X[2].d[0]=KD(0xc000000000000000ull); X[2].d[1]=0.0;
    // 180019f9c: movaps xmmword ptr [rsp + 0x170], xmm9
    WR64((uint64_t)(0 + G[4] + 368ull),X[9].u[0]); WR64((uint64_t)(0 + G[4] + 368ull)+8,X[9].u[1]);
    // 180019fa5: movaps xmmword ptr [rsp + 0x160], xmm10
    WR64((uint64_t)(0 + G[4] + 352ull),X[10].u[0]); WR64((uint64_t)(0 + G[4] + 352ull)+8,X[10].u[1]);
    // 180019fae: movsxd rsi, esi
    G[6]=(uint64_t)(int64_t)(int32_t)((G[6]&0xFFFFFFFFull));
    // 180019fb1: mov r14, rbp
    G[14]=G[5];
    // 180019fb4: movaps xmmword ptr [rsp + 0x150], xmm11
    WR64((uint64_t)(0 + G[4] + 336ull),X[11].u[0]); WR64((uint64_t)(0 + G[4] + 336ull)+8,X[11].u[1]);
    // 180019fbd: jl 0x18001a43b
    if ((F.sf!=F.of)) goto L_18001a43b;
    // 180019fc3: lea rax, [rsi - 0x2]
    G[0]=(uint64_t)(0 + G[6] - 2ull);
    // 180019fc7: movaps xmmword ptr [rsp + 0x130], xmm13
    WR64((uint64_t)(0 + G[4] + 304ull),X[13].u[0]); WR64((uint64_t)(0 + G[4] + 304ull)+8,X[13].u[1]);
    // 180019fd0: movaps xmmword ptr [rsp + 0x120], xmm14
    WR64((uint64_t)(0 + G[4] + 288ull),X[14].u[0]); WR64((uint64_t)(0 + G[4] + 288ull)+8,X[14].u[1]);
    // 180019fd9: movapd xmm14, xmm2
    X[14]=X[2];
    // 180019fde: add rax, rax
    { uint64_t a=G[0], b=G[0], r=(((G[0])+(G[0]))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[0]=r;
    }
    // 180019fe1: lea rbp, [r13 + 0x10]
    G[5]=(uint64_t)(0 + G[13] + 16ull);
    // 180019fe5: lea r13, [rsp + 8*rax + 0x70]
    G[13]=(uint64_t)(0 + G[4] + (G[0]*8) + 112ull);
    // 180019fea: divsd xmm14, xmm12
    X[14].d[0]=X[14].d[0] / X[12].d[0];
    // 180019fef: nop 
L_180019ff0:;
    // 180019ff0: mov rax, qword ptr [rbp]
    G[0]=RD64((uint64_t)(0 + G[5]));
    // 180019ff4: lea rcx, [rsp + 0x50]
    G[1]=(uint64_t)(0 + G[4] + 80ull);
    // 180019ff9: movapd xmm9, xmm14
    X[9]=X[14];
    // 180019ffe: mov qword ptr [rcx], rax
    WR64((uint64_t)(0 + G[1]),G[0]);
    // 18001a001: mov rax, qword ptr [rbp + 0x8]
    G[0]=RD64((uint64_t)(0 + G[5] + 8ull));
    // 18001a005: movapd xmm11, xmm7
    X[11]=X[7];
    // 18001a00a: movapd xmm6, xmm14
    X[6]=X[14];
    // 18001a00f: mov qword ptr [rcx + 0x8], rax
    WR64((uint64_t)(0 + G[1] + 8ull),G[0]);
    // 18001a013: mov rax, qword ptr [rbp]
    G[0]=RD64((uint64_t)(0 + G[5]));
    // 18001a017: lea rcx, [rsp + 0x60]
    G[1]=(uint64_t)(0 + G[4] + 96ull);
    // 18001a01c: movsd xmm10, qword ptr [rsp + 0x58]
    X[10].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 88ull))); X[10].d[1]=0.0;
    // 18001a023: addsd xmm6, qword ptr [rsp + 0x50]
    X[6].d[0]=X[6].d[0] + DBL(RD64((uint64_t)(0 + G[4] + 80ull)));
    // 18001a029: mov qword ptr [rcx], rax
    WR64((uint64_t)(0 + G[1]),G[0]);
    // 18001a02c: mov rax, qword ptr [rbp + 0x8]
    G[0]=RD64((uint64_t)(0 + G[5] + 8ull));
    // 18001a030: inc rsi
    { uint64_t a=G[6], b=1ull, r=(((G[6])+(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(3,64,r,a,b);
      G[6]=r;
    }
    // 18001a033: addsd xmm10, xmm7
    X[10].d[0]=X[10].d[0] + X[7].d[0];
    // 18001a038: mov qword ptr [rcx + 0x8], rax
    WR64((uint64_t)(0 + G[1] + 8ull),G[0]);
    // 18001a03c: add r13, 0x10
    { uint64_t a=G[13], b=16ull, r=(((G[13])+(16ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[13]=r;
    }
    // 18001a040: subsd xmm9, qword ptr [rsp + 0x60]
    X[9].d[0]=X[9].d[0] - DBL(RD64((uint64_t)(0 + G[4] + 96ull)));
    // 18001a047: subsd xmm11, qword ptr [rsp + 0x68]
    X[11].d[0]=X[11].d[0] - DBL(RD64((uint64_t)(0 + G[4] + 104ull)));
    // 18001a04e: movapd xmm1, xmm9
    X[1]=X[9];
    // 18001a053: movapd xmm0, xmm11
    X[0]=X[11];
    // 18001a058: mulsd xmm0, xmm11
    X[0].d[0]=X[0].d[0] * X[11].d[0];
    // 18001a05d: mulsd xmm1, xmm9
    X[1].d[0]=X[1].d[0] * X[9].d[0];
    // 18001a062: addsd xmm1, xmm0
    X[1].d[0]=X[1].d[0] + X[0].d[0];
    // 18001a066: movapd xmm0, xmm1
    X[0]=X[1];
    // 18001a06a: call 0x1800bf0a6
    X[0].d[0]=std::sqrt(X[0].d[0]);
    // 18001a06f: movapd xmm1, xmm6
    X[1]=X[6];
    // 18001a073: mulsd xmm1, xmm6
    X[1].d[0]=X[1].d[0] * X[6].d[0];
    // 18001a077: movapd xmm13, xmm0
    X[13]=X[0];
    // 18001a07c: movapd xmm0, xmm10
    X[0]=X[10];
    // 18001a081: mulsd xmm0, xmm10
    X[0].d[0]=X[0].d[0] * X[10].d[0];
    // 18001a086: addsd xmm1, xmm0
    X[1].d[0]=X[1].d[0] + X[0].d[0];
    // 18001a08a: movapd xmm0, xmm1
    X[0]=X[1];
    // 18001a08e: call 0x1800bf0a6
    X[0].d[0]=std::sqrt(X[0].d[0]);
    // 18001a093: ucomisd xmm9, xmm7
    F.ucom(X[9].d[0], X[7].d[0]);
    // 18001a098: divsd xmm13, xmm0
    X[13].d[0]=X[13].d[0] / X[0].d[0];
    // 18001a09d: jp 0x18001a0a1
    if (F.pf) goto L_18001a0a1;
    // 18001a09f: je 0x18001a0a9
    if (F.zf) goto L_18001a0a9;
L_18001a0a1:;
    // 18001a0a1: ucomisd xmm6, xmm7
    F.ucom(X[6].d[0], X[7].d[0]);
    // 18001a0a5: jp 0x18001a0b0
    if (F.pf) goto L_18001a0b0;
    // 18001a0a7: jne 0x18001a0b0
    if (!F.zf) goto L_18001a0b0;
L_18001a0a9:;
    // 18001a0a9: movapd xmm9, xmm7
    X[9]=X[7];
    // 18001a0ae: jmp 0x18001a0d7
    goto L_18001a0d7;
L_18001a0b0:;
    // 18001a0b0: movapd xmm1, xmm9
    X[1]=X[9];
    // 18001a0b5: movapd xmm0, xmm11
    X[0]=X[11];
    // 18001a0ba: call 0x1800bf0ac
    X[0].d[0]=std::atan2(X[0].d[0],X[1].d[0]);
    // 18001a0bf: movapd xmm1, xmm6
    X[1]=X[6];
    // 18001a0c3: movapd xmm9, xmm0
    X[9]=X[0];
    // 18001a0c8: movapd xmm0, xmm10
    X[0]=X[10];
    // 18001a0cd: call 0x1800bf0ac
    X[0].d[0]=std::atan2(X[0].d[0],X[1].d[0]);
    // 18001a0d2: subsd xmm9, xmm0
    X[9].d[0]=X[9].d[0] - X[0].d[0];
L_18001a0d7:;
    // 18001a0d7: movapd xmm0, xmm9
    X[0]=X[9];
    // 18001a0dc: call 0x1800bf09a
    X[0].d[0]=std::cos(X[0].d[0]);
    // 18001a0e1: movapd xmm6, xmm0
    X[6]=X[0];
    // 18001a0e5: movapd xmm0, xmm9
    X[0]=X[9];
    // 18001a0ea: mulsd xmm6, xmm13
    X[6].d[0]=X[6].d[0] * X[13].d[0];
    // 18001a0ef: call 0x1800bf0a0
    X[0].d[0]=std::sin(X[0].d[0]);
    // 18001a0f4: cmp rsi, 0x4
    { uint64_t a=G[6], b=4ull, r=(((G[6])-(4ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
    }
    // 18001a0f8: mov r8, rsi
    G[8]=G[6];
    // 18001a0fb: movapd xmm5, xmm0
    X[5]=X[0];
    // 18001a0ff: mulsd xmm5, xmm13
    X[5].d[0]=X[5].d[0] * X[13].d[0];
    // 18001a104: jl 0x18001a35c
    if ((F.sf!=F.of)) goto L_18001a35c;
    // 18001a10a: mov rdx, rsi
    G[2]=G[6];
    // 18001a10d: mov rcx, r13
    G[1]=G[13];
    // 18001a110: shr rdx, 0x2
    { uint64_t a=G[2], b=2ull, r=(((G[2])>>(2ull))&0xFFFFFFFFFFFFFFFFull); F.set(4,64,r,a,b);
      G[2]=r;
    }
    // 18001a114: lea rax, [4*rdx]
    G[0]=(uint64_t)(0 + (G[2]*4));
    // 18001a11c: sub r8, rax
    { uint64_t a=G[8], b=G[0], r=(((G[8])-(G[0]))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[8]=r;
    }
    // 18001a11f: nop 
L_18001a120:;
    // 18001a120: mov rax, qword ptr [rcx + 0x10]
    G[0]=RD64((uint64_t)(0 + G[1] + 16ull));
    // 18001a124: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a129: movapd xmm3, xmm5
    X[3]=X[5];
    // 18001a12d: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001a130: mov rax, qword ptr [rcx + 0x18]
    G[0]=RD64((uint64_t)(0 + G[1] + 24ull));
    // 18001a134: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001a138: mov rax, qword ptr [rcx + 0x20]
    G[0]=RD64((uint64_t)(0 + G[1] + 32ull));
    // 18001a13c: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 18001a141: movsd xmm1, qword ptr [rsp + 0x20]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[1].d[1]=0.0;
    // 18001a147: movsd xmm2, qword ptr [rsp + 0x28]
    X[2].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[2].d[1]=0.0;
    // 18001a14d: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001a150: mov rax, qword ptr [rcx + 0x28]
    G[0]=RD64((uint64_t)(0 + G[1] + 40ull));
    // 18001a154: mulsd xmm3, xmm1
    X[3].d[0]=X[3].d[0] * X[1].d[0];
    // 18001a158: movapd xmm4, xmm1
    X[4]=X[1];
    // 18001a15c: movapd xmm0, xmm2
    X[0]=X[2];
    // 18001a160: mulsd xmm2, xmm6
    X[2].d[0]=X[2].d[0] * X[6].d[0];
    // 18001a164: addsd xmm3, xmm2
    X[3].d[0]=X[3].d[0] + X[2].d[0];
    // 18001a168: mulsd xmm0, xmm5
    X[0].d[0]=X[0].d[0] * X[5].d[0];
    // 18001a16c: mulsd xmm4, xmm6
    X[4].d[0]=X[4].d[0] * X[6].d[0];
    // 18001a170: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001a174: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 18001a179: subsd xmm4, xmm0
    X[4].d[0]=X[4].d[0] - X[0].d[0];
    // 18001a17d: movsd xmm0, qword ptr [rsp + 0x30]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 48ull))); X[0].d[1]=0.0;
    // 18001a183: movsd xmm1, qword ptr [rsp + 0x38]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 56ull))); X[1].d[1]=0.0;
    // 18001a189: addsd xmm0, xmm4
    X[0].d[0]=X[0].d[0] + X[4].d[0];
    // 18001a18d: addsd xmm1, xmm3
    X[1].d[0]=X[1].d[0] + X[3].d[0];
    // 18001a191: movapd xmm3, xmm5
    X[3]=X[5];
    // 18001a195: movsd qword ptr [rsp + 0x40], xmm0
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[0].d[0]));
    // 18001a19b: movsd qword ptr [rsp + 0x48], xmm1
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[1].d[0]));
    // 18001a1a1: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001a1a4: mov qword ptr [rcx + 0x20], rax
    WR64((uint64_t)(0 + G[1] + 32ull),G[0]);
    // 18001a1a8: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 18001a1ac: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a1b1: mov qword ptr [rcx + 0x28], rax
    WR64((uint64_t)(0 + G[1] + 40ull),G[0]);
    // 18001a1b5: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001a1b8: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001a1bb: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001a1bf: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001a1c3: mov rax, qword ptr [rcx + 0x10]
    G[0]=RD64((uint64_t)(0 + G[1] + 16ull));
    // 18001a1c7: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 18001a1cc: movsd xmm1, qword ptr [rsp + 0x20]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[1].d[1]=0.0;
    // 18001a1d2: movsd xmm2, qword ptr [rsp + 0x28]
    X[2].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[2].d[1]=0.0;
    // 18001a1d8: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001a1db: mov rax, qword ptr [rcx + 0x18]
    G[0]=RD64((uint64_t)(0 + G[1] + 24ull));
    // 18001a1df: mulsd xmm3, xmm1
    X[3].d[0]=X[3].d[0] * X[1].d[0];
    // 18001a1e3: movapd xmm4, xmm1
    X[4]=X[1];
    // 18001a1e7: movapd xmm0, xmm2
    X[0]=X[2];
    // 18001a1eb: mulsd xmm2, xmm6
    X[2].d[0]=X[2].d[0] * X[6].d[0];
    // 18001a1ef: addsd xmm3, xmm2
    X[3].d[0]=X[3].d[0] + X[2].d[0];
    // 18001a1f3: mulsd xmm0, xmm5
    X[0].d[0]=X[0].d[0] * X[5].d[0];
    // 18001a1f7: mulsd xmm4, xmm6
    X[4].d[0]=X[4].d[0] * X[6].d[0];
    // 18001a1fb: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001a1ff: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 18001a204: subsd xmm4, xmm0
    X[4].d[0]=X[4].d[0] - X[0].d[0];
    // 18001a208: movsd xmm0, qword ptr [rsp + 0x30]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 48ull))); X[0].d[1]=0.0;
    // 18001a20e: movsd xmm1, qword ptr [rsp + 0x38]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 56ull))); X[1].d[1]=0.0;
    // 18001a214: addsd xmm0, xmm4
    X[0].d[0]=X[0].d[0] + X[4].d[0];
    // 18001a218: addsd xmm1, xmm3
    X[1].d[0]=X[1].d[0] + X[3].d[0];
    // 18001a21c: movapd xmm3, xmm5
    X[3]=X[5];
    // 18001a220: movsd qword ptr [rsp + 0x40], xmm0
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[0].d[0]));
    // 18001a226: movsd qword ptr [rsp + 0x48], xmm1
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[1].d[0]));
    // 18001a22c: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001a22f: mov qword ptr [rcx + 0x10], rax
    WR64((uint64_t)(0 + G[1] + 16ull),G[0]);
    // 18001a233: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 18001a237: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a23c: mov qword ptr [rcx + 0x18], rax
    WR64((uint64_t)(0 + G[1] + 24ull),G[0]);
    // 18001a240: mov rax, qword ptr [rcx - 0x10]
    G[0]=RD64((uint64_t)(0 + G[1] - 16ull));
    // 18001a244: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001a247: mov rax, qword ptr [rcx - 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] - 8ull));
    // 18001a24b: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001a24f: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 18001a254: movsd xmm1, qword ptr [rsp + 0x20]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[1].d[1]=0.0;
    // 18001a25a: movsd xmm2, qword ptr [rsp + 0x28]
    X[2].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[2].d[1]=0.0;
    // 18001a260: movapd xmm0, xmm2
    X[0]=X[2];
    // 18001a264: movapd xmm4, xmm1
    X[4]=X[1];
    // 18001a268: mulsd xmm3, xmm1
    X[3].d[0]=X[3].d[0] * X[1].d[0];
    // 18001a26c: mulsd xmm2, xmm6
    X[2].d[0]=X[2].d[0] * X[6].d[0];
    // 18001a270: mulsd xmm4, xmm6
    X[4].d[0]=X[4].d[0] * X[6].d[0];
    // 18001a274: mulsd xmm0, xmm5
    X[0].d[0]=X[0].d[0] * X[5].d[0];
    // 18001a278: addsd xmm3, xmm2
    X[3].d[0]=X[3].d[0] + X[2].d[0];
    // 18001a27c: subsd xmm4, xmm0
    X[4].d[0]=X[4].d[0] - X[0].d[0];
    // 18001a280: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001a283: sub rcx, 0x40
    { uint64_t a=G[1], b=64ull, r=(((G[1])-(64ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[1]=r;
    }
    // 18001a287: sub rdx, 0x1
    { uint64_t a=G[2], b=1ull, r=(((G[2])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[2]=r;
    }
    // 18001a28b: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001a28e: mov rax, qword ptr [rcx + 0x48]
    G[0]=RD64((uint64_t)(0 + G[1] + 72ull));
    // 18001a292: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001a296: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 18001a29b: movsd xmm0, qword ptr [rsp + 0x30]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 48ull))); X[0].d[1]=0.0;
    // 18001a2a1: movsd xmm1, qword ptr [rsp + 0x38]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 56ull))); X[1].d[1]=0.0;
    // 18001a2a7: addsd xmm0, xmm4
    X[0].d[0]=X[0].d[0] + X[4].d[0];
    // 18001a2ab: addsd xmm1, xmm3
    X[1].d[0]=X[1].d[0] + X[3].d[0];
    // 18001a2af: movapd xmm3, xmm5
    X[3]=X[5];
    // 18001a2b3: movsd qword ptr [rsp + 0x40], xmm0
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[0].d[0]));
    // 18001a2b9: movsd qword ptr [rsp + 0x48], xmm1
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[1].d[0]));
    // 18001a2bf: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001a2c2: mov qword ptr [rcx + 0x40], rax
    WR64((uint64_t)(0 + G[1] + 64ull),G[0]);
    // 18001a2c6: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 18001a2ca: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a2cf: mov qword ptr [rcx + 0x48], rax
    WR64((uint64_t)(0 + G[1] + 72ull),G[0]);
    // 18001a2d3: mov rax, qword ptr [rcx + 0x20]
    G[0]=RD64((uint64_t)(0 + G[1] + 32ull));
    // 18001a2d7: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001a2da: mov rax, qword ptr [rcx + 0x28]
    G[0]=RD64((uint64_t)(0 + G[1] + 40ull));
    // 18001a2de: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001a2e2: mov rax, qword ptr [rcx + 0x30]
    G[0]=RD64((uint64_t)(0 + G[1] + 48ull));
    // 18001a2e6: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 18001a2eb: movsd xmm1, qword ptr [rsp + 0x20]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[1].d[1]=0.0;
    // 18001a2f1: movsd xmm2, qword ptr [rsp + 0x28]
    X[2].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[2].d[1]=0.0;
    // 18001a2f7: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001a2fa: mov rax, qword ptr [rcx + 0x38]
    G[0]=RD64((uint64_t)(0 + G[1] + 56ull));
    // 18001a2fe: movapd xmm4, xmm1
    X[4]=X[1];
    // 18001a302: mulsd xmm3, xmm1
    X[3].d[0]=X[3].d[0] * X[1].d[0];
    // 18001a306: movapd xmm0, xmm2
    X[0]=X[2];
    // 18001a30a: mulsd xmm4, xmm6
    X[4].d[0]=X[4].d[0] * X[6].d[0];
    // 18001a30e: mulsd xmm2, xmm6
    X[2].d[0]=X[2].d[0] * X[6].d[0];
    // 18001a312: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001a316: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 18001a31b: addsd xmm3, xmm2
    X[3].d[0]=X[3].d[0] + X[2].d[0];
    // 18001a31f: mulsd xmm0, xmm5
    X[0].d[0]=X[0].d[0] * X[5].d[0];
    // 18001a323: movsd xmm1, qword ptr [rsp + 0x38]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 56ull))); X[1].d[1]=0.0;
    // 18001a329: subsd xmm4, xmm0
    X[4].d[0]=X[4].d[0] - X[0].d[0];
    // 18001a32d: movsd xmm0, qword ptr [rsp + 0x30]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 48ull))); X[0].d[1]=0.0;
    // 18001a333: addsd xmm1, xmm3
    X[1].d[0]=X[1].d[0] + X[3].d[0];
    // 18001a337: movsd qword ptr [rsp + 0x48], xmm1
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[1].d[0]));
    // 18001a33d: addsd xmm0, xmm4
    X[0].d[0]=X[0].d[0] + X[4].d[0];
    // 18001a341: movsd qword ptr [rsp + 0x40], xmm0
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[0].d[0]));
    // 18001a347: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001a34a: mov qword ptr [rcx + 0x30], rax
    WR64((uint64_t)(0 + G[1] + 48ull),G[0]);
    // 18001a34e: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 18001a352: mov qword ptr [rcx + 0x38], rax
    WR64((uint64_t)(0 + G[1] + 56ull),G[0]);
    // 18001a356: jne 0x18001a120
    if (!F.zf) goto L_18001a120;
L_18001a35c:;
    // 18001a35c: cmp r8, 0x1
    { uint64_t a=G[8], b=1ull, r=(((G[8])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
    }
    // 18001a360: jl 0x18001a40b
    if ((F.sf!=F.of)) goto L_18001a40b;
    // 18001a366: mov rax, r8
    G[0]=G[8];
    // 18001a369: add rax, rax
    { uint64_t a=G[0], b=G[0], r=(((G[0])+(G[0]))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[0]=r;
    }
    // 18001a36c: lea rdx, [rsp + 8*rax + 0x70]
    G[2]=(uint64_t)(0 + G[4] + (G[0]*8) + 112ull);
L_18001a371:;
    // 18001a371: mov rax, qword ptr [rdx - 0x10]
    G[0]=RD64((uint64_t)(0 + G[2] - 16ull));
    // 18001a375: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a37a: movapd xmm3, xmm5
    X[3]=X[5];
    // 18001a37e: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001a381: mov rax, qword ptr [rdx - 0x8]
    G[0]=RD64((uint64_t)(0 + G[2] - 8ull));
    // 18001a385: add rdx, -0x10
    { uint64_t a=G[2], b=18446744073709551600ull, r=(((G[2])+(18446744073709551600ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[2]=r;
    }
    // 18001a389: sub r8, 0x1
    { uint64_t a=G[8], b=1ull, r=(((G[8])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[8]=r;
    }
    // 18001a38d: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001a391: mov rax, qword ptr [rdx + 0x10]
    G[0]=RD64((uint64_t)(0 + G[2] + 16ull));
    // 18001a395: movsd xmm1, qword ptr [rsp + 0x20]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[1].d[1]=0.0;
    // 18001a39b: movsd xmm2, qword ptr [rsp + 0x28]
    X[2].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[2].d[1]=0.0;
    // 18001a3a1: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 18001a3a6: movapd xmm4, xmm1
    X[4]=X[1];
    // 18001a3aa: mulsd xmm3, xmm1
    X[3].d[0]=X[3].d[0] * X[1].d[0];
    // 18001a3ae: movapd xmm0, xmm2
    X[0]=X[2];
    // 18001a3b2: mulsd xmm2, xmm6
    X[2].d[0]=X[2].d[0] * X[6].d[0];
    // 18001a3b6: mulsd xmm0, xmm5
    X[0].d[0]=X[0].d[0] * X[5].d[0];
    // 18001a3ba: mulsd xmm4, xmm6
    X[4].d[0]=X[4].d[0] * X[6].d[0];
    // 18001a3be: addsd xmm3, xmm2
    X[3].d[0]=X[3].d[0] + X[2].d[0];
    // 18001a3c2: subsd xmm4, xmm0
    X[4].d[0]=X[4].d[0] - X[0].d[0];
    // 18001a3c6: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001a3c9: mov rax, qword ptr [rdx + 0x18]
    G[0]=RD64((uint64_t)(0 + G[2] + 24ull));
    // 18001a3cd: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001a3d1: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 18001a3d6: movsd xmm0, qword ptr [rsp + 0x30]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 48ull))); X[0].d[1]=0.0;
    // 18001a3dc: movsd xmm1, qword ptr [rsp + 0x38]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 56ull))); X[1].d[1]=0.0;
    // 18001a3e2: addsd xmm0, xmm4
    X[0].d[0]=X[0].d[0] + X[4].d[0];
    // 18001a3e6: addsd xmm1, xmm3
    X[1].d[0]=X[1].d[0] + X[3].d[0];
    // 18001a3ea: movsd qword ptr [rsp + 0x40], xmm0
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[0].d[0]));
    // 18001a3f0: movsd qword ptr [rsp + 0x48], xmm1
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[1].d[0]));
    // 18001a3f6: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001a3f9: mov qword ptr [rdx + 0x10], rax
    WR64((uint64_t)(0 + G[2] + 16ull),G[0]);
    // 18001a3fd: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 18001a401: mov qword ptr [rdx + 0x18], rax
    WR64((uint64_t)(0 + G[2] + 24ull),G[0]);
    // 18001a405: jne 0x18001a371
    if (!F.zf) goto L_18001a371;
L_18001a40b:;
    // 18001a40b: add rbp, 0x10
    { uint64_t a=G[5], b=16ull, r=(((G[5])+(16ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[5]=r;
    }
    // 18001a40f: sub r14, 0x1
    { uint64_t a=G[14], b=1ull, r=(((G[14])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[14]=r;
    }
    // 18001a413: jne 0x180019ff0
    if (!F.zf) goto L_180019ff0;
    // 18001a419: movsd xmm2, qword ptr [rip + 0x25905f] # 0x180273480
    X[2].d[0]=KD(0xc000000000000000ull); X[2].d[1]=0.0;
    // 18001a421: movsd xmm6, qword ptr [rip + 0x257f57] # 0x180272380
    X[6].d[0]=KD(0x3ff0000000000000ull); X[6].d[1]=0.0;
    // 18001a429: movaps xmm14, xmmword ptr [rsp + 0x120]
    X[14].u[0]=RD64((uint64_t)(0 + G[4] + 288ull)); X[14].u[1]=RD64((uint64_t)(0 + G[4] + 288ull)+8);
    // 18001a432: movaps xmm13, xmmword ptr [rsp + 0x130]
    X[13].u[0]=RD64((uint64_t)(0 + G[4] + 304ull)); X[13].u[1]=RD64((uint64_t)(0 + G[4] + 304ull)+8);
L_18001a43b:;
    // 18001a43b: mov r14, qword ptr [rsp + 0x1b8]
    G[14]=RD64((uint64_t)(0 + G[4] + 440ull));
    // 18001a443: test r12, r12
    { uint64_t a=G[12], b=G[12], r=(((G[12])&(G[12]))&0xFFFFFFFFFFFFFFFFull); F.set(0,64,r,a,b);
    }
    // 18001a446: js 0x18001a64d
    if (F.sf) goto L_18001a64d;
    // 18001a44c: lea rax, [r12 + 0x1]
    G[0]=(uint64_t)(0 + G[12] + 1ull);
    // 18001a451: cmp rax, 0x4
    { uint64_t a=G[0], b=4ull, r=(((G[0])-(4ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
    }
    // 18001a455: jl 0x18001a5d0
    if ((F.sf!=F.of)) goto L_18001a5d0;
    // 18001a45b: mov r10, qword ptr [rsp + 0x228]
    G[10]=RD64((uint64_t)(0 + G[4] + 552ull));
    // 18001a463: lea r8, [rsp + 0x70]
    G[8]=(uint64_t)(0 + G[4] + 112ull);
    // 18001a468: lea r9, [rsp + 0x80]
    G[9]=(uint64_t)(0 + G[4] + 128ull);
    // 18001a470: lea rdx, [r12 + 0x1]
    G[2]=(uint64_t)(0 + G[12] + 1ull);
    // 18001a475: sub r8, r10
    { uint64_t a=G[8], b=G[10], r=(((G[8])-(G[10]))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[8]=r;
    }
    // 18001a478: sub r9, r10
    { uint64_t a=G[9], b=G[10], r=(((G[9])-(G[10]))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[9]=r;
    }
    // 18001a47b: shr rdx, 0x2
    { uint64_t a=G[2], b=2ull, r=(((G[2])>>(2ull))&0xFFFFFFFFFFFFFFFFull); F.set(4,64,r,a,b);
      G[2]=r;
    }
    // 18001a47f: lea rcx, [rsp + 0x80]
    G[1]=(uint64_t)(0 + G[4] + 128ull);
    // 18001a487: lea r15, [4*rdx]
    G[15]=(uint64_t)(0 + (G[2]*4));
    // 18001a48f: nop 
L_18001a490:;
    // 18001a490: mov rax, qword ptr [rcx - 0x10]
    G[0]=RD64((uint64_t)(0 + G[1] - 16ull));
    // 18001a494: lea r11, [rsp + 0x20]
    G[11]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a499: add rdi, 0x40
    { uint64_t a=G[7], b=64ull, r=(((G[7])+(64ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[7]=r;
    }
    // 18001a49d: mov qword ptr [r11], rax
    WR64((uint64_t)(0 + G[11]),G[0]);
    // 18001a4a0: mov rax, qword ptr [rcx - 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] - 8ull));
    // 18001a4a4: add rcx, 0x40
    { uint64_t a=G[1], b=64ull, r=(((G[1])+(64ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[1]=r;
    }
    // 18001a4a8: sub rdx, 0x1
    { uint64_t a=G[2], b=1ull, r=(((G[2])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[2]=r;
    }
    // 18001a4ac: mov qword ptr [r11 + 0x8], rax
    WR64((uint64_t)(0 + G[11] + 8ull),G[0]);
    // 18001a4b0: lea r11, [rsp + 0x30]
    G[11]=(uint64_t)(0 + G[4] + 48ull);
    // 18001a4b5: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 18001a4bb: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 18001a4c1: mulsd xmm0, xmm8
    X[0].d[0]=X[0].d[0] * X[8].d[0];
    // 18001a4c6: mulsd xmm1, xmm8
    X[1].d[0]=X[1].d[0] * X[8].d[0];
    // 18001a4cb: movsd qword ptr [rsp + 0x30], xmm0
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[0].d[0]));
    // 18001a4d1: movsd qword ptr [rsp + 0x38], xmm1
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[1].d[0]));
    // 18001a4d7: mov rax, qword ptr [r11]
    G[0]=RD64((uint64_t)(0 + G[11]));
    // 18001a4da: mov qword ptr [rdi - 0x60], rax
    WR64((uint64_t)(0 + G[7] - 96ull),G[0]);
    // 18001a4de: mov rax, qword ptr [r11 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[11] + 8ull));
    // 18001a4e2: lea r11, [rsp + 0x20]
    G[11]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a4e7: mov qword ptr [rdi - 0x58], rax
    WR64((uint64_t)(0 + G[7] - 88ull),G[0]);
    // 18001a4eb: mov rax, qword ptr [rcx - 0x40]
    G[0]=RD64((uint64_t)(0 + G[1] - 64ull));
    // 18001a4ef: mov qword ptr [r11], rax
    WR64((uint64_t)(0 + G[11]),G[0]);
    // 18001a4f2: mov rax, qword ptr [rcx - 0x38]
    G[0]=RD64((uint64_t)(0 + G[1] - 56ull));
    // 18001a4f6: mov qword ptr [r11 + 0x8], rax
    WR64((uint64_t)(0 + G[11] + 8ull),G[0]);
    // 18001a4fa: lea r11, [rsp + 0x30]
    G[11]=(uint64_t)(0 + G[4] + 48ull);
    // 18001a4ff: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 18001a505: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 18001a50b: mulsd xmm0, xmm8
    X[0].d[0]=X[0].d[0] * X[8].d[0];
    // 18001a510: mulsd xmm1, xmm8
    X[1].d[0]=X[1].d[0] * X[8].d[0];
    // 18001a515: movsd qword ptr [rsp + 0x30], xmm0
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[0].d[0]));
    // 18001a51b: movsd qword ptr [rsp + 0x38], xmm1
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[1].d[0]));
    // 18001a521: mov rax, qword ptr [r11]
    G[0]=RD64((uint64_t)(0 + G[11]));
    // 18001a524: mov qword ptr [rdi - 0x50], rax
    WR64((uint64_t)(0 + G[7] - 80ull),G[0]);
    // 18001a528: mov rax, qword ptr [r11 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[11] + 8ull));
    // 18001a52c: lea r11, [rsp + 0x20]
    G[11]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a531: mov qword ptr [rdi - 0x48], rax
    WR64((uint64_t)(0 + G[7] - 72ull),G[0]);
    // 18001a535: mov rax, qword ptr [r8 + rdi - 0x40]
    G[0]=RD64((uint64_t)(0 + G[8] + G[7] - 64ull));
    // 18001a53a: mov qword ptr [r11], rax
    WR64((uint64_t)(0 + G[11]),G[0]);
    // 18001a53d: mov rax, qword ptr [r8 + rdi - 0x38]
    G[0]=RD64((uint64_t)(0 + G[8] + G[7] - 56ull));
    // 18001a542: mov qword ptr [r11 + 0x8], rax
    WR64((uint64_t)(0 + G[11] + 8ull),G[0]);
    // 18001a546: lea r11, [rsp + 0x30]
    G[11]=(uint64_t)(0 + G[4] + 48ull);
    // 18001a54b: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 18001a551: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 18001a557: mulsd xmm0, xmm8
    X[0].d[0]=X[0].d[0] * X[8].d[0];
    // 18001a55c: mulsd xmm1, xmm8
    X[1].d[0]=X[1].d[0] * X[8].d[0];
    // 18001a561: movsd qword ptr [rsp + 0x30], xmm0
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[0].d[0]));
    // 18001a567: movsd qword ptr [rsp + 0x38], xmm1
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[1].d[0]));
    // 18001a56d: mov rax, qword ptr [r11]
    G[0]=RD64((uint64_t)(0 + G[11]));
    // 18001a570: mov qword ptr [rdi - 0x40], rax
    WR64((uint64_t)(0 + G[7] - 64ull),G[0]);
    // 18001a574: mov rax, qword ptr [r11 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[11] + 8ull));
    // 18001a578: lea r11, [rsp + 0x20]
    G[11]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a57d: mov qword ptr [rdi - 0x38], rax
    WR64((uint64_t)(0 + G[7] - 56ull),G[0]);
    // 18001a581: mov rax, qword ptr [r9 + rdi - 0x40]
    G[0]=RD64((uint64_t)(0 + G[9] + G[7] - 64ull));
    // 18001a586: mov qword ptr [r11], rax
    WR64((uint64_t)(0 + G[11]),G[0]);
    // 18001a589: mov rax, qword ptr [r9 + rdi - 0x38]
    G[0]=RD64((uint64_t)(0 + G[9] + G[7] - 56ull));
    // 18001a58e: mov qword ptr [r11 + 0x8], rax
    WR64((uint64_t)(0 + G[11] + 8ull),G[0]);
    // 18001a592: lea r11, [rsp + 0x30]
    G[11]=(uint64_t)(0 + G[4] + 48ull);
    // 18001a597: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 18001a59d: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 18001a5a3: mulsd xmm0, xmm8
    X[0].d[0]=X[0].d[0] * X[8].d[0];
    // 18001a5a8: mulsd xmm1, xmm8
    X[1].d[0]=X[1].d[0] * X[8].d[0];
    // 18001a5ad: movsd qword ptr [rsp + 0x30], xmm0
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[0].d[0]));
    // 18001a5b3: movsd qword ptr [rsp + 0x38], xmm1
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[1].d[0]));
    // 18001a5b9: mov rax, qword ptr [r11]
    G[0]=RD64((uint64_t)(0 + G[11]));
    // 18001a5bc: mov qword ptr [rdi - 0x30], rax
    WR64((uint64_t)(0 + G[7] - 48ull),G[0]);
    // 18001a5c0: mov rax, qword ptr [r11 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[11] + 8ull));
    // 18001a5c4: mov qword ptr [rdi - 0x28], rax
    WR64((uint64_t)(0 + G[7] - 40ull),G[0]);
    // 18001a5c8: jne 0x18001a490
    if (!F.zf) goto L_18001a490;
    // 18001a5ce: jmp 0x18001a5d8
    goto L_18001a5d8;
L_18001a5d0:;
    // 18001a5d0: mov r10, qword ptr [rsp + 0x228]
    G[10]=RD64((uint64_t)(0 + G[4] + 552ull));
L_18001a5d8:;
    // 18001a5d8: cmp r15, r12
    { uint64_t a=G[15], b=G[12], r=(((G[15])-(G[12]))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
    }
    // 18001a5db: jg 0x18001a64d
    if ((!F.zf&&F.sf==F.of)) goto L_18001a64d;
    // 18001a5dd: mov rcx, r15
    G[1]=G[15];
    // 18001a5e0: lea r8, [rsp + 0x70]
    G[8]=(uint64_t)(0 + G[4] + 112ull);
    // 18001a5e5: mov rdx, r12
    G[2]=G[12];
    // 18001a5e8: shl rcx, 0x4
    { uint64_t a=G[1], b=4ull, r=(((G[1])<<(4ull))&0xFFFFFFFFFFFFFFFFull); F.set(4,64,r,a,b);
      G[1]=r;
    }
    // 18001a5ec: sub rdx, r15
    { uint64_t a=G[2], b=G[15], r=(((G[2])-(G[15]))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[2]=r;
    }
    // 18001a5ef: sub r8, r10
    { uint64_t a=G[8], b=G[10], r=(((G[8])-(G[10]))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[8]=r;
    }
    // 18001a5f2: add rcx, r10
    { uint64_t a=G[1], b=G[10], r=(((G[1])+(G[10]))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[1]=r;
    }
    // 18001a5f5: inc rdx
    { uint64_t a=G[2], b=1ull, r=(((G[2])+(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(3,64,r,a,b);
      G[2]=r;
    }
L_18001a5f8:;
    // 18001a5f8: mov rax, qword ptr [rcx + r8]
    G[0]=RD64((uint64_t)(0 + G[1] + G[8]));
    // 18001a5fc: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a601: add rcx, 0x10
    { uint64_t a=G[1], b=16ull, r=(((G[1])+(16ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[1]=r;
    }
    // 18001a605: sub rdx, 0x1
    { uint64_t a=G[2], b=1ull, r=(((G[2])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[2]=r;
    }
    // 18001a609: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001a60c: mov rax, qword ptr [rcx + r8 - 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + G[8] - 8ull));
    // 18001a611: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001a615: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 18001a61a: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 18001a620: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 18001a626: mulsd xmm0, xmm8
    X[0].d[0]=X[0].d[0] * X[8].d[0];
    // 18001a62b: mulsd xmm1, xmm8
    X[1].d[0]=X[1].d[0] * X[8].d[0];
    // 18001a630: movsd qword ptr [rsp + 0x30], xmm0
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[0].d[0]));
    // 18001a636: movsd qword ptr [rsp + 0x38], xmm1
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[1].d[0]));
    // 18001a63c: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001a63f: mov qword ptr [rcx - 0x10], rax
    WR64((uint64_t)(0 + G[1] - 16ull),G[0]);
    // 18001a643: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 18001a647: mov qword ptr [rcx - 0x8], rax
    WR64((uint64_t)(0 + G[1] - 8ull),G[0]);
    // 18001a64b: jne 0x18001a5f8
    if (!F.zf) goto L_18001a5f8;
L_18001a64d:;
    // 18001a64d: movsd qword ptr [rsp + 0x20], xmm7
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[7].d[0]));
    // 18001a653: movsd qword ptr [rsp + 0x28], xmm7
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[7].d[0]));
    // 18001a659: lea rcx, [rsp + 0x60]
    G[1]=(uint64_t)(0 + G[4] + 96ull);
    // 18001a65e: lea rdx, [rsp + 0x70]
    G[2]=(uint64_t)(0 + G[4] + 112ull);
    // 18001a663: mov r10d, 0x1
    G[10]=(uint64_t)(uint32_t)(1ull);
    // 18001a669: movsd qword ptr [rsp + 0x60], xmm6
    WR64((uint64_t)(0 + G[4] + 96ull),BITS(X[6].d[0]));
    // 18001a66f: movsd qword ptr [rsp + 0x68], xmm7
    WR64((uint64_t)(0 + G[4] + 104ull),BITS(X[7].d[0]));
    // 18001a675: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001a678: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 18001a67b: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001a67f: lea rcx, [rsp + 0x20]
    G[1]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a684: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 18001a688: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001a68b: lea rdx, [rsp + 0x80]
    G[2]=(uint64_t)(0 + G[4] + 128ull);
    // 18001a693: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 18001a696: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001a69a: lea rcx, [rsp + 0x20]
    G[1]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a69f: movsd qword ptr [rsp + 0x20], xmm7
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[7].d[0]));
    // 18001a6a5: movsd qword ptr [rsp + 0x28], xmm7
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[7].d[0]));
    // 18001a6ab: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 18001a6af: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001a6b2: lea rdx, [rsp + 0x90]
    G[2]=(uint64_t)(0 + G[4] + 144ull);
    // 18001a6ba: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 18001a6bd: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001a6c1: lea rcx, [rsp + 0x20]
    G[1]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a6c6: movsd qword ptr [rsp + 0x20], xmm7
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[7].d[0]));
    // 18001a6cc: movsd qword ptr [rsp + 0x28], xmm7
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[7].d[0]));
    // 18001a6d2: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 18001a6d6: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001a6d9: lea rdx, [rsp + 0xa0]
    G[2]=(uint64_t)(0 + G[4] + 160ull);
    // 18001a6e1: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 18001a6e4: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001a6e8: lea rcx, [rsp + 0x20]
    G[1]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a6ed: movsd qword ptr [rsp + 0x20], xmm7
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[7].d[0]));
    // 18001a6f3: movsd qword ptr [rsp + 0x28], xmm7
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[7].d[0]));
    // 18001a6f9: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 18001a6fd: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001a700: lea rdx, [rsp + 0xb0]
    G[2]=(uint64_t)(0 + G[4] + 176ull);
    // 18001a708: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 18001a70b: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001a70f: lea rcx, [rsp + 0x20]
    G[1]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a714: movsd qword ptr [rsp + 0x20], xmm7
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[7].d[0]));
    // 18001a71a: movsd qword ptr [rsp + 0x28], xmm7
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[7].d[0]));
    // 18001a720: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 18001a724: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001a727: lea rdx, [rsp + 0xc0]
    G[2]=(uint64_t)(0 + G[4] + 192ull);
    // 18001a72f: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 18001a732: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001a736: lea rcx, [rsp + 0x20]
    G[1]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a73b: movsd qword ptr [rsp + 0x20], xmm7
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[7].d[0]));
    // 18001a741: movsd qword ptr [rsp + 0x28], xmm7
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[7].d[0]));
    // 18001a747: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 18001a74b: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001a74e: lea rdx, [rsp + 0xd0]
    G[2]=(uint64_t)(0 + G[4] + 208ull);
    // 18001a756: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 18001a759: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001a75d: lea rcx, [rsp + 0x20]
    G[1]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a762: movsd qword ptr [rsp + 0x20], xmm7
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[7].d[0]));
    // 18001a768: movsd qword ptr [rsp + 0x28], xmm7
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[7].d[0]));
    // 18001a76e: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 18001a772: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001a775: lea rdx, [rsp + 0xe0]
    G[2]=(uint64_t)(0 + G[4] + 224ull);
    // 18001a77d: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 18001a780: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001a784: lea rcx, [rsp + 0x20]
    G[1]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a789: movsd qword ptr [rsp + 0x20], xmm7
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[7].d[0]));
    // 18001a78f: movsd qword ptr [rsp + 0x28], xmm7
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[7].d[0]));
    // 18001a795: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 18001a799: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001a79c: lea rdx, [rsp + 0xf0]
    G[2]=(uint64_t)(0 + G[4] + 240ull);
    // 18001a7a4: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 18001a7a7: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001a7ab: lea rcx, [rsp + 0x20]
    G[1]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a7b0: movsd qword ptr [rsp + 0x20], xmm7
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[7].d[0]));
    // 18001a7b6: movsd qword ptr [rsp + 0x28], xmm7
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[7].d[0]));
    // 18001a7bc: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 18001a7c0: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001a7c3: lea rdx, [rsp + 0x100]
    G[2]=(uint64_t)(0 + G[4] + 256ull);
    // 18001a7cb: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 18001a7ce: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001a7d2: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 18001a7d6: mov eax, dword ptr [rsp + 0x1f8]
    G[0]=(uint64_t)(uint32_t)(RD32((uint64_t)(0 + G[4] + 504ull)));
    // 18001a7dd: mov r15, qword ptr [rsp + 0x1b0]
    G[15]=RD64((uint64_t)(0 + G[4] + 432ull));
    // 18001a7e5: mov rdi, r10
    G[7]=G[10];
    // 18001a7e8: cmp eax, edi
    { uint64_t a=(G[0]&0xFFFFFFFFull), b=(G[7]&0xFFFFFFFFull), r=((((G[0]&0xFFFFFFFFull))-((G[7]&0xFFFFFFFFull)))&0xFFFFFFFFull); F.set(2,32,r,a,b);
    }
    // 18001a7ea: jl 0x18001ac4f
    if ((F.sf!=F.of)) goto L_18001ac4f;
    // 18001a7f0: mov rsi, qword ptr [rsp + 0x1f0]
    G[6]=RD64((uint64_t)(0 + G[4] + 496ull));
    // 18001a7f8: divsd xmm2, xmm12
    X[2].d[0]=X[2].d[0] / X[12].d[0];
    // 18001a7fd: lea rbp, [rsp + 0x70]
    G[5]=(uint64_t)(0 + G[4] + 112ull);
    // 18001a802: add rsi, 0x10
    { uint64_t a=G[6], b=16ull, r=(((G[6])+(16ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[6]=r;
    }
    // 18001a806: mov r13, rax
    G[13]=G[0];
    // 18001a809: sub rbp, 0x10
    { uint64_t a=G[5], b=16ull, r=(((G[5])-(16ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[5]=r;
    }
    // 18001a80d: divsd xmm15, xmm12
    X[15].d[0]=X[15].d[0] / X[12].d[0];
    // 18001a812: movsd qword ptr [rsp + 0x210], xmm2
    WR64((uint64_t)(0 + G[4] + 528ull),BITS(X[2].d[0]));
    // 18001a81b: nop dword ptr [rax + rax]
L_18001a820:;
    // 18001a820: mov rax, qword ptr [rsi]
    G[0]=RD64((uint64_t)(0 + G[6]));
    // 18001a823: lea rcx, [rsp + 0x60]
    G[1]=(uint64_t)(0 + G[4] + 96ull);
    // 18001a828: movapd xmm8, xmm2
    X[8]=X[2];
    // 18001a82d: mov qword ptr [rcx], rax
    WR64((uint64_t)(0 + G[1]),G[0]);
    // 18001a830: mov rax, qword ptr [rsi + 0x8]
    G[0]=RD64((uint64_t)(0 + G[6] + 8ull));
    // 18001a834: movapd xmm10, xmm7
    X[10]=X[7];
    // 18001a839: movapd xmm6, xmm15
    X[6]=X[15];
    // 18001a83e: movapd xmm9, xmm7
    X[9]=X[7];
    // 18001a843: mov qword ptr [rcx + 0x8], rax
    WR64((uint64_t)(0 + G[1] + 8ull),G[0]);
    // 18001a847: mov rax, qword ptr [rsi]
    G[0]=RD64((uint64_t)(0 + G[6]));
    // 18001a84a: lea rcx, [rsp + 0x50]
    G[1]=(uint64_t)(0 + G[4] + 80ull);
    // 18001a84f: subsd xmm6, qword ptr [rsp + 0x60]
    X[6].d[0]=X[6].d[0] - DBL(RD64((uint64_t)(0 + G[4] + 96ull)));
    // 18001a855: subsd xmm9, qword ptr [rsp + 0x68]
    X[9].d[0]=X[9].d[0] - DBL(RD64((uint64_t)(0 + G[4] + 104ull)));
    // 18001a85c: mov qword ptr [rcx], rax
    WR64((uint64_t)(0 + G[1]),G[0]);
    // 18001a85f: mov rax, qword ptr [rsi + 0x8]
    G[0]=RD64((uint64_t)(0 + G[6] + 8ull));
    // 18001a863: mov qword ptr [rcx + 0x8], rax
    WR64((uint64_t)(0 + G[1] + 8ull),G[0]);
    // 18001a867: subsd xmm8, qword ptr [rsp + 0x50]
    X[8].d[0]=X[8].d[0] - DBL(RD64((uint64_t)(0 + G[4] + 80ull)));
    // 18001a86e: subsd xmm10, qword ptr [rsp + 0x58]
    X[10].d[0]=X[10].d[0] - DBL(RD64((uint64_t)(0 + G[4] + 88ull)));
    // 18001a875: movapd xmm1, xmm10
    X[1]=X[10];
    // 18001a87a: movapd xmm0, xmm8
    X[0]=X[8];
    // 18001a87f: mulsd xmm0, xmm8
    X[0].d[0]=X[0].d[0] * X[8].d[0];
    // 18001a884: mulsd xmm1, xmm10
    X[1].d[0]=X[1].d[0] * X[10].d[0];
    // 18001a889: addsd xmm0, xmm1
    X[0].d[0]=X[0].d[0] + X[1].d[0];
    // 18001a88d: call 0x1800bf0a6
    X[0].d[0]=std::sqrt(X[0].d[0]);
    // 18001a892: movapd xmm1, xmm9
    X[1]=X[9];
    // 18001a897: mulsd xmm1, xmm9
    X[1].d[0]=X[1].d[0] * X[9].d[0];
    // 18001a89c: movapd xmm11, xmm0
    X[11]=X[0];
    // 18001a8a1: movapd xmm0, xmm6
    X[0]=X[6];
    // 18001a8a5: mulsd xmm0, xmm6
    X[0].d[0]=X[0].d[0] * X[6].d[0];
    // 18001a8a9: addsd xmm0, xmm1
    X[0].d[0]=X[0].d[0] + X[1].d[0];
    // 18001a8ad: call 0x1800bf0a6
    X[0].d[0]=std::sqrt(X[0].d[0]);
    // 18001a8b2: ucomisd xmm8, xmm7
    F.ucom(X[8].d[0], X[7].d[0]);
    // 18001a8b7: divsd xmm11, xmm0
    X[11].d[0]=X[11].d[0] / X[0].d[0];
    // 18001a8bc: jp 0x18001a8c0
    if (F.pf) goto L_18001a8c0;
    // 18001a8be: je 0x18001a8c8
    if (F.zf) goto L_18001a8c8;
L_18001a8c0:;
    // 18001a8c0: ucomisd xmm6, xmm7
    F.ucom(X[6].d[0], X[7].d[0]);
    // 18001a8c4: jp 0x18001a8cf
    if (F.pf) goto L_18001a8cf;
    // 18001a8c6: jne 0x18001a8cf
    if (!F.zf) goto L_18001a8cf;
L_18001a8c8:;
    // 18001a8c8: movapd xmm8, xmm7
    X[8]=X[7];
    // 18001a8cd: jmp 0x18001a8f6
    goto L_18001a8f6;
L_18001a8cf:;
    // 18001a8cf: movapd xmm1, xmm8
    X[1]=X[8];
    // 18001a8d4: movapd xmm0, xmm10
    X[0]=X[10];
    // 18001a8d9: call 0x1800bf0ac
    X[0].d[0]=std::atan2(X[0].d[0],X[1].d[0]);
    // 18001a8de: movapd xmm1, xmm6
    X[1]=X[6];
    // 18001a8e2: movapd xmm8, xmm0
    X[8]=X[0];
    // 18001a8e7: movapd xmm0, xmm9
    X[0]=X[9];
    // 18001a8ec: call 0x1800bf0ac
    X[0].d[0]=std::atan2(X[0].d[0],X[1].d[0]);
    // 18001a8f1: subsd xmm8, xmm0
    X[8].d[0]=X[8].d[0] - X[0].d[0];
L_18001a8f6:;
    // 18001a8f6: movapd xmm0, xmm8
    X[0]=X[8];
    // 18001a8fb: call 0x1800bf09a
    X[0].d[0]=std::cos(X[0].d[0]);
    // 18001a900: movapd xmm6, xmm0
    X[6]=X[0];
    // 18001a904: movapd xmm0, xmm8
    X[0]=X[8];
    // 18001a909: mulsd xmm6, xmm11
    X[6].d[0]=X[6].d[0] * X[11].d[0];
    // 18001a90e: call 0x1800bf0a0
    X[0].d[0]=std::sin(X[0].d[0]);
    // 18001a913: cmp rdi, 0x4
    { uint64_t a=G[7], b=4ull, r=(((G[7])-(4ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
    }
    // 18001a917: mov r8, rdi
    G[8]=G[7];
    // 18001a91a: movapd xmm5, xmm0
    X[5]=X[0];
    // 18001a91e: mulsd xmm5, xmm11
    X[5].d[0]=X[5].d[0] * X[11].d[0];
    // 18001a923: jl 0x18001ab7c
    if ((F.sf!=F.of)) goto L_18001ab7c;
    // 18001a929: mov rdx, rdi
    G[2]=G[7];
    // 18001a92c: mov rcx, rbp
    G[1]=G[5];
    // 18001a92f: shr rdx, 0x2
    { uint64_t a=G[2], b=2ull, r=(((G[2])>>(2ull))&0xFFFFFFFFFFFFFFFFull); F.set(4,64,r,a,b);
      G[2]=r;
    }
    // 18001a933: lea rax, [4*rdx]
    G[0]=(uint64_t)(0 + (G[2]*4));
    // 18001a93b: sub r8, rax
    { uint64_t a=G[8], b=G[0], r=(((G[8])-(G[0]))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[8]=r;
    }
    // 18001a93e: nop 
L_18001a940:;
    // 18001a940: mov rax, qword ptr [rcx + 0x10]
    G[0]=RD64((uint64_t)(0 + G[1] + 16ull));
    // 18001a944: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a949: movapd xmm3, xmm5
    X[3]=X[5];
    // 18001a94d: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001a950: mov rax, qword ptr [rcx + 0x18]
    G[0]=RD64((uint64_t)(0 + G[1] + 24ull));
    // 18001a954: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001a958: mov rax, qword ptr [rcx + 0x20]
    G[0]=RD64((uint64_t)(0 + G[1] + 32ull));
    // 18001a95c: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 18001a961: movsd xmm1, qword ptr [rsp + 0x20]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[1].d[1]=0.0;
    // 18001a967: movsd xmm2, qword ptr [rsp + 0x28]
    X[2].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[2].d[1]=0.0;
    // 18001a96d: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001a970: mov rax, qword ptr [rcx + 0x28]
    G[0]=RD64((uint64_t)(0 + G[1] + 40ull));
    // 18001a974: mulsd xmm3, xmm1
    X[3].d[0]=X[3].d[0] * X[1].d[0];
    // 18001a978: movapd xmm4, xmm1
    X[4]=X[1];
    // 18001a97c: movapd xmm0, xmm2
    X[0]=X[2];
    // 18001a980: mulsd xmm2, xmm6
    X[2].d[0]=X[2].d[0] * X[6].d[0];
    // 18001a984: addsd xmm3, xmm2
    X[3].d[0]=X[3].d[0] + X[2].d[0];
    // 18001a988: mulsd xmm0, xmm5
    X[0].d[0]=X[0].d[0] * X[5].d[0];
    // 18001a98c: mulsd xmm4, xmm6
    X[4].d[0]=X[4].d[0] * X[6].d[0];
    // 18001a990: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001a994: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 18001a999: subsd xmm4, xmm0
    X[4].d[0]=X[4].d[0] - X[0].d[0];
    // 18001a99d: movsd xmm0, qword ptr [rsp + 0x30]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 48ull))); X[0].d[1]=0.0;
    // 18001a9a3: movsd xmm1, qword ptr [rsp + 0x38]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 56ull))); X[1].d[1]=0.0;
    // 18001a9a9: addsd xmm0, xmm4
    X[0].d[0]=X[0].d[0] + X[4].d[0];
    // 18001a9ad: addsd xmm1, xmm3
    X[1].d[0]=X[1].d[0] + X[3].d[0];
    // 18001a9b1: movapd xmm3, xmm5
    X[3]=X[5];
    // 18001a9b5: movsd qword ptr [rsp + 0x40], xmm0
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[0].d[0]));
    // 18001a9bb: movsd qword ptr [rsp + 0x48], xmm1
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[1].d[0]));
    // 18001a9c1: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001a9c4: mov qword ptr [rcx + 0x20], rax
    WR64((uint64_t)(0 + G[1] + 32ull),G[0]);
    // 18001a9c8: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 18001a9cc: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 18001a9d1: mov qword ptr [rcx + 0x28], rax
    WR64((uint64_t)(0 + G[1] + 40ull),G[0]);
    // 18001a9d5: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001a9d8: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001a9db: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001a9df: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001a9e3: mov rax, qword ptr [rcx + 0x10]
    G[0]=RD64((uint64_t)(0 + G[1] + 16ull));
    // 18001a9e7: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 18001a9ec: movsd xmm1, qword ptr [rsp + 0x20]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[1].d[1]=0.0;
    // 18001a9f2: movsd xmm2, qword ptr [rsp + 0x28]
    X[2].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[2].d[1]=0.0;
    // 18001a9f8: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001a9fb: mov rax, qword ptr [rcx + 0x18]
    G[0]=RD64((uint64_t)(0 + G[1] + 24ull));
    // 18001a9ff: mulsd xmm3, xmm1
    X[3].d[0]=X[3].d[0] * X[1].d[0];
    // 18001aa03: movapd xmm4, xmm1
    X[4]=X[1];
    // 18001aa07: movapd xmm0, xmm2
    X[0]=X[2];
    // 18001aa0b: mulsd xmm2, xmm6
    X[2].d[0]=X[2].d[0] * X[6].d[0];
    // 18001aa0f: addsd xmm3, xmm2
    X[3].d[0]=X[3].d[0] + X[2].d[0];
    // 18001aa13: mulsd xmm0, xmm5
    X[0].d[0]=X[0].d[0] * X[5].d[0];
    // 18001aa17: mulsd xmm4, xmm6
    X[4].d[0]=X[4].d[0] * X[6].d[0];
    // 18001aa1b: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001aa1f: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 18001aa24: subsd xmm4, xmm0
    X[4].d[0]=X[4].d[0] - X[0].d[0];
    // 18001aa28: movsd xmm0, qword ptr [rsp + 0x30]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 48ull))); X[0].d[1]=0.0;
    // 18001aa2e: movsd xmm1, qword ptr [rsp + 0x38]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 56ull))); X[1].d[1]=0.0;
    // 18001aa34: addsd xmm0, xmm4
    X[0].d[0]=X[0].d[0] + X[4].d[0];
    // 18001aa38: addsd xmm1, xmm3
    X[1].d[0]=X[1].d[0] + X[3].d[0];
    // 18001aa3c: movapd xmm3, xmm5
    X[3]=X[5];
    // 18001aa40: movsd qword ptr [rsp + 0x40], xmm0
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[0].d[0]));
    // 18001aa46: movsd qword ptr [rsp + 0x48], xmm1
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[1].d[0]));
    // 18001aa4c: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001aa4f: mov qword ptr [rcx + 0x10], rax
    WR64((uint64_t)(0 + G[1] + 16ull),G[0]);
    // 18001aa53: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 18001aa57: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 18001aa5c: mov qword ptr [rcx + 0x18], rax
    WR64((uint64_t)(0 + G[1] + 24ull),G[0]);
    // 18001aa60: mov rax, qword ptr [rcx - 0x10]
    G[0]=RD64((uint64_t)(0 + G[1] - 16ull));
    // 18001aa64: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001aa67: mov rax, qword ptr [rcx - 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] - 8ull));
    // 18001aa6b: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001aa6f: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 18001aa74: movsd xmm1, qword ptr [rsp + 0x20]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[1].d[1]=0.0;
    // 18001aa7a: movsd xmm2, qword ptr [rsp + 0x28]
    X[2].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[2].d[1]=0.0;
    // 18001aa80: movapd xmm0, xmm2
    X[0]=X[2];
    // 18001aa84: movapd xmm4, xmm1
    X[4]=X[1];
    // 18001aa88: mulsd xmm3, xmm1
    X[3].d[0]=X[3].d[0] * X[1].d[0];
    // 18001aa8c: mulsd xmm2, xmm6
    X[2].d[0]=X[2].d[0] * X[6].d[0];
    // 18001aa90: mulsd xmm4, xmm6
    X[4].d[0]=X[4].d[0] * X[6].d[0];
    // 18001aa94: mulsd xmm0, xmm5
    X[0].d[0]=X[0].d[0] * X[5].d[0];
    // 18001aa98: addsd xmm3, xmm2
    X[3].d[0]=X[3].d[0] + X[2].d[0];
    // 18001aa9c: subsd xmm4, xmm0
    X[4].d[0]=X[4].d[0] - X[0].d[0];
    // 18001aaa0: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001aaa3: sub rcx, 0x40
    { uint64_t a=G[1], b=64ull, r=(((G[1])-(64ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[1]=r;
    }
    // 18001aaa7: sub rdx, 0x1
    { uint64_t a=G[2], b=1ull, r=(((G[2])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[2]=r;
    }
    // 18001aaab: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001aaae: mov rax, qword ptr [rcx + 0x48]
    G[0]=RD64((uint64_t)(0 + G[1] + 72ull));
    // 18001aab2: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001aab6: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 18001aabb: movsd xmm0, qword ptr [rsp + 0x30]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 48ull))); X[0].d[1]=0.0;
    // 18001aac1: movsd xmm1, qword ptr [rsp + 0x38]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 56ull))); X[1].d[1]=0.0;
    // 18001aac7: addsd xmm0, xmm4
    X[0].d[0]=X[0].d[0] + X[4].d[0];
    // 18001aacb: addsd xmm1, xmm3
    X[1].d[0]=X[1].d[0] + X[3].d[0];
    // 18001aacf: movapd xmm3, xmm5
    X[3]=X[5];
    // 18001aad3: movsd qword ptr [rsp + 0x40], xmm0
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[0].d[0]));
    // 18001aad9: movsd qword ptr [rsp + 0x48], xmm1
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[1].d[0]));
    // 18001aadf: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001aae2: mov qword ptr [rcx + 0x40], rax
    WR64((uint64_t)(0 + G[1] + 64ull),G[0]);
    // 18001aae6: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 18001aaea: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 18001aaef: mov qword ptr [rcx + 0x48], rax
    WR64((uint64_t)(0 + G[1] + 72ull),G[0]);
    // 18001aaf3: mov rax, qword ptr [rcx + 0x20]
    G[0]=RD64((uint64_t)(0 + G[1] + 32ull));
    // 18001aaf7: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001aafa: mov rax, qword ptr [rcx + 0x28]
    G[0]=RD64((uint64_t)(0 + G[1] + 40ull));
    // 18001aafe: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001ab02: mov rax, qword ptr [rcx + 0x30]
    G[0]=RD64((uint64_t)(0 + G[1] + 48ull));
    // 18001ab06: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 18001ab0b: movsd xmm1, qword ptr [rsp + 0x20]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[1].d[1]=0.0;
    // 18001ab11: movsd xmm2, qword ptr [rsp + 0x28]
    X[2].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[2].d[1]=0.0;
    // 18001ab17: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001ab1a: mov rax, qword ptr [rcx + 0x38]
    G[0]=RD64((uint64_t)(0 + G[1] + 56ull));
    // 18001ab1e: movapd xmm4, xmm1
    X[4]=X[1];
    // 18001ab22: mulsd xmm3, xmm1
    X[3].d[0]=X[3].d[0] * X[1].d[0];
    // 18001ab26: movapd xmm0, xmm2
    X[0]=X[2];
    // 18001ab2a: mulsd xmm4, xmm6
    X[4].d[0]=X[4].d[0] * X[6].d[0];
    // 18001ab2e: mulsd xmm2, xmm6
    X[2].d[0]=X[2].d[0] * X[6].d[0];
    // 18001ab32: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001ab36: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 18001ab3b: addsd xmm3, xmm2
    X[3].d[0]=X[3].d[0] + X[2].d[0];
    // 18001ab3f: mulsd xmm0, xmm5
    X[0].d[0]=X[0].d[0] * X[5].d[0];
    // 18001ab43: movsd xmm1, qword ptr [rsp + 0x38]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 56ull))); X[1].d[1]=0.0;
    // 18001ab49: subsd xmm4, xmm0
    X[4].d[0]=X[4].d[0] - X[0].d[0];
    // 18001ab4d: movsd xmm0, qword ptr [rsp + 0x30]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 48ull))); X[0].d[1]=0.0;
    // 18001ab53: addsd xmm1, xmm3
    X[1].d[0]=X[1].d[0] + X[3].d[0];
    // 18001ab57: movsd qword ptr [rsp + 0x48], xmm1
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[1].d[0]));
    // 18001ab5d: addsd xmm0, xmm4
    X[0].d[0]=X[0].d[0] + X[4].d[0];
    // 18001ab61: movsd qword ptr [rsp + 0x40], xmm0
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[0].d[0]));
    // 18001ab67: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001ab6a: mov qword ptr [rcx + 0x30], rax
    WR64((uint64_t)(0 + G[1] + 48ull),G[0]);
    // 18001ab6e: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 18001ab72: mov qword ptr [rcx + 0x38], rax
    WR64((uint64_t)(0 + G[1] + 56ull),G[0]);
    // 18001ab76: jne 0x18001a940
    if (!F.zf) goto L_18001a940;
L_18001ab7c:;
    // 18001ab7c: cmp r8, 0x1
    { uint64_t a=G[8], b=1ull, r=(((G[8])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
    }
    // 18001ab80: jl 0x18001ac2b
    if ((F.sf!=F.of)) goto L_18001ac2b;
    // 18001ab86: mov rax, r8
    G[0]=G[8];
    // 18001ab89: add rax, rax
    { uint64_t a=G[0], b=G[0], r=(((G[0])+(G[0]))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[0]=r;
    }
    // 18001ab8c: lea rdx, [rsp + 8*rax + 0x70]
    G[2]=(uint64_t)(0 + G[4] + (G[0]*8) + 112ull);
L_18001ab91:;
    // 18001ab91: mov rax, qword ptr [rdx - 0x10]
    G[0]=RD64((uint64_t)(0 + G[2] - 16ull));
    // 18001ab95: lea r9, [rsp + 0x20]
    G[9]=(uint64_t)(0 + G[4] + 32ull);
    // 18001ab9a: movapd xmm3, xmm5
    X[3]=X[5];
    // 18001ab9e: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001aba1: mov rax, qword ptr [rdx - 0x8]
    G[0]=RD64((uint64_t)(0 + G[2] - 8ull));
    // 18001aba5: add rdx, -0x10
    { uint64_t a=G[2], b=18446744073709551600ull, r=(((G[2])+(18446744073709551600ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[2]=r;
    }
    // 18001aba9: sub r8, 0x1
    { uint64_t a=G[8], b=1ull, r=(((G[8])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[8]=r;
    }
    // 18001abad: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001abb1: mov rax, qword ptr [rdx + 0x10]
    G[0]=RD64((uint64_t)(0 + G[2] + 16ull));
    // 18001abb5: movsd xmm1, qword ptr [rsp + 0x20]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[1].d[1]=0.0;
    // 18001abbb: movsd xmm2, qword ptr [rsp + 0x28]
    X[2].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[2].d[1]=0.0;
    // 18001abc1: lea r9, [rsp + 0x30]
    G[9]=(uint64_t)(0 + G[4] + 48ull);
    // 18001abc6: movapd xmm4, xmm1
    X[4]=X[1];
    // 18001abca: mulsd xmm3, xmm1
    X[3].d[0]=X[3].d[0] * X[1].d[0];
    // 18001abce: movapd xmm0, xmm2
    X[0]=X[2];
    // 18001abd2: mulsd xmm2, xmm6
    X[2].d[0]=X[2].d[0] * X[6].d[0];
    // 18001abd6: mulsd xmm0, xmm5
    X[0].d[0]=X[0].d[0] * X[5].d[0];
    // 18001abda: mulsd xmm4, xmm6
    X[4].d[0]=X[4].d[0] * X[6].d[0];
    // 18001abde: addsd xmm3, xmm2
    X[3].d[0]=X[3].d[0] + X[2].d[0];
    // 18001abe2: subsd xmm4, xmm0
    X[4].d[0]=X[4].d[0] - X[0].d[0];
    // 18001abe6: mov qword ptr [r9], rax
    WR64((uint64_t)(0 + G[9]),G[0]);
    // 18001abe9: mov rax, qword ptr [rdx + 0x18]
    G[0]=RD64((uint64_t)(0 + G[2] + 24ull));
    // 18001abed: mov qword ptr [r9 + 0x8], rax
    WR64((uint64_t)(0 + G[9] + 8ull),G[0]);
    // 18001abf1: lea r9, [rsp + 0x40]
    G[9]=(uint64_t)(0 + G[4] + 64ull);
    // 18001abf6: movsd xmm0, qword ptr [rsp + 0x30]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 48ull))); X[0].d[1]=0.0;
    // 18001abfc: movsd xmm1, qword ptr [rsp + 0x38]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 56ull))); X[1].d[1]=0.0;
    // 18001ac02: addsd xmm0, xmm4
    X[0].d[0]=X[0].d[0] + X[4].d[0];
    // 18001ac06: addsd xmm1, xmm3
    X[1].d[0]=X[1].d[0] + X[3].d[0];
    // 18001ac0a: movsd qword ptr [rsp + 0x40], xmm0
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[0].d[0]));
    // 18001ac10: movsd qword ptr [rsp + 0x48], xmm1
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[1].d[0]));
    // 18001ac16: mov rax, qword ptr [r9]
    G[0]=RD64((uint64_t)(0 + G[9]));
    // 18001ac19: mov qword ptr [rdx + 0x10], rax
    WR64((uint64_t)(0 + G[2] + 16ull),G[0]);
    // 18001ac1d: mov rax, qword ptr [r9 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[9] + 8ull));
    // 18001ac21: mov qword ptr [rdx + 0x18], rax
    WR64((uint64_t)(0 + G[2] + 24ull),G[0]);
    // 18001ac25: jne 0x18001ab91
    if (!F.zf) goto L_18001ab91;
L_18001ac2b:;
    // 18001ac2b: movsd xmm2, qword ptr [rsp + 0x210]
    X[2].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 528ull))); X[2].d[1]=0.0;
    // 18001ac34: inc rdi
    { uint64_t a=G[7], b=1ull, r=(((G[7])+(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(3,64,r,a,b);
      G[7]=r;
    }
    // 18001ac37: add rsi, 0x10
    { uint64_t a=G[6], b=16ull, r=(((G[6])+(16ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[6]=r;
    }
    // 18001ac3b: add rbp, 0x10
    { uint64_t a=G[5], b=16ull, r=(((G[5])+(16ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[5]=r;
    }
    // 18001ac3f: sub r13, 0x1
    { uint64_t a=G[13], b=1ull, r=(((G[13])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[13]=r;
    }
    // 18001ac43: jne 0x18001a820
    if (!F.zf) goto L_18001a820;
    // 18001ac49: mov r10d, 0x1
    G[10]=(uint64_t)(uint32_t)(1ull);
L_18001ac4f:;
    // 18001ac4f: cmp r12, 0x4
    { uint64_t a=G[12], b=4ull, r=(((G[12])-(4ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
    }
    // 18001ac53: movsd xmm2, qword ptr [rip + 0x25771d] # 0x180272378
    X[2].d[0]=KD(0xbff0000000000000ull); X[2].d[1]=0.0;
    // 18001ac5b: movaps xmm15, xmmword ptr [rsp + 0x110]
    X[15].u[0]=RD64((uint64_t)(0 + G[4] + 272ull)); X[15].u[1]=RD64((uint64_t)(0 + G[4] + 272ull)+8);
    // 18001ac64: movaps xmm12, xmmword ptr [rsp + 0x140]
    X[12].u[0]=RD64((uint64_t)(0 + G[4] + 320ull)); X[12].u[1]=RD64((uint64_t)(0 + G[4] + 320ull)+8);
    // 18001ac6d: movaps xmm11, xmmword ptr [rsp + 0x150]
    X[11].u[0]=RD64((uint64_t)(0 + G[4] + 336ull)); X[11].u[1]=RD64((uint64_t)(0 + G[4] + 336ull)+8);
    // 18001ac76: movaps xmm10, xmmword ptr [rsp + 0x160]
    X[10].u[0]=RD64((uint64_t)(0 + G[4] + 352ull)); X[10].u[1]=RD64((uint64_t)(0 + G[4] + 352ull)+8);
    // 18001ac7f: mov r13, qword ptr [rsp + 0x1c0]
    G[13]=RD64((uint64_t)(0 + G[4] + 448ull));
    // 18001ac87: mov rsi, qword ptr [rsp + 0x1d0]
    G[6]=RD64((uint64_t)(0 + G[4] + 464ull));
    // 18001ac8f: mov rbp, qword ptr [rsp + 0x1d8]
    G[5]=RD64((uint64_t)(0 + G[4] + 472ull));
    // 18001ac97: movaps xmm9, xmmword ptr [rsp + 0x170]
    X[9].u[0]=RD64((uint64_t)(0 + G[4] + 368ull)); X[9].u[1]=RD64((uint64_t)(0 + G[4] + 368ull)+8);
    // 18001aca0: movaps xmm8, xmmword ptr [rsp + 0x180]
    X[8].u[0]=RD64((uint64_t)(0 + G[4] + 384ull)); X[8].u[1]=RD64((uint64_t)(0 + G[4] + 384ull)+8);
    // 18001aca9: movaps xmm7, xmmword ptr [rsp + 0x190]
    X[7].u[0]=RD64((uint64_t)(0 + G[4] + 400ull)); X[7].u[1]=RD64((uint64_t)(0 + G[4] + 400ull)+8);
    // 18001acb1: movaps xmm6, xmmword ptr [rsp + 0x1a0]
    X[6].u[0]=RD64((uint64_t)(0 + G[4] + 416ull)); X[6].u[1]=RD64((uint64_t)(0 + G[4] + 416ull)+8);
    // 18001acb9: jl 0x18001ae38
    if ((F.sf!=F.of)) goto L_18001ae38;
    // 18001acbf: mov r11, qword ptr [rsp + 0x220]
    G[11]=RD64((uint64_t)(0 + G[4] + 544ull));
    // 18001acc7: lea r8, [rsp + 0x70]
    G[8]=(uint64_t)(0 + G[4] + 112ull);
    // 18001accc: lea r9, [rsp + 0x80]
    G[9]=(uint64_t)(0 + G[4] + 128ull);
    // 18001acd4: mov rdx, r12
    G[2]=G[12];
    // 18001acd7: sub r8, r11
    { uint64_t a=G[8], b=G[11], r=(((G[8])-(G[11]))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[8]=r;
    }
    // 18001acda: sub r9, r11
    { uint64_t a=G[9], b=G[11], r=(((G[9])-(G[11]))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[9]=r;
    }
    // 18001acdd: shr rdx, 0x2
    { uint64_t a=G[2], b=2ull, r=(((G[2])>>(2ull))&0xFFFFFFFFFFFFFFFFull); F.set(4,64,r,a,b);
      G[2]=r;
    }
    // 18001ace1: lea rcx, [rsp + 0x90]
    G[1]=(uint64_t)(0 + G[4] + 144ull);
    // 18001ace9: lea r10, [4*rdx + 0x1]
    G[10]=(uint64_t)(0 + (G[2]*4) + 1ull);
    // 18001acf1: nop word ptr [rax + rax]
L_18001ad00:;
    // 18001ad00: mov rax, qword ptr [rcx - 0x10]
    G[0]=RD64((uint64_t)(0 + G[1] - 16ull));
    // 18001ad04: lea rdi, [rsp + 0x20]
    G[7]=(uint64_t)(0 + G[4] + 32ull);
    // 18001ad09: add rbx, 0x40
    { uint64_t a=G[3], b=64ull, r=(((G[3])+(64ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[3]=r;
    }
    // 18001ad0d: mov qword ptr [rdi], rax
    WR64((uint64_t)(0 + G[7]),G[0]);
    // 18001ad10: mov rax, qword ptr [rcx - 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] - 8ull));
    // 18001ad14: add rcx, 0x40
    { uint64_t a=G[1], b=64ull, r=(((G[1])+(64ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[1]=r;
    }
    // 18001ad18: sub rdx, 0x1
    { uint64_t a=G[2], b=1ull, r=(((G[2])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[2]=r;
    }
    // 18001ad1c: mov qword ptr [rdi + 0x8], rax
    WR64((uint64_t)(0 + G[7] + 8ull),G[0]);
    // 18001ad20: lea rdi, [rsp + 0x30]
    G[7]=(uint64_t)(0 + G[4] + 48ull);
    // 18001ad25: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 18001ad2b: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 18001ad31: mulsd xmm0, xmm2
    X[0].d[0]=X[0].d[0] * X[2].d[0];
    // 18001ad35: mulsd xmm1, xmm2
    X[1].d[0]=X[1].d[0] * X[2].d[0];
    // 18001ad39: movsd qword ptr [rsp + 0x30], xmm0
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[0].d[0]));
    // 18001ad3f: movsd qword ptr [rsp + 0x38], xmm1
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[1].d[0]));
    // 18001ad45: mov rax, qword ptr [rdi]
    G[0]=RD64((uint64_t)(0 + G[7]));
    // 18001ad48: mov qword ptr [rbx - 0x60], rax
    WR64((uint64_t)(0 + G[3] - 96ull),G[0]);
    // 18001ad4c: mov rax, qword ptr [rdi + 0x8]
    G[0]=RD64((uint64_t)(0 + G[7] + 8ull));
    // 18001ad50: lea rdi, [rsp + 0x20]
    G[7]=(uint64_t)(0 + G[4] + 32ull);
    // 18001ad55: mov qword ptr [rbx - 0x58], rax
    WR64((uint64_t)(0 + G[3] - 88ull),G[0]);
    // 18001ad59: mov rax, qword ptr [rcx - 0x40]
    G[0]=RD64((uint64_t)(0 + G[1] - 64ull));
    // 18001ad5d: mov qword ptr [rdi], rax
    WR64((uint64_t)(0 + G[7]),G[0]);
    // 18001ad60: mov rax, qword ptr [rcx - 0x38]
    G[0]=RD64((uint64_t)(0 + G[1] - 56ull));
    // 18001ad64: mov qword ptr [rdi + 0x8], rax
    WR64((uint64_t)(0 + G[7] + 8ull),G[0]);
    // 18001ad68: lea rdi, [rsp + 0x30]
    G[7]=(uint64_t)(0 + G[4] + 48ull);
    // 18001ad6d: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 18001ad73: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 18001ad79: mulsd xmm0, xmm2
    X[0].d[0]=X[0].d[0] * X[2].d[0];
    // 18001ad7d: mulsd xmm1, xmm2
    X[1].d[0]=X[1].d[0] * X[2].d[0];
    // 18001ad81: movsd qword ptr [rsp + 0x30], xmm0
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[0].d[0]));
    // 18001ad87: movsd qword ptr [rsp + 0x38], xmm1
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[1].d[0]));
    // 18001ad8d: mov rax, qword ptr [rdi]
    G[0]=RD64((uint64_t)(0 + G[7]));
    // 18001ad90: mov qword ptr [rbx - 0x50], rax
    WR64((uint64_t)(0 + G[3] - 80ull),G[0]);
    // 18001ad94: mov rax, qword ptr [rdi + 0x8]
    G[0]=RD64((uint64_t)(0 + G[7] + 8ull));
    // 18001ad98: lea rdi, [rsp + 0x20]
    G[7]=(uint64_t)(0 + G[4] + 32ull);
    // 18001ad9d: mov qword ptr [rbx - 0x48], rax
    WR64((uint64_t)(0 + G[3] - 72ull),G[0]);
    // 18001ada1: mov rax, qword ptr [rbx + r8 - 0x40]
    G[0]=RD64((uint64_t)(0 + G[3] + G[8] - 64ull));
    // 18001ada6: mov qword ptr [rdi], rax
    WR64((uint64_t)(0 + G[7]),G[0]);
    // 18001ada9: mov rax, qword ptr [rbx + r8 - 0x38]
    G[0]=RD64((uint64_t)(0 + G[3] + G[8] - 56ull));
    // 18001adae: mov qword ptr [rdi + 0x8], rax
    WR64((uint64_t)(0 + G[7] + 8ull),G[0]);
    // 18001adb2: lea rdi, [rsp + 0x30]
    G[7]=(uint64_t)(0 + G[4] + 48ull);
    // 18001adb7: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 18001adbd: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 18001adc3: mulsd xmm0, xmm2
    X[0].d[0]=X[0].d[0] * X[2].d[0];
    // 18001adc7: mulsd xmm1, xmm2
    X[1].d[0]=X[1].d[0] * X[2].d[0];
    // 18001adcb: movsd qword ptr [rsp + 0x30], xmm0
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[0].d[0]));
    // 18001add1: movsd qword ptr [rsp + 0x38], xmm1
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[1].d[0]));
    // 18001add7: mov rax, qword ptr [rdi]
    G[0]=RD64((uint64_t)(0 + G[7]));
    // 18001adda: mov qword ptr [rbx - 0x40], rax
    WR64((uint64_t)(0 + G[3] - 64ull),G[0]);
    // 18001adde: mov rax, qword ptr [rdi + 0x8]
    G[0]=RD64((uint64_t)(0 + G[7] + 8ull));
    // 18001ade2: lea rdi, [rsp + 0x20]
    G[7]=(uint64_t)(0 + G[4] + 32ull);
    // 18001ade7: mov qword ptr [rbx - 0x38], rax
    WR64((uint64_t)(0 + G[3] - 56ull),G[0]);
    // 18001adeb: mov rax, qword ptr [rbx + r9 - 0x40]
    G[0]=RD64((uint64_t)(0 + G[3] + G[9] - 64ull));
    // 18001adf0: mov qword ptr [rdi], rax
    WR64((uint64_t)(0 + G[7]),G[0]);
    // 18001adf3: mov rax, qword ptr [rbx + r9 - 0x38]
    G[0]=RD64((uint64_t)(0 + G[3] + G[9] - 56ull));
    // 18001adf8: mov qword ptr [rdi + 0x8], rax
    WR64((uint64_t)(0 + G[7] + 8ull),G[0]);
    // 18001adfc: lea rdi, [rsp + 0x30]
    G[7]=(uint64_t)(0 + G[4] + 48ull);
    // 18001ae01: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 18001ae07: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 18001ae0d: mulsd xmm0, xmm2
    X[0].d[0]=X[0].d[0] * X[2].d[0];
    // 18001ae11: mulsd xmm1, xmm2
    X[1].d[0]=X[1].d[0] * X[2].d[0];
    // 18001ae15: movsd qword ptr [rsp + 0x30], xmm0
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[0].d[0]));
    // 18001ae1b: movsd qword ptr [rsp + 0x38], xmm1
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[1].d[0]));
    // 18001ae21: mov rax, qword ptr [rdi]
    G[0]=RD64((uint64_t)(0 + G[7]));
    // 18001ae24: mov qword ptr [rbx - 0x30], rax
    WR64((uint64_t)(0 + G[3] - 48ull),G[0]);
    // 18001ae28: mov rax, qword ptr [rdi + 0x8]
    G[0]=RD64((uint64_t)(0 + G[7] + 8ull));
    // 18001ae2c: mov qword ptr [rbx - 0x28], rax
    WR64((uint64_t)(0 + G[3] - 40ull),G[0]);
    // 18001ae30: jne 0x18001ad00
    if (!F.zf) goto L_18001ad00;
    // 18001ae36: jmp 0x18001ae40
    goto L_18001ae40;
L_18001ae38:;
    // 18001ae38: mov r11, qword ptr [rsp + 0x220]
    G[11]=RD64((uint64_t)(0 + G[4] + 544ull));
L_18001ae40:;
    // 18001ae40: cmp r10, r12
    { uint64_t a=G[10], b=G[12], r=(((G[10])-(G[12]))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
    }
    // 18001ae43: mov rdi, qword ptr [rsp + 0x1c8]
    G[7]=RD64((uint64_t)(0 + G[4] + 456ull));
    // 18001ae4b: mov rbx, qword ptr [rsp + 0x200]
    G[3]=RD64((uint64_t)(0 + G[4] + 512ull));
    // 18001ae53: jg 0x18001aec0
    if ((!F.zf&&F.sf==F.of)) goto L_18001aec0;
    // 18001ae55: mov rcx, r10
    G[1]=G[10];
    // 18001ae58: lea rdx, [rsp + 0x70]
    G[2]=(uint64_t)(0 + G[4] + 112ull);
    // 18001ae5d: sub r12, r10
    { uint64_t a=G[12], b=G[10], r=(((G[12])-(G[10]))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[12]=r;
    }
    // 18001ae60: shl rcx, 0x4
    { uint64_t a=G[1], b=4ull, r=(((G[1])<<(4ull))&0xFFFFFFFFFFFFFFFFull); F.set(4,64,r,a,b);
      G[1]=r;
    }
    // 18001ae64: sub rdx, r11
    { uint64_t a=G[2], b=G[11], r=(((G[2])-(G[11]))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[2]=r;
    }
    // 18001ae67: add rcx, r11
    { uint64_t a=G[1], b=G[11], r=(((G[1])+(G[11]))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[1]=r;
    }
    // 18001ae6a: inc r12
    { uint64_t a=G[12], b=1ull, r=(((G[12])+(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(3,64,r,a,b);
      G[12]=r;
    }
L_18001ae6d:;
    // 18001ae6d: mov rax, qword ptr [rcx + rdx]
    G[0]=RD64((uint64_t)(0 + G[1] + G[2]));
    // 18001ae71: lea r8, [rsp + 0x20]
    G[8]=(uint64_t)(0 + G[4] + 32ull);
    // 18001ae76: add rcx, 0x10
    { uint64_t a=G[1], b=16ull, r=(((G[1])+(16ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[1]=r;
    }
    // 18001ae7a: sub r12, 0x1
    { uint64_t a=G[12], b=1ull, r=(((G[12])-(1ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[12]=r;
    }
    // 18001ae7e: mov qword ptr [r8], rax
    WR64((uint64_t)(0 + G[8]),G[0]);
    // 18001ae81: mov rax, qword ptr [rcx + rdx - 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + G[2] - 8ull));
    // 18001ae86: mov qword ptr [r8 + 0x8], rax
    WR64((uint64_t)(0 + G[8] + 8ull),G[0]);
    // 18001ae8a: lea r8, [rsp + 0x30]
    G[8]=(uint64_t)(0 + G[4] + 48ull);
    // 18001ae8f: movsd xmm0, qword ptr [rsp + 0x20]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 32ull))); X[0].d[1]=0.0;
    // 18001ae95: movsd xmm1, qword ptr [rsp + 0x28]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 40ull))); X[1].d[1]=0.0;
    // 18001ae9b: mulsd xmm0, xmm2
    X[0].d[0]=X[0].d[0] * X[2].d[0];
    // 18001ae9f: mulsd xmm1, xmm2
    X[1].d[0]=X[1].d[0] * X[2].d[0];
    // 18001aea3: movsd qword ptr [rsp + 0x30], xmm0
    WR64((uint64_t)(0 + G[4] + 48ull),BITS(X[0].d[0]));
    // 18001aea9: movsd qword ptr [rsp + 0x38], xmm1
    WR64((uint64_t)(0 + G[4] + 56ull),BITS(X[1].d[0]));
    // 18001aeaf: mov rax, qword ptr [r8]
    G[0]=RD64((uint64_t)(0 + G[8]));
    // 18001aeb2: mov qword ptr [rcx - 0x10], rax
    WR64((uint64_t)(0 + G[1] - 16ull),G[0]);
    // 18001aeb6: mov rax, qword ptr [r8 + 0x8]
    G[0]=RD64((uint64_t)(0 + G[8] + 8ull));
    // 18001aeba: mov qword ptr [rcx - 0x8], rax
    WR64((uint64_t)(0 + G[1] - 8ull),G[0]);
    // 18001aebe: jne 0x18001ae6d
    if (!F.zf) goto L_18001ae6d;
L_18001aec0:;
    // 18001aec0: add rsp, 0x1e0
    { uint64_t a=G[4], b=480ull, r=(((G[4])+(480ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[4]=r;
    }
    // 18001aec7: pop r12
    G[12]=RD64(G[4]);
    G[4]+=8;
    // 18001aec9: ret 
    return;
}

inline void aaDesign1aed0(Regs& R)
{
    uint64_t* G = R.g; XR* X = R.x; Flags& F = R.f;
    // 18001aed0: mov rax, rsp
    G[0]=G[4];
    // 18001aed3: mov qword ptr [rax + 0x10], rbp
    WR64((uint64_t)(0 + G[0] + 16ull),G[5]);
    // 18001aed7: mov qword ptr [rax + 0x18], rsi
    WR64((uint64_t)(0 + G[0] + 24ull),G[6]);
    // 18001aedb: push rdi
    G[4]-=8; WR64(G[4],G[7]);
    // 18001aedc: sub rsp, 0x310
    { uint64_t a=G[4], b=784ull, r=(((G[4])-(784ull))&0xFFFFFFFFFFFFFFFFull); F.set(2,64,r,a,b);
      G[4]=r;
    }
    // 18001aee3: mulsd xmm1, qword ptr [rip + 0x258605] # 0x1802734f0
    X[1].d[0]=X[1].d[0] * KD(0x3feccccccccccccdull);
    // 18001aeeb: movaps xmmword ptr [rax - 0x18], xmm6
    WR64((uint64_t)(0 + G[0] - 24ull),X[6].u[0]); WR64((uint64_t)(0 + G[0] - 24ull)+8,X[6].u[1]);
    // 18001aeef: movaps xmmword ptr [rax - 0x28], xmm7
    WR64((uint64_t)(0 + G[0] - 40ull),X[7].u[0]); WR64((uint64_t)(0 + G[0] - 40ull)+8,X[7].u[1]);
    // 18001aef3: mov rsi, rcx
    G[6]=G[1];
    // 18001aef6: mulsd xmm1, qword ptr [rip + 0x258542] # 0x180273440
    X[1].d[0]=X[1].d[0] * KD(0x400921fb54432708ull);
    // 18001aefe: movaps xmmword ptr [rax - 0x38], xmm8
    WR64((uint64_t)(0 + G[0] - 56ull),X[8].u[0]); WR64((uint64_t)(0 + G[0] - 56ull)+8,X[8].u[1]);
    // 18001af03: movapd xmm0, xmm1
    X[0]=X[1];
    // 18001af07: call 0x1800bf0b8
    X[0].d[0]=std::tan(X[0].d[0]);
    // 18001af0c: lea rcx, [rsp + 0x40]
    G[1]=(uint64_t)(0 + G[4] + 64ull);
    // 18001af11: lea rdx, [rsp + 0x70]
    G[2]=(uint64_t)(0 + G[4] + 112ull);
    // 18001af16: movapd xmm8, xmm0
    X[8]=X[0];
    // 18001af1b: mulsd xmm8, qword ptr [rip + 0x25820c] # 0x180273130
    X[8].d[0]=X[8].d[0] * KD(0x4000000000000000ull);
    // 18001af24: movapd xmm2, xmm8
    X[2]=X[8];
    // 18001af29: movapd xmm1, xmm8
    X[1]=X[8];
    // 18001af2e: movapd xmm0, xmm8
    X[0]=X[8];
    // 18001af33: mulsd xmm0, qword ptr [rip + 0x2585ad] # 0x1802734e8
    X[0].d[0]=X[0].d[0] * KD(0xbfca47baa9b499d0ull);
    // 18001af3b: mulsd xmm1, qword ptr [rip + 0x25859d] # 0x1802734e0
    X[1].d[0]=X[1].d[0] * KD(0x3fca47baa9b499d0ull);
    // 18001af43: mulsd xmm2, qword ptr [rip + 0x25858d] # 0x1802734d8
    X[2].d[0]=X[2].d[0] * KD(0xbfd495e9e1b089a0ull);
    // 18001af4b: movapd xmm7, xmm8
    X[7]=X[8];
    // 18001af50: movsd qword ptr [rsp + 0x48], xmm1
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[1].d[0]));
    // 18001af56: movsd qword ptr [rsp + 0x40], xmm2
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[2].d[0]));
    // 18001af5c: movapd xmm1, xmm8
    X[1]=X[8];
    // 18001af61: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001af64: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 18001af67: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001af6b: lea rcx, [rsp + 0x40]
    G[1]=(uint64_t)(0 + G[4] + 64ull);
    // 18001af70: movsd qword ptr [rsp + 0x48], xmm0
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[0].d[0]));
    // 18001af76: movsd qword ptr [rsp + 0x40], xmm2
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[2].d[0]));
    // 18001af7c: movapd xmm0, xmm8
    X[0]=X[8];
    // 18001af81: mulsd xmm0, qword ptr [rip + 0x258547] # 0x1802734d0
    X[0].d[0]=X[0].d[0] * KD(0x3fe2b5bb384fd2a6ull);
    // 18001af89: mulsd xmm1, qword ptr [rip + 0x258537] # 0x1802734c8
    X[1].d[0]=X[1].d[0] * KD(0xbfd1739f340d4dc6ull);
    // 18001af91: mulsd xmm7, qword ptr [rip + 0x258527] # 0x1802734c0
    X[7].d[0]=X[7].d[0] * KD(0xbfb060fe47991bc5ull);
    // 18001af99: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 18001af9d: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001afa0: lea rdx, [rsp + 0x80]
    G[2]=(uint64_t)(0 + G[4] + 128ull);
    // 18001afa8: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 18001afab: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001afaf: lea rcx, [rsp + 0x40]
    G[1]=(uint64_t)(0 + G[4] + 64ull);
    // 18001afb4: movsd qword ptr [rsp + 0x48], xmm0
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[0].d[0]));
    // 18001afba: movsd qword ptr [rsp + 0x40], xmm1
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[1].d[0]));
    // 18001afc0: movapd xmm0, xmm8
    X[0]=X[8];
    // 18001afc5: mulsd xmm0, qword ptr [rip + 0x2584eb] # 0x1802734b8
    X[0].d[0]=X[0].d[0] * KD(0xbfe2b5bb384fd2a6ull);
    // 18001afcd: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 18001afd1: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001afd4: lea rdx, [rsp + 0x90]
    G[2]=(uint64_t)(0 + G[4] + 144ull);
    // 18001afdc: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 18001afdf: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001afe3: lea rcx, [rsp + 0x40]
    G[1]=(uint64_t)(0 + G[4] + 64ull);
    // 18001afe8: movsd qword ptr [rsp + 0x48], xmm0
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[0].d[0]));
    // 18001afee: movsd qword ptr [rsp + 0x40], xmm1
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[1].d[0]));
    // 18001aff4: movapd xmm1, xmm8
    X[1]=X[8];
    // 18001aff9: mulsd xmm1, qword ptr [rip + 0x2584af] # 0x1802734b0
    X[1].d[0]=X[1].d[0] * KD(0xbfc7525460aa64c3ull);
    // 18001b001: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 18001b005: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001b008: lea rdx, [rsp + 0xa0]
    G[2]=(uint64_t)(0 + G[4] + 160ull);
    // 18001b010: movapd xmm0, xmm8
    X[0]=X[8];
    // 18001b015: mulsd xmm0, qword ptr [rip + 0x25848b] # 0x1802734a8
    X[0].d[0]=X[0].d[0] * KD(0x3fec0055fbb517a4ull);
    // 18001b01d: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 18001b020: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001b024: lea rcx, [rsp + 0x40]
    G[1]=(uint64_t)(0 + G[4] + 64ull);
    // 18001b029: movsd qword ptr [rsp + 0x48], xmm0
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[0].d[0]));
    // 18001b02f: movsd qword ptr [rsp + 0x40], xmm1
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[1].d[0]));
    // 18001b035: movapd xmm0, xmm8
    X[0]=X[8];
    // 18001b03a: mulsd xmm0, qword ptr [rip + 0x25845e] # 0x1802734a0
    X[0].d[0]=X[0].d[0] * KD(0xbfec0055fbb517a4ull);
    // 18001b042: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 18001b046: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001b049: lea rdx, [rsp + 0xb0]
    G[2]=(uint64_t)(0 + G[4] + 176ull);
    // 18001b051: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 18001b054: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001b058: lea rcx, [rsp + 0x40]
    G[1]=(uint64_t)(0 + G[4] + 64ull);
    // 18001b05d: movsd qword ptr [rsp + 0x48], xmm0
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[0].d[0]));
    // 18001b063: movsd qword ptr [rsp + 0x40], xmm1
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[1].d[0]));
    // 18001b069: movapd xmm0, xmm8
    X[0]=X[8];
    // 18001b06e: mulsd xmm0, qword ptr [rip + 0x258422] # 0x180273498
    X[0].d[0]=X[0].d[0] * KD(0x3ff083d03968d75aull);
    // 18001b076: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 18001b07a: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001b07d: lea rdx, [rsp + 0xc0]
    G[2]=(uint64_t)(0 + G[4] + 192ull);
    // 18001b085: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 18001b088: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001b08c: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 18001b090: movsd qword ptr [rsp + 0x40], xmm7
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[7].d[0]));
    // 18001b096: mulsd xmm8, qword ptr [rip + 0x2583f1] # 0x180273490
    X[8].d[0]=X[8].d[0] * KD(0xbff083d03968d75aull);
    // 18001b09f: movsd xmm1, qword ptr [rip + 0x2583e1] # 0x180273488
    X[1].d[0]=KD(0x3f747ae147ae147bull); X[1].d[1]=0.0;
    // 18001b0a7: movsd qword ptr [rsp + 0x48], xmm0
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[0].d[0]));
    // 18001b0ad: lea rcx, [rsp + 0x40]
    G[1]=(uint64_t)(0 + G[4] + 64ull);
    // 18001b0b2: lea rdx, [rsp + 0xd0]
    G[2]=(uint64_t)(0 + G[4] + 208ull);
    // 18001b0ba: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001b0bd: movsd xmm0, qword ptr [rip + 0x25700b] # 0x1802720d0
    X[0].d[0]=KD(0x4024000000000000ull); X[0].d[1]=0.0;
    // 18001b0c5: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 18001b0c8: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001b0cc: lea rcx, [rsp + 0x40]
    G[1]=(uint64_t)(0 + G[4] + 64ull);
    // 18001b0d1: movsd qword ptr [rsp + 0x40], xmm7
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[7].d[0]));
    // 18001b0d7: movsd qword ptr [rsp + 0x48], xmm8
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[8].d[0]));
    // 18001b0de: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 18001b0e2: mov rax, qword ptr [rcx]
    G[0]=RD64((uint64_t)(0 + G[1]));
    // 18001b0e5: lea rdx, [rsp + 0xe0]
    G[2]=(uint64_t)(0 + G[4] + 224ull);
    // 18001b0ed: mov qword ptr [rdx], rax
    WR64((uint64_t)(0 + G[2]),G[0]);
    // 18001b0f0: mov rax, qword ptr [rcx + 0x8]
    G[0]=RD64((uint64_t)(0 + G[1] + 8ull));
    // 18001b0f4: mov qword ptr [rdx + 0x8], rax
    WR64((uint64_t)(0 + G[2] + 8ull),G[0]);
    // 18001b0f8: call 0x1800bf082
    X[0].d[0]=std::pow(X[0].d[0],X[1].d[0]);
    // 18001b0fd: xorpd xmm2, xmm2
    X[2].u[0]^=X[2].u[0]; X[2].u[1]^=X[2].u[1];
    // 18001b101: movsd xmm3, qword ptr [rsp + 0x70]
    X[3].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 112ull))); X[3].d[1]=0.0;
    // 18001b107: movsd xmm6, qword ptr [rsp + 0x78]
    X[6].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 120ull))); X[6].d[1]=0.0;
    // 18001b10d: movsd xmm5, qword ptr [rsp + 0x88]
    X[5].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 136ull))); X[5].d[1]=0.0;
    // 18001b116: movapd xmm4, xmm3
    X[4]=X[3];
    // 18001b11a: movapd xmm1, xmm6
    X[1]=X[6];
    // 18001b11e: mulsd xmm6, xmm0
    X[6].d[0]=X[6].d[0] * X[0].d[0];
    // 18001b122: mulsd xmm3, xmm2
    X[3].d[0]=X[3].d[0] * X[2].d[0];
    // 18001b126: mulsd xmm4, xmm0
    X[4].d[0]=X[4].d[0] * X[0].d[0];
    // 18001b12a: mulsd xmm1, xmm2
    X[1].d[0]=X[1].d[0] * X[2].d[0];
    // 18001b12e: addsd xmm6, xmm3
    X[6].d[0]=X[6].d[0] + X[3].d[0];
    // 18001b132: subsd xmm4, xmm1
    X[4].d[0]=X[4].d[0] - X[1].d[0];
    // 18001b136: movsd xmm1, qword ptr [rsp + 0x80]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 128ull))); X[1].d[1]=0.0;
    // 18001b13f: movsd qword ptr [rsp + 0x48], xmm6
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[6].d[0]));
    // 18001b145: movsd qword ptr [rsp + 0x40], xmm4
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[4].d[0]));
    // 18001b14b: movapd xmm2, xmm1
    X[2]=X[1];
    // 18001b14f: movaps xmm0, xmmword ptr [rsp + 0x40]
    X[0].u[0]=RD64((uint64_t)(0 + G[4] + 64ull)); X[0].u[1]=RD64((uint64_t)(0 + G[4] + 64ull)+8);
    // 18001b154: movdqa xmmword ptr [rsp + 0x50], xmm0
    WR64((uint64_t)(0 + G[4] + 80ull),X[0].u[0]); WR64((uint64_t)(0 + G[4] + 80ull)+8,X[0].u[1]);
    // 18001b15a: movapd xmm0, xmm5
    X[0]=X[5];
    // 18001b15e: mulsd xmm2, xmm4
    X[2].d[0]=X[2].d[0] * X[4].d[0];
    // 18001b162: mulsd xmm1, qword ptr [rsp + 0x58]
    X[1].d[0]=X[1].d[0] * DBL(RD64((uint64_t)(0 + G[4] + 88ull)));
    // 18001b168: mulsd xmm5, qword ptr [rsp + 0x50]
    X[5].d[0]=X[5].d[0] * DBL(RD64((uint64_t)(0 + G[4] + 80ull)));
    // 18001b16e: movsd xmm4, qword ptr [rsp + 0x98]
    X[4].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 152ull))); X[4].d[1]=0.0;
    // 18001b177: addsd xmm5, xmm1
    X[5].d[0]=X[5].d[0] + X[1].d[0];
    // 18001b17b: movsd xmm1, qword ptr [rsp + 0x90]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 144ull))); X[1].d[1]=0.0;
    // 18001b184: mulsd xmm0, xmm6
    X[0].d[0]=X[0].d[0] * X[6].d[0];
    // 18001b188: movsd qword ptr [rsp + 0x48], xmm5
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[5].d[0]));
    // 18001b18e: movapd xmm3, xmm1
    X[3]=X[1];
    // 18001b192: subsd xmm2, xmm0
    X[2].d[0]=X[2].d[0] - X[0].d[0];
    // 18001b196: movsd qword ptr [rsp + 0x40], xmm2
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[2].d[0]));
    // 18001b19c: movaps xmm0, xmmword ptr [rsp + 0x40]
    X[0].u[0]=RD64((uint64_t)(0 + G[4] + 64ull)); X[0].u[1]=RD64((uint64_t)(0 + G[4] + 64ull)+8);
    // 18001b1a1: mulsd xmm3, xmm2
    X[3].d[0]=X[3].d[0] * X[2].d[0];
    // 18001b1a5: movdqa xmmword ptr [rsp + 0x50], xmm0
    WR64((uint64_t)(0 + G[4] + 80ull),X[0].u[0]); WR64((uint64_t)(0 + G[4] + 80ull)+8,X[0].u[1]);
    // 18001b1ab: mulsd xmm1, qword ptr [rsp + 0x58]
    X[1].d[0]=X[1].d[0] * DBL(RD64((uint64_t)(0 + G[4] + 88ull)));
    // 18001b1b1: movapd xmm0, xmm4
    X[0]=X[4];
    // 18001b1b5: mulsd xmm4, qword ptr [rsp + 0x50]
    X[4].d[0]=X[4].d[0] * DBL(RD64((uint64_t)(0 + G[4] + 80ull)));
    // 18001b1bb: mulsd xmm0, xmm5
    X[0].d[0]=X[0].d[0] * X[5].d[0];
    // 18001b1bf: movsd xmm5, qword ptr [rsp + 0xa8]
    X[5].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 168ull))); X[5].d[1]=0.0;
    // 18001b1c8: subsd xmm3, xmm0
    X[3].d[0]=X[3].d[0] - X[0].d[0];
    // 18001b1cc: addsd xmm4, xmm1
    X[4].d[0]=X[4].d[0] + X[1].d[0];
    // 18001b1d0: movsd xmm1, qword ptr [rsp + 0xa0]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 160ull))); X[1].d[1]=0.0;
    // 18001b1d9: movsd qword ptr [rsp + 0x40], xmm3
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[3].d[0]));
    // 18001b1df: movsd qword ptr [rsp + 0x48], xmm4
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[4].d[0]));
    // 18001b1e5: movapd xmm2, xmm1
    X[2]=X[1];
    // 18001b1e9: movaps xmm0, xmmword ptr [rsp + 0x40]
    X[0].u[0]=RD64((uint64_t)(0 + G[4] + 64ull)); X[0].u[1]=RD64((uint64_t)(0 + G[4] + 64ull)+8);
    // 18001b1ee: movdqa xmmword ptr [rsp + 0x50], xmm0
    WR64((uint64_t)(0 + G[4] + 80ull),X[0].u[0]); WR64((uint64_t)(0 + G[4] + 80ull)+8,X[0].u[1]);
    // 18001b1f4: movapd xmm0, xmm5
    X[0]=X[5];
    // 18001b1f8: mulsd xmm2, xmm3
    X[2].d[0]=X[2].d[0] * X[3].d[0];
    // 18001b1fc: mulsd xmm1, qword ptr [rsp + 0x58]
    X[1].d[0]=X[1].d[0] * DBL(RD64((uint64_t)(0 + G[4] + 88ull)));
    // 18001b202: mulsd xmm5, qword ptr [rsp + 0x50]
    X[5].d[0]=X[5].d[0] * DBL(RD64((uint64_t)(0 + G[4] + 80ull)));
    // 18001b208: addsd xmm5, xmm1
    X[5].d[0]=X[5].d[0] + X[1].d[0];
    // 18001b20c: movsd xmm1, qword ptr [rsp + 0xb0]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 176ull))); X[1].d[1]=0.0;
    // 18001b215: mulsd xmm0, xmm4
    X[0].d[0]=X[0].d[0] * X[4].d[0];
    // 18001b219: movsd xmm4, qword ptr [rsp + 0xb8]
    X[4].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 184ull))); X[4].d[1]=0.0;
    // 18001b222: movsd qword ptr [rsp + 0x48], xmm5
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[5].d[0]));
    // 18001b228: movapd xmm3, xmm1
    X[3]=X[1];
    // 18001b22c: subsd xmm2, xmm0
    X[2].d[0]=X[2].d[0] - X[0].d[0];
    // 18001b230: movsd qword ptr [rsp + 0x40], xmm2
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[2].d[0]));
    // 18001b236: movaps xmm0, xmmword ptr [rsp + 0x40]
    X[0].u[0]=RD64((uint64_t)(0 + G[4] + 64ull)); X[0].u[1]=RD64((uint64_t)(0 + G[4] + 64ull)+8);
    // 18001b23b: mulsd xmm3, xmm2
    X[3].d[0]=X[3].d[0] * X[2].d[0];
    // 18001b23f: movdqa xmmword ptr [rsp + 0x50], xmm0
    WR64((uint64_t)(0 + G[4] + 80ull),X[0].u[0]); WR64((uint64_t)(0 + G[4] + 80ull)+8,X[0].u[1]);
    // 18001b245: movapd xmm0, xmm4
    X[0]=X[4];
    // 18001b249: mulsd xmm4, qword ptr [rsp + 0x50]
    X[4].d[0]=X[4].d[0] * DBL(RD64((uint64_t)(0 + G[4] + 80ull)));
    // 18001b24f: mulsd xmm1, qword ptr [rsp + 0x58]
    X[1].d[0]=X[1].d[0] * DBL(RD64((uint64_t)(0 + G[4] + 88ull)));
    // 18001b255: mulsd xmm0, xmm5
    X[0].d[0]=X[0].d[0] * X[5].d[0];
    // 18001b259: movsd xmm5, qword ptr [rsp + 0xc8]
    X[5].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 200ull))); X[5].d[1]=0.0;
    // 18001b262: subsd xmm3, xmm0
    X[3].d[0]=X[3].d[0] - X[0].d[0];
    // 18001b266: addsd xmm4, xmm1
    X[4].d[0]=X[4].d[0] + X[1].d[0];
    // 18001b26a: movsd xmm1, qword ptr [rsp + 0xc0]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 192ull))); X[1].d[1]=0.0;
    // 18001b273: movsd qword ptr [rsp + 0x40], xmm3
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[3].d[0]));
    // 18001b279: movsd qword ptr [rsp + 0x48], xmm4
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[4].d[0]));
    // 18001b27f: movapd xmm2, xmm1
    X[2]=X[1];
    // 18001b283: movaps xmm0, xmmword ptr [rsp + 0x40]
    X[0].u[0]=RD64((uint64_t)(0 + G[4] + 64ull)); X[0].u[1]=RD64((uint64_t)(0 + G[4] + 64ull)+8);
    // 18001b288: movdqa xmmword ptr [rsp + 0x50], xmm0
    WR64((uint64_t)(0 + G[4] + 80ull),X[0].u[0]); WR64((uint64_t)(0 + G[4] + 80ull)+8,X[0].u[1]);
    // 18001b28e: movapd xmm0, xmm5
    X[0]=X[5];
    // 18001b292: mulsd xmm2, xmm3
    X[2].d[0]=X[2].d[0] * X[3].d[0];
    // 18001b296: mulsd xmm1, qword ptr [rsp + 0x58]
    X[1].d[0]=X[1].d[0] * DBL(RD64((uint64_t)(0 + G[4] + 88ull)));
    // 18001b29c: mulsd xmm5, qword ptr [rsp + 0x50]
    X[5].d[0]=X[5].d[0] * DBL(RD64((uint64_t)(0 + G[4] + 80ull)));
    // 18001b2a2: movsd xmm3, qword ptr [rsp + 0xd8]
    X[3].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 216ull))); X[3].d[1]=0.0;
    // 18001b2ab: addsd xmm5, xmm1
    X[5].d[0]=X[5].d[0] + X[1].d[0];
    // 18001b2af: movsd xmm1, qword ptr [rsp + 0xd0]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 208ull))); X[1].d[1]=0.0;
    // 18001b2b8: mulsd xmm0, xmm4
    X[0].d[0]=X[0].d[0] * X[4].d[0];
    // 18001b2bc: movapd xmm4, xmm1
    X[4]=X[1];
    // 18001b2c0: movsd qword ptr [rsp + 0x48], xmm5
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[5].d[0]));
    // 18001b2c6: subsd xmm2, xmm0
    X[2].d[0]=X[2].d[0] - X[0].d[0];
    // 18001b2ca: movsd qword ptr [rsp + 0x40], xmm2
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[2].d[0]));
    // 18001b2d0: movaps xmm0, xmmword ptr [rsp + 0x40]
    X[0].u[0]=RD64((uint64_t)(0 + G[4] + 64ull)); X[0].u[1]=RD64((uint64_t)(0 + G[4] + 64ull)+8);
    // 18001b2d5: mulsd xmm4, xmm2
    X[4].d[0]=X[4].d[0] * X[2].d[0];
    // 18001b2d9: movdqa xmmword ptr [rsp + 0x50], xmm0
    WR64((uint64_t)(0 + G[4] + 80ull),X[0].u[0]); WR64((uint64_t)(0 + G[4] + 80ull)+8,X[0].u[1]);
    // 18001b2df: mulsd xmm1, qword ptr [rsp + 0x58]
    X[1].d[0]=X[1].d[0] * DBL(RD64((uint64_t)(0 + G[4] + 88ull)));
    // 18001b2e5: movapd xmm0, xmm3
    X[0]=X[3];
    // 18001b2e9: mulsd xmm3, qword ptr [rsp + 0x50]
    X[3].d[0]=X[3].d[0] * DBL(RD64((uint64_t)(0 + G[4] + 80ull)));
    // 18001b2ef: mulsd xmm0, xmm5
    X[0].d[0]=X[0].d[0] * X[5].d[0];
    // 18001b2f3: subsd xmm4, xmm0
    X[4].d[0]=X[4].d[0] - X[0].d[0];
    // 18001b2f7: addsd xmm3, xmm1
    X[3].d[0]=X[3].d[0] + X[1].d[0];
    // 18001b2fb: movsd qword ptr [rsp + 0x40], xmm4
    WR64((uint64_t)(0 + G[4] + 64ull),BITS(X[4].d[0]));
    // 18001b301: mulsd xmm4, xmm7
    X[4].d[0]=X[4].d[0] * X[7].d[0];
    // 18001b305: movsd qword ptr [rsp + 0x48], xmm3
    WR64((uint64_t)(0 + G[4] + 72ull),BITS(X[3].d[0]));
    // 18001b30b: movaps xmm0, xmmword ptr [rsp + 0x40]
    X[0].u[0]=RD64((uint64_t)(0 + G[4] + 64ull)); X[0].u[1]=RD64((uint64_t)(0 + G[4] + 64ull)+8);
    // 18001b310: mulsd xmm3, xmm8
    X[3].d[0]=X[3].d[0] * X[8].d[0];
    // 18001b315: movdqa xmmword ptr [rsp + 0x50], xmm0
    WR64((uint64_t)(0 + G[4] + 80ull),X[0].u[0]); WR64((uint64_t)(0 + G[4] + 80ull)+8,X[0].u[1]);
    // 18001b31b: movsd xmm1, qword ptr [rsp + 0x58]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 88ull))); X[1].d[1]=0.0;
    // 18001b321: movsd xmm0, qword ptr [rsp + 0x50]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 80ull))); X[0].d[1]=0.0;
    // 18001b327: subsd xmm4, xmm3
    X[4].d[0]=X[4].d[0] - X[3].d[0];
    // 18001b32b: mulsd xmm0, xmm8
    X[0].d[0]=X[0].d[0] * X[8].d[0];
    // 18001b330: mulsd xmm1, xmm7
    X[1].d[0]=X[1].d[0] * X[7].d[0];
    // 18001b334: addsd xmm0, xmm1
    X[0].d[0]=X[0].d[0] + X[1].d[0];
    // 18001b338: mulsd xmm4, xmm4
    X[4].d[0]=X[4].d[0] * X[4].d[0];
    // 18001b33c: mulsd xmm0, xmm0
    X[0].d[0]=X[0].d[0] * X[0].d[0];
    // 18001b340: addsd xmm0, xmm4
    X[0].d[0]=X[0].d[0] + X[4].d[0];
    // 18001b344: call 0x1800bf0a6
    X[0].d[0]=std::sqrt(X[0].d[0]);
    // 18001b349: movsd xmm1, qword ptr [rip + 0x25702f] # 0x180272380
    X[1].d[0]=KD(0x3ff0000000000000ull); X[1].d[1]=0.0;
    // 18001b351: lea rax, [rsp + 0x100]
    G[0]=(uint64_t)(0 + G[4] + 256ull);
    // 18001b359: mov qword ptr [rsp + 0x38], rax
    WR64((uint64_t)(0 + G[4] + 56ull),G[0]);
    // 18001b35e: xor r9d, r9d
    G[9]=(uint64_t)(uint32_t)(0); F.set(0,32,0ull,0,0);
    // 18001b361: lea rax, [rsp + 0x1a0]
    G[0]=(uint64_t)(0 + G[4] + 416ull);
    // 18001b369: mov qword ptr [rsp + 0x30], rax
    WR64((uint64_t)(0 + G[4] + 48ull),G[0]);
    // 18001b36e: lea r8, [rsp + 0x240]
    G[8]=(uint64_t)(0 + G[4] + 576ull);
    // 18001b376: lea rcx, [rsp + 0x60]
    G[1]=(uint64_t)(0 + G[4] + 96ull);
    // 18001b37b: movsd qword ptr [rsp + 0x28], xmm1
    WR64((uint64_t)(0 + G[4] + 40ull),BITS(X[1].d[0]));
    // 18001b381: movsd qword ptr [rsp + 0x20], xmm0
    WR64((uint64_t)(0 + G[4] + 32ull),BITS(X[0].d[0]));
    // 18001b387: lea edx, [r9 + 0x8]
    G[2]=(uint64_t)(uint32_t)((uint64_t)(0 + G[9] + 8ull));
    // 18001b38b: call 0x180019570
    G[4]-=8; WR64(G[4],0); aaDesign19570(R); G[4]+=8;
    // 18001b390: movsd xmm0, qword ptr [rsp + 0x1b0]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 432ull))); X[0].d[1]=0.0;
    // 18001b399: movsd xmm1, qword ptr [rsp + 0x110]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 272ull))); X[1].d[1]=0.0;
    // 18001b3a2: movsd qword ptr [rsi + 0x2e0], xmm0
    WR64((uint64_t)(0 + G[6] + 736ull),BITS(X[0].d[0]));
    // 18001b3aa: movsd xmm0, qword ptr [rsp + 0x1c0]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 448ull))); X[0].d[1]=0.0;
    // 18001b3b3: movsd qword ptr [rsi + 0x320], xmm1
    WR64((uint64_t)(0 + G[6] + 800ull),BITS(X[1].d[0]));
    // 18001b3bb: movsd xmm1, qword ptr [rsp + 0x120]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 288ull))); X[1].d[1]=0.0;
    // 18001b3c4: movsd qword ptr [rsi + 0x2e8], xmm0
    WR64((uint64_t)(0 + G[6] + 744ull),BITS(X[0].d[0]));
    // 18001b3cc: movsd xmm0, qword ptr [rsp + 0x1d0]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 464ull))); X[0].d[1]=0.0;
    // 18001b3d5: movsd qword ptr [rsi + 0x328], xmm1
    WR64((uint64_t)(0 + G[6] + 808ull),BITS(X[1].d[0]));
    // 18001b3dd: movsd xmm1, qword ptr [rsp + 0x130]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 304ull))); X[1].d[1]=0.0;
    // 18001b3e6: movsd qword ptr [rsi + 0x2f0], xmm0
    WR64((uint64_t)(0 + G[6] + 752ull),BITS(X[0].d[0]));
    // 18001b3ee: movsd xmm0, qword ptr [rsp + 0x1e0]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 480ull))); X[0].d[1]=0.0;
    // 18001b3f7: movsd qword ptr [rsi + 0x330], xmm1
    WR64((uint64_t)(0 + G[6] + 816ull),BITS(X[1].d[0]));
    // 18001b3ff: movsd xmm1, qword ptr [rsp + 0x140]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 320ull))); X[1].d[1]=0.0;
    // 18001b408: movsd qword ptr [rsi + 0x2f8], xmm0
    WR64((uint64_t)(0 + G[6] + 760ull),BITS(X[0].d[0]));
    // 18001b410: movsd xmm0, qword ptr [rsp + 0x1f0]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 496ull))); X[0].d[1]=0.0;
    // 18001b419: movsd qword ptr [rsi + 0x338], xmm1
    WR64((uint64_t)(0 + G[6] + 824ull),BITS(X[1].d[0]));
    // 18001b421: movsd xmm1, qword ptr [rsp + 0x150]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 336ull))); X[1].d[1]=0.0;
    // 18001b42a: movsd qword ptr [rsi + 0x300], xmm0
    WR64((uint64_t)(0 + G[6] + 768ull),BITS(X[0].d[0]));
    // 18001b432: movsd qword ptr [rsi + 0x340], xmm1
    WR64((uint64_t)(0 + G[6] + 832ull),BITS(X[1].d[0]));
    // 18001b43a: movsd xmm0, qword ptr [rsp + 0x200]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 512ull))); X[0].d[1]=0.0;
    // 18001b443: movsd xmm1, qword ptr [rsp + 0x160]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 352ull))); X[1].d[1]=0.0;
    // 18001b44c: movaps xmm8, xmmword ptr [rsp + 0x2e0]
    X[8].u[0]=RD64((uint64_t)(0 + G[4] + 736ull)); X[8].u[1]=RD64((uint64_t)(0 + G[4] + 736ull)+8);
    // 18001b455: movaps xmm7, xmmword ptr [rsp + 0x2f0]
    X[7].u[0]=RD64((uint64_t)(0 + G[4] + 752ull)); X[7].u[1]=RD64((uint64_t)(0 + G[4] + 752ull)+8);
    // 18001b45d: movaps xmm6, xmmword ptr [rsp + 0x300]
    X[6].u[0]=RD64((uint64_t)(0 + G[4] + 768ull)); X[6].u[1]=RD64((uint64_t)(0 + G[4] + 768ull)+8);
    // 18001b465: xor eax, eax
    G[0]=(uint64_t)(uint32_t)(0); F.set(0,32,0ull,0,0);
    // 18001b467: movsd qword ptr [rsi + 0x308], xmm0
    WR64((uint64_t)(0 + G[6] + 776ull),BITS(X[0].d[0]));
    // 18001b46f: movsd xmm0, qword ptr [rsp + 0x210]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 528ull))); X[0].d[1]=0.0;
    // 18001b478: movsd qword ptr [rsi + 0x348], xmm1
    WR64((uint64_t)(0 + G[6] + 840ull),BITS(X[1].d[0]));
    // 18001b480: lea edi, [rax + 0x1]
    G[7]=(uint64_t)(uint32_t)((uint64_t)(0 + G[0] + 1ull));
    // 18001b483: movsd xmm1, qword ptr [rsp + 0x170]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 368ull))); X[1].d[1]=0.0;
    // 18001b48c: movsd qword ptr [rsi + 0x310], xmm0
    WR64((uint64_t)(0 + G[6] + 784ull),BITS(X[0].d[0]));
    // 18001b494: movsd xmm0, qword ptr [rsp + 0x220]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 544ull))); X[0].d[1]=0.0;
    // 18001b49d: movsd qword ptr [rsi + 0x350], xmm1
    WR64((uint64_t)(0 + G[6] + 848ull),BITS(X[1].d[0]));
    // 18001b4a5: movsd xmm1, qword ptr [rsp + 0x180]
    X[1].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 384ull))); X[1].d[1]=0.0;
    // 18001b4ae: movsd qword ptr [rsi + 0x318], xmm0
    WR64((uint64_t)(0 + G[6] + 792ull),BITS(X[0].d[0]));
    // 18001b4b6: movsd xmm0, qword ptr [rsp + 0x100]
    X[0].d[0]=DBL(RD64((uint64_t)(0 + G[4] + 256ull))); X[0].d[1]=0.0;
    // 18001b4bf: movsd qword ptr [rsi + 0x358], xmm1
    WR64((uint64_t)(0 + G[6] + 856ull),BITS(X[1].d[0]));
    // 18001b4c7: movsd qword ptr [rsi + 0x3a0], xmm0
    WR64((uint64_t)(0 + G[6] + 928ull),BITS(X[0].d[0]));
    // 18001b4cf: mov qword ptr [rsi + 0x360], rax
    WR64((uint64_t)(0 + G[6] + 864ull),G[0]);
    // 18001b4d6: mov qword ptr [rsi + 0x368], rax
    WR64((uint64_t)(0 + G[6] + 872ull),G[0]);
    // 18001b4dd: mov qword ptr [rsi + 0x370], rax
    WR64((uint64_t)(0 + G[6] + 880ull),G[0]);
    // 18001b4e4: mov qword ptr [rsi + 0x378], rax
    WR64((uint64_t)(0 + G[6] + 888ull),G[0]);
    // 18001b4eb: mov qword ptr [rsi + 0x380], rax
    WR64((uint64_t)(0 + G[6] + 896ull),G[0]);
    // 18001b4f2: mov qword ptr [rsi + 0x388], rax
    WR64((uint64_t)(0 + G[6] + 904ull),G[0]);
    // 18001b4f9: mov qword ptr [rsi + 0x390], rax
    WR64((uint64_t)(0 + G[6] + 912ull),G[0]);
    // 18001b500: mov qword ptr [rsi + 0x398], rax
    WR64((uint64_t)(0 + G[6] + 920ull),G[0]);
    // 18001b507: mov rax, qword ptr [rsi + 0x90]
    G[0]=RD64((uint64_t)(0 + G[6] + 144ull));
    // 18001b50e: cmp di, word ptr [rax + 0x2]
    { uint64_t a=(G[7]&0xFFFFull), b=RD16((uint64_t)(0 + G[0] + 2ull)), r=((((G[7]&0xFFFFull))-(RD16((uint64_t)(0 + G[0] + 2ull))))&0xFFFFull); F.set(2,16,r,a,b);
    }
    // 18001b512: jae 0x18001b565
    if (!F.cf) goto L_18001b565;
    // 18001b514: mov qword ptr [rsp + 0x320], rbx
    WR64((uint64_t)(0 + G[4] + 800ull),G[3]);
    // 18001b51c: lea rbx, [rsi + 0x3a8]
    G[3]=(uint64_t)(0 + G[6] + 936ull);
    // 18001b523: nop word ptr [rax + rax]
L_18001b530:;
    // 18001b530: lea rdx, [rsi + 0x2e0]
    G[2]=(uint64_t)(0 + G[6] + 736ull);
    // 18001b537: mov rcx, rbx
    G[1]=G[3];
    // 18001b53a: mov r8d, 0xc8
    G[8]=(uint64_t)(uint32_t)(200ull);
    // 18001b540: call 0x1800bec18
    std::memcpy((void*)G[1],(const void*)G[2],(size_t)G[8]);
    // 18001b545: mov rax, qword ptr [rsi + 0x90]
    G[0]=RD64((uint64_t)(0 + G[6] + 144ull));
    // 18001b54c: inc edi
    { uint64_t a=(G[7]&0xFFFFFFFFull), b=1ull, r=((((G[7]&0xFFFFFFFFull))+(1ull))&0xFFFFFFFFull); F.set(3,32,r,a,b);
      G[7]=(uint64_t)(uint32_t)(r);
    }
    // 18001b54e: movzx ecx, word ptr [rax + 0x2]
    G[1]=(uint64_t)(uint32_t)(RD16((uint64_t)(0 + G[0] + 2ull)));
    // 18001b552: add rbx, 0xc8
    { uint64_t a=G[3], b=200ull, r=(((G[3])+(200ull))&0xFFFFFFFFFFFFFFFFull); F.set(1,64,r,a,b);
      G[3]=r;
    }
    // 18001b559: cmp edi, ecx
    { uint64_t a=(G[7]&0xFFFFFFFFull), b=(G[1]&0xFFFFFFFFull), r=((((G[7]&0xFFFFFFFFull))-((G[1]&0xFFFFFFFFull)))&0xFFFFFFFFull); F.set(2,32,r,a,b);
    }
    // 18001b55b: jl 0x18001b530
    if ((F.sf!=F.of)) goto L_18001b530;
    // 18001b55d: mov rbx, qword ptr [rsp + 0x320]
    G[3]=RD64((uint64_t)(0 + G[4] + 800ull));
L_18001b565:;
    // 18001b565: lea r11, [rsp + 0x310]
    G[11]=(uint64_t)(0 + G[4] + 784ull);
    // 18001b56d: mov rbp, qword ptr [r11 + 0x18]
    G[5]=RD64((uint64_t)(0 + G[11] + 24ull));
    // 18001b571: mov rsi, qword ptr [r11 + 0x20]
    G[6]=RD64((uint64_t)(0 + G[11] + 32ull));
    // 18001b575: mov rsp, r11
    G[4]=G[11];
    // 18001b578: pop rdi
    G[7]=RD64(G[4]);
    G[4]+=8;
    // 18001b579: ret 
    return;
}

// The filter of one stream: the plug-in's object (only the part the filter uses: the sections of the channels from
// offset 0x2e0 on, 0xc8 bytes each) and the design.
class Filter {
public:
    static constexpr int kMaxCh = 8;
    void design(double x, int channels)
    {
        e_.assign(0x2e0 + 0xc8 * (size_t)(kMaxCh + 1) + 0x100, 0);
        wfx_.assign(16, 0);
        const uint16_t ch = (uint16_t)channels;
        std::memcpy(&wfx_[2], &ch, 2);
        const uint64_t w = (uint64_t)(uintptr_t)wfx_.data();
        std::memcpy(&e_[0x90], &w, 8);
        alignas(16) static thread_local uint8_t stack[0x1000];
        Regs R;
        std::memset(&R, 0, sizeof R);
        R.g[1] = (uint64_t)(uintptr_t)e_.data();
        R.g[4] = (uint64_t)(uintptr_t)(stack + 0xe00) - 8;
        R.x[1].d[0] = x;
        aaDesign1aed0(R);
    }
    bool ready() const { return !e_.empty(); }
    // RVA 0x1b670: one block, interleaved float frames, in place
    void apply(float* data, int frames, int channels)
    {
        if (e_.empty()) return;
        for (int c = 0; c < channels; ++c) {
            uint8_t* b = &e_[0x2e0 + 0xc8 * (size_t)c];
            double* z = (double*)(b + 0x80);
            for (int k = 0; k < 8; ++k) {
                uint64_t u;
                std::memcpy(&u, &z[k], 8);
                if (((u >> 32) & 0x7ff00000u) < 0x3df00000u) z[k] = 0.0;     // too small to matter: cleared
            }
            for (int i = 0; i < frames; ++i) {
                float* p = data + (size_t)i * channels + c;
                double y = (double)*p;
                *p = (float)section(b, y);
            }
        }
    }

private:
    // RVA 0x1bc90: eight sections in a row, the states passing from one to the next (a lattice of delay elements)
    static double section(uint8_t* blk, double x)
    {
        double a[8], b[8], z[8], g;
        std::memcpy(a, blk, 64);
        std::memcpy(b, blk + 0x40, 64);
        std::memcpy(z, blk + 0x80, 64);
        std::memcpy(&g, blk + 0xc0, 8);
        double t = x;
        t = z[0] * a[0] + t;
        t = a[1] * z[1] + t;
        t = a[2] * z[2] + t;
        t = a[3] * z[3] + t;
        t = a[4] * z[4] + t;
        t = a[5] * z[5] + t;
        t = a[6] * z[6] + t;
        t = a[7] * z[7] + t;
        double s = 0.0;
        s = b[0] * z[0] + s;
        s = s + b[1] * z[1];
        s = s + b[2] * z[2];
        s = s + b[3] * z[3];
        s = s + b[4] * z[4];
        s = s + b[5] * z[5];
        s = s + b[6] * z[6];
        s = s + b[7] * z[7];
        for (int k = 7; k > 0; --k) z[k] = z[k - 1];
        z[0] = t;
        std::memcpy(blk + 0x80, z, 64);
        return g * t + s;
    }
    std::vector<uint8_t> e_, wfx_;
};

}  // namespace aa
}  // namespace vegpack1
}  // namespace reno
