#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

// binding=0 is the shared visualizer uniform block (unchanged across all shaders).
layout(std140, binding = 0) uniform buf {
    float time;       // animation clock (seconds)
    float amplitude;  // overall audio loudness 0..1
    vec2 resolution;  // viewport pixel size (x=width, y=height)
};

// binding=1 is the frequency spectrum: 64 log-spaced bands in [0..1], packed as
// vec4[16] (std140). Shaders that don't declare this block don't get the data —
// it's purely additive.
layout(std140, binding = 1) uniform spec {
    vec4 bands[16];
};

void main() {
    // v_uv runs 0..1 across the fullscreen triangle's visible area; v_uv.y is
    // already 0 at the bottom in this RHI target, so use it directly (bars grow
    // upward).
    vec2 uv = v_uv;

    float fx = uv.x * 64.0;
    int bi = int(floor(fx));
    float frac = fract(fx);

    // Pull this column's band height. Address bands[] only with loop induction
    // variables (constant-index expressions) and avoid integer bitwise ops, so
    // this compiles on the GLSL ES path too — dynamic uniform indexing and
    // >>/& are not portable there.
    float h = 0.0;
    for (int g = 0; g < 16; ++g) {
        for (int c = 0; c < 4; ++c) {
            if (g * 4 + c == bi)
                h = bands[g][c];
        }
    }

    // Thin gap between columns reads them as discrete bars.
    float gap = smoothstep(0.0, 0.06, frac) * smoothstep(0.0, 0.06, 1.0 - frac);
    float bar = step(uv.y, h) * gap;

    // Colour ramp across the spectrum (low=warm, high=cool), brightened toward
    // the top of each bar and pulsed slightly by overall amplitude.
    vec3 lo = vec3(1.0, 0.25, 0.35);
    vec3 hi = vec3(0.25, 0.6, 1.0);
    vec3 col = mix(lo, hi, uv.x);
    col *= 0.55 + 0.45 * (uv.y / max(h, 1e-3));   // gradient up the bar
    col *= 0.85 + 0.15 * amplitude;

    // Soft glow beneath the bars for a little depth.
    float glow = h * smoothstep(0.0, 0.35, h - uv.y) * 0.15 * gap;

    fragColor = vec4(col * bar + mix(lo, hi, uv.x) * glow, 1.0);
}
