;******************************************************************************
;* linear least squares model
;*
;* Copyright (c) 2013 Loren Merritt
;*
;* This file is part of FFmpeg.
;*
;* FFmpeg is free software; you can redistribute it and/or
;* modify it under the terms of the GNU Lesser General Public
;* License as published by the Free Software Foundation; either
;* version 2.1 of the License, or (at your option) any later version.
;*
;* FFmpeg is distributed in the hope that it will be useful,
;* but WITHOUT ANY WARRANTY; without even the implied warranty of
;* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
;* Lesser General Public License for more details.
;*
;* You should have received a copy of the GNU Lesser General Public
;* License along with FFmpeg; if not, write to the Free Software
;* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
;******************************************************************************

%include "x86util.asm"

SECTION .text

%define MAX_VARS 32
%define MAX_VARS_ALIGN (MAX_VARS+4)
%define COVAR_STRIDE MAX_VARS_ALIGN*8
%define COVAR(x,y) [covarq + (x)*8 + (y)*COVAR_STRIDE]

struc LLSModel
    .covariance:  resq MAX_VARS_ALIGN*MAX_VARS_ALIGN
    .coeff:       resq MAX_VARS*MAX_VARS
    .variance:    resq MAX_VARS
    .indep_count: resd 1
endstruc

%macro ADDPD_MEM 2
%if cpuflag(avx)
    vaddpd %2, %1
%else
    addpd  %2, %1
%endif
    mova   %1, %2
%endmacro

INIT_XMM sse2
%define movdqa movaps
cglobal update_lls, 2,5,8, ctx, var, i, j, covar2
    %define covarq ctxq
    mov     id, [ctxq + LLSModel.indep_count]
    lea   varq, [varq + iq*8]
    neg     iq
    mov covar2q, covarq
.loopi:
    ; Compute all 3 pairwise products of a 2x2 block that lies on the diagonal
    mova    m1, [varq + iq*8]
    mova    m3, [varq + iq*8 + 16]
    pshufd  m4, m1, q1010
    pshufd  m5, m1, q3232
    pshufd  m6, m3, q1010
    pshufd  m7, m3, q3232
    mulpd   m0, m1, m4
    mulpd   m1, m1, m5
    lea covarq, [covar2q + 16]
    ADDPD_MEM COVAR(-2,0), m0
    ADDPD_MEM COVAR(-2,1), m1
    lea     jq, [iq + 2]
    cmp     jd, -2
    jg .skip4x4
.loop4x4:
    ; Compute all 16 pairwise products of a 4x4 block
    mulpd   m0, m4, m3
    mulpd   m1, m5, m3
    mulpd   m2, m6, m3
    mulpd   m3, m3, m7
    ADDPD_MEM COVAR(0,0), m0
    ADDPD_MEM COVAR(0,1), m1
    ADDPD_MEM COVAR(0,2), m2
    ADDPD_MEM COVAR(0,3), m3
    mova    m3, [varq + jq*8 + 16]
    mulpd   m0, m4, m3
    mulpd   m1, m5, m3
    mulpd   m2, m6, m3
    mulpd   m3, m3, m7
    ADDPD_MEM COVAR(2,0), m0
    ADDPD_MEM COVAR(2,1), m1
    ADDPD_MEM COVAR(2,2), m2
    ADDPD_MEM COVAR(2,3), m3
    mova    m3, [varq + jq*8 + 32]
    add covarq, 32
    add     jq, 4
    cmp     jd, -2
    jle .loop4x4
.skip4x4:
    test    jd, jd
    jg .skip2x4
    mulpd   m4, m3
    mulpd   m5, m3
    mulpd   m6, m3
    mulpd   m7, m3
    ADDPD_MEM COVAR(0,0), m4
    ADDPD_MEM COVAR(0,1), m5
    ADDPD_MEM COVAR(0,2), m6
    ADDPD_MEM COVAR(0,3), m7
.skip2x4:
    add     iq, 4
    add covar2q, 4*COVAR_STRIDE+32
    cmp     id, -2
    jle .loopi
    test    id, id
    jg .ret
    mov     jq, iq
    %define covarq covar2q
.loop2x1:
    movsd   m0, [varq + iq*8]
    movlhps m0, m0
    mulpd   m0, [varq + jq*8]
    ADDPD_MEM COVAR(0,0), m0
    inc     iq
    add covarq, COVAR_STRIDE
    test    id, id
    jle .loop2x1
.ret:
    REP_RET

%if HAVE_AVX_EXTERNAL && 0
INIT_YMM avx
cglobal update_lls, 3,6,8, ctx, var, count, i, j, count2
    %define covarq ctxq
    mov  countd, [ctxq + LLSModel.indep_count]
    lea count2d, [countq-2]
    xor     id, id
.loopi:
    ; Compute all 10 pairwise products of a 4x4 block that lies on the diagonal
    mova    ymm1, [varq + iq*8]
    vbroadcastsd ymm4, [varq + iq*8]
    vbroadcastsd ymm5, [varq + iq*8 + 8]
    vbroadcastsd ymm6, [varq + iq*8 + 16]
    vbroadcastsd ymm7, [varq + iq*8 + 24]
    vextractf128 xmm3, ymm1, 1
    vmulpd  ymm0, ymm1, ymm4
    vmulpd  ymm1, ymm1, ymm5
    vmulpd  xmm2, xmm3, xmm6
    vmulpd  xmm3, xmm3, xmm7
    ADDPD_MEM COVAR(iq  ,0), ymm0
    ADDPD_MEM COVAR(iq  ,1), ymm1
    ADDPD_MEM COVAR(iq+2,2), xmm2
    ADDPD_MEM COVAR(iq+2,3), xmm3
    lea     jd, [iq + 4]
    cmp     jd, count2d
    jg .skip4x4
.loop4x4:
    ; Compute all 16 pairwise products of a 4x4 block
    mova    ymm3, [varq + jq*8]
    vmulpd  ymm0, ymm3, ymm4
    vmulpd  ymm1, ymm3, ymm5
    vmulpd  ymm2, ymm3, ymm6
    vmulpd  ymm3, ymm3, ymm7
    ADDPD_MEM COVAR(jq,0), ymm0
    ADDPD_MEM COVAR(jq,1), ymm1
    ADDPD_MEM COVAR(jq,2), ymm2
    ADDPD_MEM COVAR(jq,3), ymm3
    add     jd, 4
    cmp     jd, count2d
    jle .loop4x4
.skip4x4:
    cmp     jd, countd
    jg .skip2x4
    mova    xmm3, [varq + jq*8]
    vmulpd  xmm0, xmm3, xmm4
    vmulpd  xmm1, xmm3, xmm5
    vmulpd  xmm2, xmm3, xmm6
    vmulpd  xmm3, xmm3, xmm7
    ADDPD_MEM COVAR(jq,0), xmm0
    ADDPD_MEM COVAR(jq,1), xmm1
    ADDPD_MEM COVAR(jq,2), xmm2
    ADDPD_MEM COVAR(jq,3), xmm3
.skip2x4:
    add     id, 4
    add covarq, 4*COVAR_STRIDE
    cmp     id, count2d
    jle .loopi
    cmp     id, countd
    jg .ret
    mov     jd, id
.loop2x1:
    vmovddup xmm0, [varq + iq*8]
    vmulpd   xmm0, [varq + jq*8]
    ADDPD_MEM COVAR(jq,0), xmm0
    inc     id
    add covarq, COVAR_STRIDE
    cmp     id, countd
    jle .loop2x1
.ret:
    REP_RET
%endif