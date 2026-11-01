// SMOKEFACE domain shader source (re/smokeface/smoke_ds.hlsl; signatures checked identical to the game's 7211bcbdc0fd262c)
static const char kSmokeDsSrc[] = R"HLSL(
// SMOKEFACE: Dark Souls III's particle-card domain shader (7211bcbdc0fd262c), instruction for instruction, except that the card's
// two edge directions (the particle's own matrix columns 0 / 1) are turned by fixRow = game camera^T x eye camera, so the card
// faces the head-free game camera instead of the eye. fixRow = identity reproduces the game's shader.
cbuffer C0 : register(b0) { float4 cb0[28]; };
cbuffer Fix : register(b13) { float4 fixRow[3]; };
struct CP {
    float4 p : POSITION; float4 c0 : COLOR0; float4 c1 : COLOR1; float4 c2 : COLOR2; float4 c3 : COLOR3; float4 c4 : COLOR4;
    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float3 t3 : TEXCOORD3; float2 t4 : TEXCOORD4;
    float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7;
};
struct PC { float e[4] : SV_TessFactor; float i[2] : SV_InsideTessFactor; };
struct DSOut {
    float4 o0 : COLOR0; float4 o1 : TEXCOORD0; float4 o2 : TEXCOORD1; float2 o3xy : TEXCOORD2; float o3z : TEXCOORD9;
    float4 o4 : TEXCOORD3; float4 o5 : TEXCOORD4; float4 o6 : TEXCOORD5; float4 o7 : TEXCOORD6; float4 o8 : TEXCOORD7;
    float4 o9 : TEXCOORD8; float4 pos : SV_Position;
};
[domain("quad")]
DSOut main(PC pc, float2 dom : SV_DomainLocation, const OutputPatch<CP, 1> ip) {
    CP v = ip[0];
    DSOut o;
    bool rx = 0.0001 >= cb0[23].y, ry = 0.0001 >= cb0[23].w, rz = rx && ry;
    float4 r1 = float4(v.p.xyz, 1.0);
    float2 d = dom - 0.5;
    float3 base = float3(dot(r1, v.t5), dot(r1, v.t6), dot(r1, v.t7));
    float3 c0 = float3(v.t5.x, v.t6.x, v.t7.x), c1 = float3(v.t5.y, v.t6.y, v.t7.y);
    float3 rc0 = float3(dot(fixRow[0].xyz, c0), dot(fixRow[1].xyz, c0), dot(fixRow[2].xyz, c0));
    float3 rc1 = float3(dot(fixRow[0].xyz, c1), dot(fixRow[1].xyz, c1), dot(fixRow[2].xyz, c1));
    float3 w = base + (rc0 * d.x + rc1 * d.y) * fixRow[0].w;   // SMOKESCALE: card size (1 = the game's)
    float3 t = cb0[24].xyz - w;
    float len = sqrt(dot(t, t));
    float4 q = len - cb0[23].zxxz;
    float a = 1.0 - saturate(q.x / cb0[23].w);
    float b = 1.0 - saturate(q.y / -cb0[23].y);
    float s = rx ? a : b;
    bool ror = rx || ry;
    float s2 = rz ? 1.0 : s;
    float2 zw = 1.0 - saturate(float2(-1.0 / cb0[23].y, 1.0 / cb0[23].w) * q.zw);
    float s3 = zw.y * zw.x;
    float fade = ror ? s2 : s3;
    float4 col = v.c0 * v.c4;
    o.o0 = float4(col.xyz, saturate(fade * col.w));
    float4 rr = (dom.xyxy - 0.5) * float4(1.0, -1.0, 1.0, -1.0) + 0.5;
    float2 r1xy = rr.zw * v.t1.zw + v.t4.xx;
    float4 rr2 = rr * v.t2 + 0.5;
    rr2 = -v.t2 * 0.5 + rr2;
    r1xy = -v.t1.zw * v.t4.xx + r1xy;
    o.o1 = float4(r1xy + v.t0.xy, rr2.xy + v.t0.zw);
    o.o2 = float4(rr2.zw + v.t1.xy, r1xy + v.t3.xy);
    float4 wp = float4(w, 1.0);
    float4 vpos = float4(dot(wp, cb0[4]), dot(wp, cb0[5]), dot(wp, cb0[6]), dot(wp, cb0[7]));
    // keeps the patch-constant inputs declared exactly as the game's shader (never true: tess factors are >= 0)
    const float tf = pc.e[0] + pc.e[1] + pc.e[2] + pc.e[3] + pc.i[0] + pc.i[1];
    o.o6 = float4(cb0[22].xyz - w, tf < -1e30 ? 1.0 : 0.0);
    float cw = dot(vpos, cb0[11]);
    o.o3xy = float2(v.t4.y, saturate((cw - cb0[16].x) * cb0[16].y));
    o.o3z = v.t3.z;
    float cx = dot(vpos, cb0[8]), cy = dot(vpos, cb0[9]);
    o.o4.xy = float2(cx, cy) / cw * float2(0.5, -0.5) + 0.5;
    float m = cb0[27].x * v.p.w;
    float r0x = cw - (m * 0.5 + cb0[27].y);
    o.o4.w = cb0[27].y * 2.0 + m * 0.5;
    float y2 = cw - r0x;
    float zz = 1.0 + cb0[21].w;
    float ww = min(zz - r0x, 1.0);
    r0x = (r0x < zz) ? (ww * y2 + r0x) : r0x;
    o.o4.z = r0x;
    float cz = (cb0[21].z / cb0[21].y * r0x - cb0[21].x / cb0[21].y) * (cw / r0x);
    o.o5 = float4(cx, cy, cz, cw);
    o.pos = o.o5;
    o.o7 = v.c1; o.o8 = v.c2; o.o9 = v.c3;
    return o;
}
)HLSL";
