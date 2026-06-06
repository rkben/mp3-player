#version 440
layout(location = 0) in vec2 position;
layout(location = 0) out vec2 v_uv;

void main() {
    // Map oversized triangle coordinates (-1 to 3) into UV space (0 to 2)
    v_uv = position * 0.5 + 0.5; 
    gl_Position = vec4(position, 0.0, 1.0);
}