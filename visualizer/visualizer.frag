#version 440
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform UniformBlock {
    mat4 matrix;
    float amplitude; // Real-time volume from C++
} ubuf;

void main() {
    vec2 center = vec2(0.5, 0.5);
    float dist = distance(v_uv, center);
    
    // The circle base size is 0.1, expanding dynamically up to 0.5 based on audio beat
    float radius = 0.1 + (ubuf.amplitude * 0.4); 
    
    vec3 color = vec3(0.1); // Dark background
    if (dist < radius) {
        color = vec3(0.4, 0.8, 0.3); // Cyan beat circle
    }
    
    fragColor = vec4(color, 1.0);
}