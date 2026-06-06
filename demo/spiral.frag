#version 440

// Aggressive swirling spiral with sparks shooting off the arms.
// Drop in over plasma.frag (same `time` uniform at binding 0), or point
// qt_add_shaders at this file. Compile with qsb at build time.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    float time;
} ubuf;

// cheap hash for spark jitter
float hash(float n) { return fract(sin(n) * 43758.5453123); }

void main()
{
    float t = ubuf.time;

    // centre the coords, aspect-ish (assumes squarish surface)
    vec2 p = v_uv * 2.0 - 1.0;
    float r = length(p);
    float a = atan(p.y, p.x);

    // --- the spiral core ---
    // logarithmic spiral arms that rotate and tighten over time
    float arms = 5.0;
    float spiral = sin(arms * a + r * 14.0 - t * 6.0);
    // sharpen into bright filaments
    float core = smoothstep(0.55, 1.0, spiral) / (r * 2.2 + 0.15);

    // pulsing hot centre
    float center = 0.6 / (r * r * 8.0 + 0.04);
    center *= 0.7 + 0.3 * sin(t * 9.0);

    // --- sparks shooting outward along the arms ---
    float sparks = 0.0;
    for (int i = 0; i < 14; ++i) {
        float fi = float(i);
        // each spark rides out along a fixed angle, looping in radius
        float ang = (fi / 14.0) * 6.28318 + t * 0.7;
        float speed = 0.6 + hash(fi) * 1.6;
        float rad = fract(hash(fi * 3.1) + t * speed);          // 0..1 travel
        vec2 sp = vec2(cos(ang), sin(ang)) * rad;
        float d = length(p - sp);
        float life = 1.0 - rad;                                  // fade as it flies out
        sparks += life * 0.012 / (d * d + 0.0008);
    }

    // --- colour ---
    // arms tinted by angle + time, hot white core, electric-blue sparks
    vec3 armCol = 0.5 + 0.5 * cos(vec3(0.0, 2.1, 4.2) + a * 1.5 + t * 1.3 + r * 3.0);
    vec3 col = armCol * core * 1.6;
    col += vec3(1.0, 0.85, 0.6) * center;          // warm core
    col += vec3(0.4, 0.7, 1.0) * sparks;           // cool sparks

    // subtle vignette + gamma
    col *= smoothstep(1.4, 0.2, r);
    col = pow(max(col, 0.0), vec3(0.85));

    fragColor = vec4(col, 1.0);
}
