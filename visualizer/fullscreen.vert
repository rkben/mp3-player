#version 440
layout(location = 0) in vec4 position;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec2 v_uv;

layout(std140, binding = 0) uniform UniformBlock {
    mat4 matrix;
    float amplitude; 
} ubuf;

void main() {
    v_uv = uv;
    gl_Position = ubuf.matrix * position;
}