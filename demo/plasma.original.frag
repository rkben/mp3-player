#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    float time;
} ubuf;

// A cheap animated plasma — stand-in for "fun shader shenanigans" behind the
// album-art surface. All maths, no textures, so it shows the pipeline plumbing.
void main()
{
    vec2 p = v_uv * 6.0;
    float t = ubuf.time;
    float v = sin(p.x + t)
            + sin(p.y + t * 1.3)
            + sin((p.x + p.y) * 0.7 + t * 0.7)
            + sin(length(p - 3.0) - t * 1.5);
    vec3 col = 0.5 + 0.5 * cos(vec3(0.0, 2.0, 4.0) + v + t);
    fragColor = vec4(col, 1.0);
}
