#version 450
// The sampler has an immutable VkSamplerYcbcrConversion (BT.709, narrow range), so texture() already returns gamma-encoded RGB.
// The render target is an *_SRGB image (the format SteamVR is known to accept), whose hardware encodes linear values on write,
// so linearise first: the bytes that land in the image are then exactly the sRGB-encoded values we decoded.
layout(set = 0, binding = 0) uniform sampler2D video;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
vec3 srgb_to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }
void main() { outColor = vec4(srgb_to_linear(texture(video, uv).rgb), 1.0); }
