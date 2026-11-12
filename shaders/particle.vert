#version 450
layout(constant_id=0) const uint particleStride=3;
layout(set=0,binding=0,std140) uniform Frame {
    mat4 view;
    mat4 projection;
    vec4 playback; // alpha, radius, particle count, floor height
} frame;
layout(set=0,binding=1,std430) readonly buffer Current { float xyz[]; } currentFrame;
layout(set=0,binding=2,std430) readonly buffer Next { float xyz[]; } nextFrame;
layout(location=0) out vec2 uv;
layout(location=1) flat out vec3 centerView;
layout(location=2) flat out vec3 color;
layout(location=3) flat out uint floorFlag;
layout(location=4) flat out float grainRadius;
layout(location=5) flat out vec3 grainAxis;

uint grainHash(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
float grainRandom(uint id, uint salt) {
    return float(grainHash(id ^ salt) >> 8) / 16777216.0;
}
const vec2 corners[6]=vec2[](vec2(-1,-1),vec2(1,-1),vec2(1,1),vec2(-1,-1),vec2(1,1),vec2(-1,1));
void main() {
    uv=corners[gl_VertexIndex];
    floorFlag=gl_InstanceIndex==0 ? 1 : 0;
    grainRadius=frame.playback.y;
    grainAxis=vec3(0,1,0);
    if (floorFlag==1) {
        vec3 world=vec3(0.5+0.5*uv.x,frame.playback.w,0.5+0.5*uv.y);
        gl_Position=frame.projection*frame.view*vec4(world,1);
        centerView=vec3(0);color=vec3(0.19,0.22,0.25);
    } else {
        uint p=gl_InstanceIndex-1;
        uint i=particleStride*p;
        vec3 a=vec3(currentFrame.xyz[i],currentFrame.xyz[i+1],currentFrame.xyz[i+2]);
        vec3 b=a;
        if (particleStride==3)
            b=vec3(nextFrame.xyz[i],nextFrame.xyz[i+1],nextFrame.xyz[i+2]);
        centerView=(frame.view*vec4(mix(a,b,frame.playback.x),1)).xyz;
        // Fixed particle IDs keep color, size, and mineral orientation stable during replay.
        grainRadius*=mix(0.72,1.22,grainRandom(p,0x1234567u));
        grainAxis=normalize(vec3(grainRandom(p,0x9876543u),grainRandom(p,0xabcdefu),grainRandom(p,0x2468aceu))-0.5);
        gl_Position=frame.projection*vec4(centerView+vec3(uv*grainRadius,0),1);
        color=p<uint(frame.playback.z)/2 ? vec3(0.47,0.30,0.13) : vec3(0.66,0.51,0.30);
        color*=mix(0.65,1.18,grainRandom(p,0xfedcba9u));
        float mineral=grainRandom(p,0x13579bdu);
        if(mineral<0.06) color=mix(color,vec3(0.16,0.12,0.075),0.55);
        if(mineral>0.93) color=mix(color,vec3(0.79,0.72,0.54),0.45);
    }
}
