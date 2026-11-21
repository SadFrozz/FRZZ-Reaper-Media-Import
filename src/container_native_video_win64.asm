EXTERN frzz_video_ordinal:PROC
EXTERN frzz_original_open:QWORD
EXTERN frzz_video_peak_arguments:PROC
EXTERN frzz_original_peak_name:QWORD
EXTERN frzz_video_main_channels:PROC
EXTERN frzz_original_channels:QWORD

.code
; REAPER's private callers can retain results in volatile registers. Forward
; their exact register/stack state, changing only the input arguments we own.
frzz_video_decoder_hook PROC FRAME
    pushfq
    .allocstack 8
    sub rsp, 0C0h
    .allocstack 0C0h
    .endprolog
    mov [rsp+20h], rax
    mov [rsp+28h], rcx
    mov [rsp+30h], rdx
    mov [rsp+38h], r8
    mov [rsp+40h], r10
    mov [rsp+48h], r11
    movaps [rsp+50h], xmm0
    movaps [rsp+60h], xmm1
    movaps [rsp+70h], xmm2
    movaps [rsp+80h], xmm3
    movaps [rsp+90h], xmm4
    movaps [rsp+0A0h], xmm5
    mov r8d, r9d
    lea r9, [rsp+0C8h]
    call frzz_video_ordinal
    mov r9d, eax
    movaps xmm0, [rsp+50h]
    movaps xmm1, [rsp+60h]
    movaps xmm2, [rsp+70h]
    movaps xmm3, [rsp+80h]
    movaps xmm4, [rsp+90h]
    movaps xmm5, [rsp+0A0h]
    mov rax, [rsp+20h]
    mov rcx, [rsp+28h]
    mov rdx, [rsp+30h]
    mov r8, [rsp+38h]
    mov r10, [rsp+40h]
    mov r11, [rsp+48h]
    add rsp, 0C0h
    popfq
    jmp qword ptr [frzz_original_open]
frzz_video_decoder_hook ENDP

frzz_video_peak_hook PROC FRAME
    pushfq
    .allocstack 8
    sub rsp, 0E0h
    .allocstack 0E0h
    .endprolog
    mov [rsp+20h], rax
    mov [rsp+28h], rcx
    mov [rsp+30h], rdx
    mov [rsp+38h], r8
    mov [rsp+40h], r10
    mov [rsp+48h], r11
    movaps [rsp+50h], xmm0
    movaps [rsp+60h], xmm1
    movaps [rsp+70h], xmm2
    movaps [rsp+80h], xmm3
    movaps [rsp+90h], xmm4
    movaps [rsp+0A0h], xmm5
    mov [rsp+0B0h], r9
    mov rdx, [rsp+110h]
    lea r8, [rsp+0C0h]
    call frzz_video_peak_arguments
    mov rcx, [rsp+0C0h]
    mov rax, [rsp+0C8h]
    mov [rsp+110h], rax
    mov r9, [rsp+0B0h]
    movaps xmm0, [rsp+50h]
    movaps xmm1, [rsp+60h]
    movaps xmm2, [rsp+70h]
    movaps xmm3, [rsp+80h]
    movaps xmm4, [rsp+90h]
    movaps xmm5, [rsp+0A0h]
    mov rax, [rsp+20h]
    mov rdx, [rsp+30h]
    mov r8, [rsp+38h]
    mov r10, [rsp+40h]
    mov r11, [rsp+48h]
    add rsp, 0E0h
    popfq
    jmp qword ptr [frzz_original_peak_name]
frzz_video_peak_hook ENDP

; Immediately before the native FFmpeg factory records the channel count.
frzz_video_channels_hook PROC FRAME
    pushfq
    .allocstack 8
    sub rsp, 0C8h
    .allocstack 0C8h
    .endprolog
    mov [rsp+20h], rax
    mov [rsp+28h], rcx
    mov [rsp+30h], rdx
    mov [rsp+38h], r8
    mov [rsp+40h], r9
    mov [rsp+48h], r10
    mov [rsp+50h], r11
    movaps [rsp+60h], xmm0
    movaps [rsp+70h], xmm1
    movaps [rsp+80h], xmm2
    movaps [rsp+90h], xmm3
    movaps [rsp+0A0h], xmm4
    movaps [rsp+0B0h], xmm5
    lea rcx, [rbp+68h]
    mov rdx, r8
    mov r8d, ebx
    call frzz_video_main_channels
    mov ebx, eax
    movaps xmm0, [rsp+60h]
    movaps xmm1, [rsp+70h]
    movaps xmm2, [rsp+80h]
    movaps xmm3, [rsp+90h]
    movaps xmm4, [rsp+0A0h]
    movaps xmm5, [rsp+0B0h]
    mov rax, [rsp+20h]
    mov rcx, [rsp+28h]
    mov rdx, [rsp+30h]
    mov r8, [rsp+38h]
    mov r9, [rsp+40h]
    mov r10, [rsp+48h]
    mov r11, [rsp+50h]
    add rsp, 0C8h
    popfq
    jmp qword ptr [frzz_original_channels]
frzz_video_channels_hook ENDP
END
