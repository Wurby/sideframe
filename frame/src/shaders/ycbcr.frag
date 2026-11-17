#version 450
// The sampler has an immutable VkSamplerYcbcrConversion (BT.709, narrow range), so texture() already returns gamma-encoded RGB.
// The render target is an *_SRGB image (the format SteamVR is known to accept), whose hardware encodes linear values on write,
// so linearise first: the bytes that land in the image are then exactly the sRGB-encoded values we decoded.
layout(set = 0, binding = 0) uniform sampler2D video;
layout(push_constant) uniform PC { vec4 cog; vec4 misc; } pc;   // cog: x, y (pixels, origin top-left), radius, alpha; misc.x = 1 when drawing the strip beside the picture; misc.y = y of the disconnect button (same x and radius), misc.z = its alpha
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
vec3 srgb_to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }
// The settings cog, drawn in the transparent strip beside the picture: a white gear on a soft dark disc. Straight (non-premultiplied) alpha.
vec4 cog_pixel(vec2 c, float R, float a) {
    vec2 d = gl_FragCoord.xy - c; float r = length(d) / R;
    if (a <= 0.0 || r > 1.5) return vec4(0.0);
    float teeth = 0.5 + 0.5 * cos(8.0 * atan(d.y, d.x)), aa = 1.3 / R;
    float outer = 0.74 + 0.17 * smoothstep(0.35, 0.65, teeth);
    float body = 1.0 - smoothstep(outer - aa, outer + aa, r), hole = 1.0 - smoothstep(0.30 - aa, 0.30 + aa, r);
    float disc = 1.0 - smoothstep(1.22 - aa, 1.22 + aa, r);
    float sa = a * clamp(body - hole, 0.0, 1.0), da = 0.55 * a * disc;
    float oa = sa + da * (1.0 - sa);
    return vec4(vec3(sa / max(oa, 1e-4)), oa);          // white where the gear is, black in the disc around it
}
// The disconnect button: a monitor with a cross over its screen, on the same soft disc.
float sd_rbox(vec2 p, vec2 b, float r) { vec2 q = abs(p) - b + r; return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r; }
float seg_dist(vec2 p, vec2 a, vec2 b) { vec2 pa = p - a, ba = b - a; return length(pa - ba * clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0)); }
vec4 disconnect_pixel(vec2 c, float R, float a) {
    vec2 d = gl_FragCoord.xy - c; float r = length(d) / R;
    if (a <= 0.0 || r > 1.5) return vec4(0.0);
    float aa = 1.3 / R; vec2 p = vec2(d.x, -d.y) / R;     // p.y is "up" (the strip's y axis points down)
    float screen = 1.0 - smoothstep(-aa, aa, abs(sd_rbox(p - vec2(0.0, 0.14), vec2(0.82, 0.52), 0.12)) - 0.06);
    float neck = 1.0 - smoothstep(-aa, aa, sd_rbox(p - vec2(0.0, -0.50), vec2(0.07, 0.10), 0.0));
    float base = 1.0 - smoothstep(-aa, aa, sd_rbox(p - vec2(0.0, -0.64), vec2(0.38, 0.07), 0.03));
    float cross = 1.0 - smoothstep(0.065 - aa, 0.065 + aa, min(seg_dist(p, vec2(-0.30, -0.16), vec2(0.30, 0.44)), seg_dist(p, vec2(-0.30, 0.44), vec2(0.30, -0.16))));
    float disc = 1.0 - smoothstep(1.22 - aa, 1.22 + aa, r);
    float sa = a * clamp(max(max(screen, neck), max(base, cross)), 0.0, 1.0), da = 0.55 * a * disc;
    float oa = sa + da * (1.0 - sa);
    return vec4(vec3(sa / max(oa, 1e-4)), oa);
}

// When the picture is shrunk (the Mac's display is wider than the headset can show), one bilinear tap skips source pixels and text shimmers/blurs.
// Average four taps spread over the output pixel's footprint instead (in linear light), which keeps thin strokes. Not shrunk: a single tap, untouched.
void main() {
    if (pc.misc.x > 0.5) {
        vec4 g = cog_pixel(pc.cog.xy, pc.cog.z, pc.cog.w), q = disconnect_pixel(vec2(pc.cog.x, pc.misc.y), pc.cog.z, pc.misc.z);
        outColor = g.a >= q.a ? g : q; return;      // the two never overlap
    }
    vec2 dx = dFdx(uv), dy = dFdy(uv), sz = vec2(textureSize(video, 0));
    float shrink = max(length(dx * sz), length(dy * sz));
    vec3 c;
    if (shrink < 1.1) c = srgb_to_linear(texture(video, uv).rgb);
    else {
        c = 0.25 * (srgb_to_linear(texture(video, uv + 0.25 * (dx + dy)).rgb) + srgb_to_linear(texture(video, uv + 0.25 * (dx - dy)).rgb) +
                    srgb_to_linear(texture(video, uv + 0.25 * (-dx + dy)).rgb) + srgb_to_linear(texture(video, uv + 0.25 * (-dx - dy)).rgb));
    }
    outColor = vec4(c, 1.0);
}
