layout(rgba8ui) uniform uimage2D u_BlueNoiseScramblingTex;
layout(rgba8ui) uniform uimage2D u_BlueNoiseSobolTex;

uint _pcg_state;

// https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/
uint random_u32() {
    uint state = _pcg_state;
    _pcg_state = _pcg_state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float random() {
    return float(random_u32()) * (1.0 / 4294967296.0);
}

void random_init(uvec3 p) {
    uvec2 q = 1103515245U * p.xy;
    uint  n = q.x ^ (q.y >> 3U);
    _pcg_state = n + p.z * 12345u;
}
void random_init_uv(vec2 uv, uint time) {
    _pcg_state = floatBitsToUint(uv.x);
    _pcg_state = random_u32() ^ floatBitsToUint(uv.y) ^ time;
}

// Generate 2 blue noise samples (1spp / 128 dims)
// https://belcour.github.io/blog/research/publication/2019/06/17/sampling-bluenoise.html
vec2 blueNoise(ivec2 pos, int idx, int dim) {
	pos &= 127;
	idx &= 255;
	dim &= 127;

	uvec4 value = imageLoad(u_BlueNoiseSobolTex, ivec2(idx, dim >> 1));
    value ^= imageLoad(u_BlueNoiseScramblingTex, pos);

    if ((dim & 1) != 0) value.xy = value.zw;

	return (0.5 + value.xy) / 256.0;
}

vec3 random_dir(int sampleIdx) {
    // int rand = int(random_u32());
    // const float randScale = (1.0 / (1 << 15));
    // float y = float(rand >> 16) * randScale;  // signed
    // float a = float(rand & 0x7FFFu) * (randScale * 6.283185307179586);

    vec2 n = blueNoise(ivec2(gl_GlobalInvocationID.xy), u_FrameNo, sampleIdx);
    float y = n.x * 2 - 1;
    float a = n.y * 6.283185307179586;

    float sy = sqrt(1.0 - y * y);
    return vec3(sin(a) * sy, y, cos(a) * sy);
}