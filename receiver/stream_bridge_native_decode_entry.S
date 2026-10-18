// AAPCS64 tail dispatcher; no variadic argument is decoded by this entry.
// Original x0-x15, x17-x30, all SIMD registers, NZCV/FPCR/FPSR and the original
// stack pointer are restored before branching. x16 is the ABI-designated IP0
// branch scratch. The C classifier preserves entry errno. No public ioctl
// symbol is exported: a separately reviewed loader must publish this entry
// only after preparePinnedProcess has established immutable original targets.
.text
.balign 4
.global sfnp_stream_decode_ioctl_entry
.type sfnp_stream_decode_ioctl_entry,%function
sfnp_stream_decode_ioctl_entry:
    hint #34
    sub sp,sp,#800
    stp x0,x1,[sp,#0]
    stp x2,x3,[sp,#16]
    stp x4,x5,[sp,#32]
    stp x6,x7,[sp,#48]
    stp x8,x9,[sp,#64]
    stp x10,x11,[sp,#80]
    stp x12,x13,[sp,#96]
    stp x14,x15,[sp,#112]
    stp x17,x18,[sp,#128]
    stp x19,x20,[sp,#144]
    stp x21,x22,[sp,#160]
    stp x23,x24,[sp,#176]
    stp x25,x26,[sp,#192]
    stp x27,x28,[sp,#208]
    stp x29,x30,[sp,#224]
    mrs x9,nzcv
    str x9,[sp,#240]
    mrs x9,fpcr
    str x9,[sp,#248]
    mrs x9,fpsr
    str x9,[sp,#256]
    stp q0,q1,[sp,#272]
    stp q2,q3,[sp,#304]
    stp q4,q5,[sp,#336]
    stp q6,q7,[sp,#368]
    stp q8,q9,[sp,#400]
    stp q10,q11,[sp,#432]
    stp q12,q13,[sp,#464]
    stp q14,q15,[sp,#496]
    stp q16,q17,[sp,#528]
    stp q18,q19,[sp,#560]
    stp q20,q21,[sp,#592]
    stp q22,q23,[sp,#624]
    stp q24,q25,[sp,#656]
    stp q26,q27,[sp,#688]
    stp q28,q29,[sp,#720]
    stp q30,q31,[sp,#752]
    mov x3,x30
    bl sfnp_native_decode_classify
    mov x16,x0
    ldp q0,q1,[sp,#272]
    ldp q2,q3,[sp,#304]
    ldp q4,q5,[sp,#336]
    ldp q6,q7,[sp,#368]
    ldp q8,q9,[sp,#400]
    ldp q10,q11,[sp,#432]
    ldp q12,q13,[sp,#464]
    ldp q14,q15,[sp,#496]
    ldp q16,q17,[sp,#528]
    ldp q18,q19,[sp,#560]
    ldp q20,q21,[sp,#592]
    ldp q22,q23,[sp,#624]
    ldp q24,q25,[sp,#656]
    ldp q26,q27,[sp,#688]
    ldp q28,q29,[sp,#720]
    ldp q30,q31,[sp,#752]
    ldr x9,[sp,#240]
    msr nzcv,x9
    ldr x9,[sp,#248]
    msr fpcr,x9
    ldr x9,[sp,#256]
    msr fpsr,x9
    ldp x0,x1,[sp,#0]
    ldp x2,x3,[sp,#16]
    ldp x4,x5,[sp,#32]
    ldp x6,x7,[sp,#48]
    ldp x8,x9,[sp,#64]
    ldp x10,x11,[sp,#80]
    ldp x12,x13,[sp,#96]
    ldp x14,x15,[sp,#112]
    ldp x17,x18,[sp,#128]
    ldp x19,x20,[sp,#144]
    ldp x21,x22,[sp,#160]
    ldp x23,x24,[sp,#176]
    ldp x25,x26,[sp,#192]
    ldp x27,x28,[sp,#208]
    ldp x29,x30,[sp,#224]
    add sp,sp,#800
    br x16
.size sfnp_stream_decode_ioctl_entry,.-sfnp_stream_decode_ioctl_entry
.section .note.GNU-stack,"",%progbits

