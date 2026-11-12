#version 450
layout(set=0,binding=0,std140) uniform Frame { mat4 view; mat4 projection; vec4 playback; } frame;
layout(location=0) in vec2 uv;
layout(location=1) flat in vec3 centerView;
layout(location=2) flat in vec3 color;
layout(location=3) flat in uint floorFlag;
layout(location=4) flat in float grainRadius;
layout(location=5) flat in vec3 grainAxis;
layout(location=0) out vec4 outColor;
void main() {
    vec3 lit=color;
    gl_FragDepth=gl_FragCoord.z;
    if (floorFlag==0) {
        float r2=dot(uv,uv);
        if (r2>1) discard;
        vec3 normal=vec3(uv,sqrt(1-r2));
        vec4 clip=frame.projection*vec4(centerView+grainRadius*normal,1);
        gl_FragDepth=clip.z/clip.w;
        vec3 worldNormal=transpose(mat3(frame.view))*normal;
        // Filter subpixel detail instead of letting mineral facets shimmer in motion.
        float footprint=max(length(dFdx(worldNormal)),length(dFdy(worldNormal)));
        float detail=1.0-smoothstep(0.15,0.65,footprint);
        vec3 facets=normalize(round((worldNormal+0.12*grainAxis)*5.0));
        worldNormal=normalize(mix(worldNormal,facets,0.65*detail));
        vec3 light=normalize(vec3(-0.4,0.9,0.7));
        float diffuse=max(dot(worldNormal,light),0);
        float sky=0.5+0.5*worldNormal.y;
        lit*=0.12+0.24*sky+0.70*diffuse;
        float mineralDetail=sin(18.0*dot(worldNormal,grainAxis))*sin(23.0*dot(worldNormal,grainAxis.yzx));
        lit*=1.0+0.10*detail*mineralDetail;
    } else {
        float lines=min(abs(fract((uv.x+1)*8)-0.5),abs(fract((uv.y+1)*8)-0.5));
        lit*=lines<0.015 ? 0.8 : 1.0;
    }
    outColor=vec4(pow(lit,vec3(1.0/2.2)),1);
}
