#pragma once

// HLSL for the two minimap passes, compiled once at start-up.
//  - Tile pass: one quad per LOD tile into the compose target (diffuse x hillshade + water tint).
//  - Overlay pass: one screen quad that rotates and masks the compose target (and the arrow in the test tool).
//  - Icon pass: every icon in one instanced draw from the atlas, clipped to the same mask.
//  - Nav pass (interiors): navmesh floor triangles and thick wall edges into the compose target.

namespace MM::Shaders
{
	inline constexpr const char kSource[] = R"HLSL(
cbuffer TileCB : register(b0)
{
	float4 tRect;   // NDC: left, top, right, bottom
	float4 tLight;  // normalized light xyz, w = 1 / light.z
	float4 tShade;  // ambient, diffuse, min, max
	float4 tGrade;  // brightness, saturation, hasNormal, hasWater
	float4 tWater;  // rgb, alpha
	float4 tFog;    // fog mask uv of the tile's north-west and south-east corners
	float4 tFogOn;  // x = darkness, y = on
};

cbuffer OverlayCB : register(b0)
{
	float4 oRect;   // NDC: left, top, right, bottom
	float4 oGeo;    // size px, target size px, offset east px, offset north px
	float4 oRot;    // cos/sin of map angle, cos/sin of arrow angle
	float4 oStyle;  // opacity, round, hasMap, arrow size px
	float4 oOut;    // x = write linear colour (sRGB / float HUD target), y = brightness scale
	float4 oLook;   // map style: saturation, contrast
	float4 oTint;   // map style: rgb tint
};

cbuffer IconCB : register(b0)
{
	float4 iView;  // viewport w, h, opacity, round
	float4 iMask;  // minimap centre x, y (px), half size px, unused
	float4 iOut;   // x = write linear colour (sRGB / float HUD target), y = brightness scale
};

cbuffer NavCB : register(b0)
{
	float4 nView;   // centre x, y (world), NDC per world unit, wall half width in NDC
	float4 nBand;   // floor band min z, max z, pass (0 = player's floor, 1 = other floors), other floors alpha
	float4 nColor;  // rgb
};

Texture2D    texA : register(t0);
Texture2D    texB : register(t1);
Texture2D    texC : register(t2);
Texture2D    texFog : register(t3);
SamplerState samp : register(s0);

// The minimap's colours are sRGB. A HUD target that is sRGB-typed or float (HDR) expects linear values.
// o.y = brightness (times the HDR brightness on float targets), applied in every colour mode.
float3 OutColor(float3 c, float4 o)
{
	return (o.x > 0.5 ? pow(saturate(c), 2.2) : c) * o.y;
}

struct VSOut
{
	float4 pos : SV_Position;
	float2 uv  : TEXCOORD0;
};

float2 Corner(uint id) { return float2(id & 1, id >> 1); }  // triangle strip: TL, TR, BL, BR

VSOut VSTile(uint id : SV_VertexID)
{
	float2 t = Corner(id);
	VSOut o;
	o.pos = float4(lerp(tRect.x, tRect.z, t.x), lerp(tRect.y, tRect.w, t.y), 0, 1);
	o.uv = t;  // tile image: top = north, so UV (0,1) = south-west
	return o;
}

float4 PSTile(VSOut i) : SV_Target
{
	float3 d = texA.Sample(samp, i.uv).rgb;
	float3 n = float3(0, 0, 1);
	if (tGrade.z > 0.5) {
		float3 t = texB.Sample(samp, i.uv).rgb * 2 - 1;  // object space: R = east, G = up, B = north
		n = normalize(float3(t.r, t.b, t.g) + 1e-6);
		// Terrain never faces down. A normal that does comes from a damaged map (texture tools that resize
		// with alpha-weighted filtering wreck xLODGen normals, whose alpha is near zero): use flat ground.
		if (n.z < 0.2)
			n = float3(0, 0, 1);
	}
	float s = clamp(tShade.x + tShade.y * saturate(dot(n, tLight.xyz)) * tLight.w, tShade.z, tShade.w);
	float3 c = saturate(d * s * tGrade.x);
	float g = (c.r + c.g + c.b) / 3;
	c = saturate(g + (c - g) * tGrade.y);
	if (tGrade.w > 0.5) {
		c = lerp(c, tWater.rgb, tWater.a * texC.Sample(samp, i.uv).r);
	}
	if (tFogOn.y > 0.5) {
		float explored = texFog.Sample(samp, lerp(tFog.xy, tFog.zw, i.uv)).r;
		c *= lerp(1 - tFogOn.x, 1, explored);
	}
	return float4(c, 1);
}

VSOut VSOverlay(uint id : SV_VertexID)
{
	float2 t = Corner(id);
	VSOut o;
	o.pos = float4(lerp(oRect.x, oRect.z, t.x), lerp(oRect.y, oRect.w, t.y), 0, 1);
	o.uv = (t - 0.5) * oGeo.x;  // px offset from the centre, y down
	return o;
}

float Cross2(float2 a, float2 b) { return a.x * b.y - a.y * b.x; }

// Signed distance to a triangle (negative inside).
float SdTriangle(float2 p, float2 a, float2 b, float2 c)
{
	float2 e0 = b - a, e1 = c - b, e2 = a - c;
	float2 v0 = p - a, v1 = p - b, v2 = p - c;
	float2 q0 = v0 - e0 * saturate(dot(v0, e0) / dot(e0, e0));
	float2 q1 = v1 - e1 * saturate(dot(v1, e1) / dot(e1, e1));
	float2 q2 = v2 - e2 * saturate(dot(v2, e2) / dot(e2, e2));
	float s = sign(Cross2(e0, e2));
	float2 d = min(min(float2(dot(q0, q0), s * Cross2(v0, e0)), float2(dot(q1, q1), s * Cross2(v1, e1))), float2(dot(q2, q2), s * Cross2(v2, e2)));
	return -sqrt(d.x) * sign(d.y);
}

float4 PSOverlay(VSOut i) : SV_Target
{
	float2 q = i.uv;
	float half = oGeo.x * 0.5;
	float dist = oStyle.y > 0.5 ? length(q) : max(abs(q.x), abs(q.y));
	float cover = saturate(half - dist + 0.5);
	if (cover <= 0)
		discard;

	float3 col = float3(0.07, 0.08, 0.09);
	if (oStyle.z > 0.5) {
		float2 m = float2(q.x * oRot.x - q.y * oRot.y, -q.x * oRot.y - q.y * oRot.x);  // screen -> east/north px
		float2 p = float2(oGeo.y * 0.5 + oGeo.z + m.x, oGeo.y * 0.5 - oGeo.w - m.y);
		col = texA.Sample(samp, p / oGeo.y).rgb;
		float g = dot(col, float3(0.299, 0.587, 0.114));
		col = lerp(float3(g, g, g), col, oLook.x);
		col = saturate(((col - 0.5) * oLook.y + 0.5) * oTint.rgb);
	}

	// Rim.
	col = lerp(col, float3(0.80, 0.76, 0.64), saturate(dist - (half - 2.5)));

	// Player arrow: forward = clockwise angle from screen up. Size 0 = drawn as an icon instead.
	if (oStyle.w <= 0)
		return float4(OutColor(col, oOut), cover * oStyle.x);
	float2 fwd = float2(oRot.w, -oRot.z);
	float2 rgt = float2(oRot.z, oRot.w);
	float2 a = float2(dot(q, rgt), dot(q, fwd));
	float L = oStyle.w;
	float2 tip = float2(0, L), notch = float2(0, -0.3 * L);
	float sd = min(SdTriangle(a, tip, float2(-0.7 * L, -0.65 * L), notch), SdTriangle(a, tip, notch, float2(0.7 * L, -0.65 * L)));
	col = lerp(col, float3(0.05, 0.05, 0.05), saturate(1.5 - sd));
	col = lerp(col, float3(1.0, 0.97, 0.9), saturate(0.5 - sd));

	return float4(OutColor(col, oOut), cover * oStyle.x);
}

struct IconVSOut
{
	float4 pos  : SV_Position;
	float2 uv   : TEXCOORD0;
	float4 col  : COLOR0;
	float  clip : TEXCOORD1;
};

// Per instance: INST0 = centre px, size px, rotation (clockwise); INST1.x = clip; INST2 = atlas uv rect; INST3 = tint.
IconVSOut VSIcon(uint id : SV_VertexID, float4 a : INST0, float2 b : INST1, float4 uvr : INST2, float4 tint : INST3)
{
	float2 t = Corner(id);
	float2 l = (t - 0.5) * a.z;  // y down
	float c = cos(a.w), s = sin(a.w);
	float2 px = a.xy + float2(l.x * c - l.y * s, l.x * s + l.y * c);
	IconVSOut o;
	o.pos = float4(px.x / iView.x * 2 - 1, 1 - px.y / iView.y * 2, 0, 1);
	o.uv = lerp(uvr.xy, uvr.zw, t);
	o.col = tint;
	o.clip = b.x;
	return o;
}

float4 PSIcon(IconVSOut i) : SV_Target
{
	float4 t = texA.Sample(samp, i.uv);
	float4 c = float4(t.rgb * i.col.rgb, t.a * i.col.a * iView.z);
	if (i.clip > 0.5) {
		float2 q = i.pos.xy - iMask.xy;
		float d = iView.w > 0.5 ? length(q) : max(abs(q.x), abs(q.y));
		c.a *= saturate(iMask.z - d + 0.5);
	}
	if (c.a <= 0.002)
		discard;
	return float4(OutColor(c.rgb, iOut), c.a);
}

struct NavVSOut
{
	float4 pos : SV_Position;
	float  z   : TEXCOORD0;
};

float2 NavNdc(float3 p) { return (p.xy - nView.xy) * nView.z; }  // north up: world y -> NDC y

NavVSOut VSNav(float3 p : POSITION)
{
	NavVSOut o;
	o.pos = float4(NavNdc(p), 0, 1);
	o.z = p.z;
	return o;
}

// Wall edge as a quad: each corner knows its own end (p), the other end (q) and its side (+1/-1).
// Pushed out sideways and past the end by the half width, so walls join at corners.
NavVSOut VSNavWall(float3 p : POSITION, float3 q : TEXCOORD0, float side : TEXCOORD1)
{
	float2 a = NavNdc(p), b = NavNdc(q);
	float2 d = a - b;
	float  len = length(d);
	d = len > 1e-7 ? d / len : float2(1, 0);
	float2 n = float2(-d.y, d.x) * side;
	NavVSOut o;
	o.pos = float4(a + (d + n) * nView.w, 0, 1);
	o.z = p.z;
	return o;
}

float4 PSNav(NavVSOut i) : SV_Target
{
	bool inBand = i.z >= nBand.x && i.z <= nBand.y;
	bool other = nBand.z > 0.5;
	if (inBand == other)
		discard;
	return float4(nColor.rgb, other ? nBand.w : 1.0);
}
)HLSL";
}
