; The first shader on sapphire's Adreno: 64 invocations, each stores 0xc0de0000 + its local
; invocation id at buf[id]. c0.xy = the buffer's GPU address, uploaded by the kernel.
@localsize 64, 1, 1
@invocationid(r0.x) ; r0.xyz
@const(c0.x) 0, 0, 0, 0
mov.u32u32 r1.x, c0.x
mov.u32u32 r1.y, c0.y
mov.u32u32 r2.x, 0xc0de0000
(rpt5)nop
add.u r2.x, r2.x, r0.x
(rpt5)nop
stg.a.u32 g[r1.x+(r0.x<<2)], r2.x, 1
end
nop
