#if defined(__x86_64__)
.text
.set Lframe_size,352

// Keep the state expected by REAPER's private callers across the C++ lookup.
.macro FRZZ_SAVE
    pushfq
    subq $Lframe_size,%rsp
    movq %rax,0(%rsp)
    movq %rdi,8(%rsp)
    movq %rsi,16(%rsp)
    movq %rdx,24(%rsp)
    movq %rcx,32(%rsp)
    movq %r8,40(%rsp)
    movq %r9,48(%rsp)
    movq %r10,56(%rsp)
    movq %r11,64(%rsp)
    movaps %xmm0,96(%rsp)
    movaps %xmm1,112(%rsp)
    movaps %xmm2,128(%rsp)
    movaps %xmm3,144(%rsp)
    movaps %xmm4,160(%rsp)
    movaps %xmm5,176(%rsp)
    movaps %xmm6,192(%rsp)
    movaps %xmm7,208(%rsp)
    movaps %xmm8,224(%rsp)
    movaps %xmm9,240(%rsp)
    movaps %xmm10,256(%rsp)
    movaps %xmm11,272(%rsp)
    movaps %xmm12,288(%rsp)
    movaps %xmm13,304(%rsp)
    movaps %xmm14,320(%rsp)
    movaps %xmm15,336(%rsp)
.endmacro

.macro FRZZ_RESTORE
    movq 0(%rsp),%rax
    movq 8(%rsp),%rdi
    movq 16(%rsp),%rsi
    movq 24(%rsp),%rdx
    movq 32(%rsp),%rcx
    movq 40(%rsp),%r8
    movq 48(%rsp),%r9
    movq 56(%rsp),%r10
    movq 64(%rsp),%r11
    movaps 96(%rsp),%xmm0
    movaps 112(%rsp),%xmm1
    movaps 128(%rsp),%xmm2
    movaps 144(%rsp),%xmm3
    movaps 160(%rsp),%xmm4
    movaps 176(%rsp),%xmm5
    movaps 192(%rsp),%xmm6
    movaps 208(%rsp),%xmm7
    movaps 224(%rsp),%xmm8
    movaps 240(%rsp),%xmm9
    movaps 256(%rsp),%xmm10
    movaps 272(%rsp),%xmm11
    movaps 288(%rsp),%xmm12
    movaps 304(%rsp),%xmm13
    movaps 320(%rsp),%xmm14
    movaps 336(%rsp),%xmm15
    addq $Lframe_size,%rsp
    popfq
.endmacro

.macro FRZZ_NAMED_PEAK
    movq 80(%rsp),%rax
    movq %rax,8(%rsp)
    movq 88(%rsp),%rax
    movq %rax,40(%rsp)
    FRZZ_RESTORE
    jmpq *_frzz_original_peak_name(%rip)
.endmacro

.globl _frzz_video_decoder_hook
_frzz_video_decoder_hook:
    FRZZ_SAVE
    movl %ecx,%edx
    leaq Lframe_size+8(%rsp),%rcx
    callq _frzz_video_ordinal
    movq %rax,32(%rsp)
    FRZZ_RESTORE
    jmpq *_frzz_original_open(%rip)

.globl _frzz_video_channels_hook
_frzz_video_channels_hook:
    // In the middle of the native factory the stack is already aligned.
    leaq -8(%rsp),%rsp
    FRZZ_SAVE
    leaq Lframe_size+104(%rsp),%rdi
    movl %ecx,%edx
    callq _frzz_video_main_channels
    movq %rax,32(%rsp)
    FRZZ_RESTORE
    leaq 8(%rsp),%rsp
    jmpq *_frzz_original_channels(%rip)

.globl _frzz_video_peak_hook
_frzz_video_peak_hook:
    FRZZ_SAVE
    movq %r8,%rsi
    leaq 80(%rsp),%rdx
    callq _frzz_video_peak_arguments
    FRZZ_NAMED_PEAK

.globl _frzz_video_peak_plain_hook
_frzz_video_peak_plain_hook:
    FRZZ_SAVE
    leaq Ldefault_extension(%rip),%rsi
    leaq 80(%rsp),%rdx
    callq _frzz_video_peak_arguments
    leaq Ldefault_extension(%rip),%rax
    cmpq %rax,88(%rsp)
    je Lplain_original
    FRZZ_NAMED_PEAK
Lplain_original:
    FRZZ_RESTORE
    jmpq *_frzz_original_peak_plain(%rip)

.globl _frzz_video_peak_legacy_hook
_frzz_video_peak_legacy_hook:
    FRZZ_SAVE
    leaq Ldefault_extension(%rip),%rsi
    leaq 80(%rsp),%rdx
    callq _frzz_video_peak_arguments
    leaq Ldefault_extension(%rip),%rax
    cmpq %rax,88(%rsp)
    je Llegacy_original
    movq $1,32(%rsp)
    FRZZ_NAMED_PEAK
Llegacy_original:
    FRZZ_RESTORE
    jmpq *_frzz_original_peak_legacy(%rip)

.cstring
Ldefault_extension:
    .asciz ".reapeaks"
#endif
