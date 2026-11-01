; DS3VR2 LIGHTGRID - the light-tile window seam (see lightgrid.h).
;
; GXScreenLightOrganizer's grid builder 0x140234E40 (cam in rdi) works out the view window its light tiles cover from the
; camera's fov and aspect alone - a CENTRED window - and joins its two paths at 0x14023514C:
;     mov edi, [rsp+28h] / movss [rsp+48h], xmm2 / movss [rsp+54h], xmm1 / movss [rsp+24h], xmm7
; Those 22 bytes become `jmp` to this stub (through a near trampoline). The window then lives in
;     left  xmm7  [rsp+24h]     right  xmm10 [rsp+58h]     top xmm9 [rsp+70h]
;     width xmm1  [rsp+54h]     height xmm2  [rsp+48h]
; (tile column x = left + px * width / W, row y = top - py * height / H; the row loop reloads all from the stack).
; The stub does the game's stores, lets ds3_lightgrid_fix(cam, frame) rewrite the stack copies for an eye camera,
; reloads the registers from them, does the game's `mov edi` and jumps back to 0x140235162. rsp is 0 (mod 16) here
; (the function's own frame, 7 pushes + 0x260), and nothing we touch is live except the window itself.

EXTERN ds3_lightgrid_fix:PROC
EXTERN ds3_lightgrid_back:QWORD

.code
ds3_lightgrid_stub PROC
    movss   dword ptr [rsp+48h], xmm2
    movss   dword ptr [rsp+54h], xmm1
    movss   dword ptr [rsp+24h], xmm7

    push    rax
    push    rcx
    push    rdx
    push    r8
    push    r9
    push    r10
    push    r11                             ; 38h bytes: rsp = 8 (mod 16)
    sub     rsp, 88h                        ; + shadow 20h, xmm0-5 at 20h..7Fh, 8 pad: rsp = 0 (mod 16)
    movdqa  xmmword ptr [rsp+20h], xmm0
    movdqa  xmmword ptr [rsp+30h], xmm1
    movdqa  xmmword ptr [rsp+40h], xmm2
    movdqa  xmmword ptr [rsp+50h], xmm3
    movdqa  xmmword ptr [rsp+60h], xmm4
    movdqa  xmmword ptr [rsp+70h], xmm5

    mov     rcx, rdi                        ; the camera
    lea     rdx, [rsp+0C0h]                 ; the game function's rsp (88h + 38h)
    call    ds3_lightgrid_fix

    movdqa  xmm0, xmmword ptr [rsp+20h]
    movdqa  xmm1, xmmword ptr [rsp+30h]
    movdqa  xmm2, xmmword ptr [rsp+40h]
    movdqa  xmm3, xmmword ptr [rsp+50h]
    movdqa  xmm4, xmmword ptr [rsp+60h]
    movdqa  xmm5, xmmword ptr [rsp+70h]
    add     rsp, 88h
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rdx
    pop     rcx
    pop     rax

    movss   xmm7,  dword ptr [rsp+24h]
    movss   xmm10, dword ptr [rsp+58h]
    movss   xmm9,  dword ptr [rsp+70h]
    movss   xmm1,  dword ptr [rsp+54h]
    movss   xmm2,  dword ptr [rsp+48h]
    mov     edi, dword ptr [rsp+28h]
    jmp     qword ptr [ds3_lightgrid_back]
ds3_lightgrid_stub ENDP
END
