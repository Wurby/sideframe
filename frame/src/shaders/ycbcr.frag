#version 450
// The sampler has an immutable VkSamplerYcbcrConversion (BT.709, narrow range), so texture() already returns gamma-encoded RGB.
// The render target is an *_SRGB image (the format SteamVR is known to accept), whose hardware encodes linear values on write,
// so linearise first: the bytes that land in the image are then exactly the sRGB-encoded values we decoded.
layout(set = 0, binding = 0) uniform sampler2D video;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
vec3 srgb_to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }
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
    outColor = vec4(c, 1.0);
}
