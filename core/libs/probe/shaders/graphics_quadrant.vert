#version 450

const vec2 corners[6] = vec2[](vec2(-1.0, 1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0));

void main() {
    gl_Position = vec4(corners[gl_VertexIndex % 6], 0.0, 1.0);
    gl_PointSize = 1.0;
}
