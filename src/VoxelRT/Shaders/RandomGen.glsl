layout(rg8ui) uniform uimage2D u_STBlueNoiseTex;

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

// Generate 2 blue noise samples
// https://developer.nvidia.com/blog/rendering-in-real-time-with-spatiotemporal-blue-noise-textures-part-1/
vec2 blueNoise(ivec2 pos, int frameIdx, int sampleIdx) {
    // R2 quasirandom sequence
    vec2 sampleOffset = fract(sampleIdx * vec2(0.75487766624669276005, 0.56984029099805326591) + 0.5);
    pos = (pos + ivec2(sampleOffset * 128)) & 127;
    pos.y += (frameIdx & 63) * 128;
    
    return (imageLoad(u_STBlueNoiseTex, pos).xy + 0.5) / 256.0;
}

vec3 random_dir(int frameIdx, int sampleIdx) {
    // int rand = int(random_u32());
    // const float randScale = (1.0 / (1 << 15));
    // float y = float(rand >> 16) * randScale;  // signed
    // float a = float(rand & 0x7FFFu) * (randScale * 6.283185307179586);

    vec2 n = blueNoise(ivec2(gl_GlobalInvocationID.xy), frameIdx, sampleIdx);
    float y = n.x * 2 - 1;
    float a = n.y * 6.283185307179586;

    float sy = sqrt(1.0 - y * y);
    return vec3(sin(a) * sy, y, cos(a) * sy);
}