#version 450
// The sampler has an immutable VkSamplerYcbcrConversion (BT.709, narrow range), so texture() already returns gamma-encoded RGB.
// The render target is an *_SRGB image (the format SteamVR is known to accept), whose hardware encodes linear values on write,
// so linearise first: the bytes that land in the image are then exactly the sRGB-encoded values we decoded.
layout(set = 0, binding = 0) uniform sampler2D video;
layout(push_constant) uniform PC { vec4 cog; } pc;   // settings cog: x, y (pixels, origin top-left), radius, alpha
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
vec3 srgb_to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }
// The settings cog: a white gear on a soft dark disc, drawn over the corner of the picture. Fully transparent when alpha is 0.
vec3 draw_cog(vec3 c) {
    float a = pc.cog.w; if (a <= 0.0) return c;
    vec2 d = gl_FragCoord.xy - pc.cog.xy; float R = pc.cog.z, r = length(d) / R;
    if (r > 1.5) return c;
    float teeth = 0.5 + 0.5 * cos(8.0 * atan(d.y, d.x)), aa = 1.3 / R;
    float outer = 0.74 + 0.17 * smoothstep(0.35, 0.65, teeth);
    float body = 1.0 - smoothstep(outer - aa, outer + aa, r), hole = 1.0 - smoothstep(0.30 - aa, 0.30 + aa, r);
    float disc = 1.0 - smoothstep(1.22 - aa, 1.22 + aa, r);
    c = mix(c, vec3(0.0), 0.60 * a * disc);
    return mix(c, vec3(1.0), a * clamp(body - hole, 0.0, 1.0));
}

// When the picture is shrunk (the Mac's display is wider than the headset can show), one bilinear tap skips source pixels and text shimmers/blurs.
// Average four taps spread over the output pixel's footprint instead (in linear light), which keeps thin strokes. Not shrunk: a single tap, untouched.
void main() {
    vec2 dx = dFdx(uv), dy = dFdy(uv), sz = vec2(textureSize(video, 0));
    float shrink = max(length(dx * sz), length(dy * sz));
    vec3 c;
    if (shrink < 1.1) c = srgb_to_linear(texture(video, uv).rgb);
    else {
        c = 0.25 * (srgb_to_linear(texture(video, uv + 0.25 * (dx + dy)).rgb) + srgb_to_linear(texture(video, uv + 0.25 * (dx - dy)).rgb) +
                    srgb_to_linear(texture(video, uv + 0.25 * (-dx + dy)).rgb) + srgb_to_linear(texture(video, uv + 0.25 * (-dx - dy)).rgb));
    }
    outColor = vec4(draw_cog(c), 1.0);
}
