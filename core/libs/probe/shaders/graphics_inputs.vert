#version 450

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 color;
layout(location = 3) in uvec4 bones;

void main() {
    gl_Position = position + color * float(bones.x);
    gl_PointSize = 1.0;
}
