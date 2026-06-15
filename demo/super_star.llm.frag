#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    float time;
    float amplitude;
    vec2 resolution;  // viewport pixel size (x=width, y=height)
};

void main() {
    // Shift UV so (0.0, 0.0) is the exact center of the screen
    vec2 uv = v_uv - 0.5;
    
    // KNOB: Aspect ratio correction. Prevents circles from stretching into ovals.
    // Adjust this to match your actual player window (e.g., 1.77 for 16:9).
    uv.x *= 1.333; 

    // Get polar coordinates (distance from center, and angle around center)
    float radius = length(uv);
    float angle = atan(uv.y, uv.x);

    // KNOB: Wobble amount. We distort the radius based on the angle and the music.
    // "angle * 8.0" means the mandala has 8 points.
    float wobble = sin(angle * 8.0 + time * 2.0) * (amplitude * 0.15);
    float warpedRadius = radius + wobble;

    // KNOB: Ring spacing and animation speed.
    // Multiply by 30.0 for ring density, subtract time to make them flow outward.
    float rings = sin(warpedRadius * 30.0 - time * 3.0);

    // KNOB: Ring thickness. smoothstep shapes the sine wave into sharp bands.
    // 0.85 to 1.0 leaves only the very peaks of the sine wave visible.
    float ringMask = smoothstep(0.85, 1.0, rings);

    // KNOB: Color palette. Cycles smoothly through colors based on distance and time.
    // The vec3(0, 1, 2) offsets the Red, Green, and Blue channels.
    vec3 baseColor = 0.5 + 0.5 * cos(time * 0.8 + radius * 5.0 + vec3(0.0, 1.0, 2.0));

    // Apply the mask to the colors
    vec3 col = baseColor * ringMask;

    // KNOB: Center Bass Orb. A solid glow in the middle that flares up violently.
    float centerOrb = smoothstep(0.2 + (amplitude * 0.3), 0.0, radius);
    
    // KNOB: Center Orb Color. Currently set to a warm orange/red flare.
    col += vec3(1.0, 0.3, 0.0) * centerOrb * (0.2 + amplitude * 1.5);

    fragColor = vec4(col, 1.0);
}