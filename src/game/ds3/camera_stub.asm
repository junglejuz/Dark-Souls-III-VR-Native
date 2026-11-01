; DS3VR2 - the camera seam.
;
; Dark Souls III's ChrCam director is Arxan-protected: its entry is a `jmp` into the packed second .text, and a guard
; puts the function's own bytes back a few seconds into the session. Measured 2026-10-30 (HOOKLIVE): a MinHook at the
; copy-out inside that function fired 73 times and then stopped, with the game's original `E8 ...` restored, while the
; two `call director` SITES went on firing 35 times a second all session. So nothing of ours is written inside the
; guarded function. The call sites are re-pointed at this wrapper, which does both halves of the seam:
;   before the director - put the game's own camera values back (A2.3: never let the engine read our output back)
;   after  the director - write the head / eye pose into the ChrCam it has just finished
;
; The director's signature is (ChrCam* rcx, float dt in xmm1, void* r8, void* r9, byte on the stack), so this is a
; real wrapper and rebuilds the argument area; it does not tail-jump. On entry rsp = 8 (mod 16) and the caller's
; fifth argument sits at [rsp+0x28].
;
; Frame (0x98 bytes, rsp = 0 mod 16 throughout):
;   +0x00..0x18  shadow space for the calls we make     +0x20  the fifth argument we pass on
;   +0x28 rcx    +0x30 rdx   +0x38 r8   +0x40 r9
;   +0x50 xmm0   +0x60 xmm1  +0x70 xmm2 +0x80 xmm3      (all 16-byte aligned)
;   the caller's own fifth argument is then at +0xD0 (0x98 frame + 0x10 pushes + 0x28)

EXTERN ds3_restore_camera:PROC
EXTERN ds3_after_director:PROC
EXTERN ds3_director_abs:QWORD

.code
ds3_dir_stub PROC
    push    rbx
    push    rsi
    sub     rsp, 98h
    mov     qword ptr [rsp+28h], rcx
    mov     qword ptr [rsp+30h], rdx
    mov     qword ptr [rsp+38h], r8
    mov     qword ptr [rsp+40h], r9
    movdqa  xmmword ptr [rsp+50h], xmm0
    movdqa  xmmword ptr [rsp+60h], xmm1
    movdqa  xmmword ptr [rsp+70h], xmm2
    movdqa  xmmword ptr [rsp+80h], xmm3

    call    ds3_restore_camera              ; rcx is still the ChrCam

    mov     rax, qword ptr [rsp+0D0h]       ; the caller's fifth argument
    mov     qword ptr [rsp+20h], rax        ; ... passed on as ours
    mov     rcx, qword ptr [rsp+28h]
    mov     rdx, qword ptr [rsp+30h]
    mov     r8,  qword ptr [rsp+38h]
    mov     r9,  qword ptr [rsp+40h]
    movdqa  xmm0, xmmword ptr [rsp+50h]
    movdqa  xmm1, xmmword ptr [rsp+60h]
    movdqa  xmm2, xmmword ptr [rsp+70h]
    movdqa  xmm3, xmmword ptr [rsp+80h]
    call    qword ptr [ds3_director_abs]

    mov     rbx, rax                        ; keep the director's result
    mov     rcx, qword ptr [rsp+28h]        ; ChrCam
    call    ds3_after_director
    mov     rax, rbx

    add     rsp, 98h
    pop     rsi
    pop     rbx
    ret
ds3_dir_stub ENDP
END
