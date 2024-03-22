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
vec3 random_dir() {
    int rand = int(random_u32());

    const float randScale = (1.0 / (1 << 15));
    float y = float(rand >> 16) * randScale;  // signed
    float a = float(rand & 0x7FFFu) * (randScale * 6.283185307179586);

    float sy = sqrt(1.0 - y * y);
    return vec3(sin(a) * sy, y, cos(a) * sy);
}

void random_init_uv(vec2 uv, uint time) {
    _pcg_state = floatBitsToUint(uv.x);
    _pcg_state = random_u32() ^ floatBitsToUint(uv.y) ^ time;
}