#version 460

layout(set = 0, binding = 0, r32f) uniform readonly image2D source;

void main() {
    float value = imageLoad(source, ivec2(gl_FragCoord.xy)).r;
    gl_FragDepth = value >= 0.0 ? min(value, 1.0) : 0.0;
}
