const float TWO_PI = 6.283185307179586;
void interpolation(vec3 position, uint grid, out ivec3 base, out mat3 weights) {
    vec3 xp = position / params.step.y;
    if (params.dims.z == 0) {
        float shift = grid == 0 ? -0.25 : 0.25;
        base = ivec3(floor(xp - shift));
        vec3 d = xp - (vec3(base) + shift);
        vec3 w = vec3(1) - d + sin(TWO_PI*d)/TWO_PI;
        weights = mat3(w, vec3(1)-w, vec3(0));
    } else {
        base = ivec3(floor(xp - 0.5));
        vec3 d = xp - vec3(base);
        weights = mat3(0.5*(1.5-d)*(1.5-d), 0.75-(d-1)*(d-1), 0.5*(d-0.5)*(d-0.5));
    }
}
float kernelWeight(ivec3 o, mat3 w) {
    return w[o.x].x * w[o.y].y * w[o.z].z;
}

// Offsets in cell units keep the moment inverse well scaled on fine grids.
vec3 nodeOffset(vec3 position, ivec3 cell, uint grid) {
    float shift = params.dims.z == 0 ? (grid == 0 ? -0.25 : 0.25) : 0.0;
    return vec3(cell) + shift - position / params.step.y;
}
mat3 inverseMoment(vec3 position) {
    uint grids = params.dims.z == 0 ? 2 : 1;
    int radius = params.dims.z == 0 ? 2 : 3;
    mat3 D = mat3(0);
    for (uint g=0; g<grids; ++g) {
        ivec3 base; mat3 w;
        interpolation(position,g,base,w);
        for (int z=0;z<radius;++z) for (int y=0;y<radius;++y) for (int x=0;x<radius;++x) {
            ivec3 o=ivec3(x,y,z), cell=base+o;
            if (!validNode(cell)) continue;
            float weight=kernelWeight(o,w);
            vec3 r=nodeOffset(position,cell,g);
            D+=outerProduct(r,weight*r);
        }
    }
    return inverse(D/float(grids))/(params.step.y*params.step.y);
}
