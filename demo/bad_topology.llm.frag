#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    float time;
    float amplitude;
    float pad1;
    float pad2;
};

float map(vec3 p) {
    // KNOB: Base Floor Height.
    float y = p.y + 1.5; 

    // Combine two rolling sine waves to create the landscape topology
    float wave1 = sin(p.x * 1.2 + time * 0.8) * cos(p.z * 1.2 - time * 0.5);
    float wave2 = sin(p.x * 2.5 - time * 1.2) * cos(p.z * 2.5 + time * 1.1);

    // Combine the waves
    float landscape = (wave1 * 0.5) + (wave2 * 0.25);

    // KNOB: Topographical Audio Influence.
    // When amplitude is 0, the height multiplier is 0.2 (almost flat).
    // When amplitude spikes, it multiplies the wave heights by up to 3.0+, 
    // lifting massive smooth hills out of the floor.
    float heightMultiplier = 0.2 + (amplitude * 3.5);

    // Subtract the landscape from our flat floor 'y'
    y -= landscape * heightMultiplier;

    // Safe step distance since we are warping the Y axis heavily
    return y * 0.6;
}

vec3 calcNormal( in vec3 p ) {
    const float h = 1e-3;
    const vec2 k = vec2(1.0, -1.0);
    return normalize( k.xyy*map( p + k.xyy*h ) +
                      k.yyx*map( p + k.yyx*h ) +
                      k.yxy*map( p + k.yxy*h ) +
                      k.xxx*map( p + k.xxx*h ) );
}

void main() {
    vec2 uv = v_uv;
    float aspect = 1.3333;

    // KNOB: Camera setup.
    // The camera is slowly gliding forward along the Z axis over the landscape.
    vec3 rayOri = vec3(0.0, 0.5, time * 1.5);
    
    // Point the camera forward, but tilted slightly downward (-0.2 on the Y axis)
    vec3 dir = vec3((uv - 0.5) * vec2(aspect, 1.0), 1.0);
    dir.y -= 0.2; 
    vec3 rayDir = normalize(dir);

    float depth = 0.0;
    vec3 p;
    bool hit = false;

    // Raymarch over the terrain
    for(int i = 0; i < 90; i++) {
        p = rayOri + rayDir * depth;
        float dist = map(p);
        depth += dist;
        if (dist < 0.01) { hit = true; break; }
    }

    // KNOB: Background/Sky Color
    vec3 bgCol = vec3(0.05, 0.02, 0.1); 
    vec3 col = bgCol;

    if (hit) {
        vec3 n = calcNormal(p);

        // Gentle light coming from above
        float b = max(0.0, dot(n, normalize(vec3(0.0, 1.0, 0.5))));

        // KNOB: Elevation Coloring.
        // We use the absolute world height (p.y) to color the terrain.
        // The valleys (low p.y) are dark blue. The peaks (high p.y) are bright pink/cyan.
        // Because the amplitude physically lifts the peaks, the loud parts 
        // organically change color as they rise.
        vec3 valleyColor = vec3(0.0, 0.2, 0.6);
        vec3 peakColor   = vec3(1.0, 0.3, 0.8);
        
        // Smoothly mix between valley and peak based on height
        float heightFactor = smoothstep(-1.5, 1.0, p.y);
        vec3 terrainColor = mix(valleyColor, peakColor, heightFactor);

        // Apply light and shadow
        col = terrainColor * (b * 0.8 + 0.2);

        // KNOB: Fog. Blends the distant landscape smoothly into the sky.
        float fogFactor = clamp(depth / 15.0, 0.0, 1.0);
        
        // Square the fog factor so it stays clearer up close, and falls off sharply in the distance.
        col = mix(col, bgCol, fogFactor * fogFactor);
    }

    fragColor = vec4(col, 1.0);
}