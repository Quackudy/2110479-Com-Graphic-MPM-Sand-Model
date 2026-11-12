// One-sided Jacobi SVD: F = U * diag(S) * transpose(V).
void svd3(mat3 F, out mat3 U, out vec3 S, out mat3 V) {
    mat3 B = F;
    V = mat3(1);
    for (int sweep=0; sweep<10; ++sweep) {
        for (int pair=0; pair<3; ++pair) {
            int p = pair == 2 ? 1 : 0;
            int q = pair == 0 ? 1 : 2;
            float a=dot(B[p],B[p]), b=dot(B[q],B[q]), g=dot(B[p],B[q]);
            if (abs(g) <= 1e-7*sqrt(a*b)) continue;
            float z=(b-a)/(2*g);
            float t=(z >= 0 ? 1.0 : -1.0)/(abs(z)+sqrt(1+z*z));
            float c=inversesqrt(1+t*t), s=t*c;
            vec3 bp=B[p], vp=V[p];
            B[p]=c*bp-s*B[q]; B[q]=s*bp+c*B[q];
            V[p]=c*vp-s*V[q]; V[q]=s*vp+c*V[q];
        }
    }
    S=vec3(length(B[0]), length(B[1]), length(B[2]));
    U=mat3(B[0]/max(S.x,1e-12), B[1]/max(S.y,1e-12), B[2]/max(S.z,1e-12));
    // Complete the basis for collapsed directions; normal sand states are full rank.
    if (S.x<1e-12) U[0]=normalize(cross(B[1],B[2])+vec3(1e-20,0,0));
    if (S.y<1e-12) U[1]=normalize(cross(U[2],U[0])+vec3(0,1e-20,0));
    if (S.z<1e-12) U[2]=normalize(cross(U[0],U[1])+vec3(0,0,1e-20));
}

// Drucker-Prager / StVK-Hencky, matching CK-MPM/include/mpm_material.cuh.
// Keep the original logJp volume correction, zero cohesion, and beta=1.
void sandMaterial(inout mat3 F, inout float logJp, out mat3 PF) {
    mat3 U,V; vec3 S;
    svd3(F,U,S,V);
    vec3 epsilon=log(max(abs(S),vec3(1e-4)));
    float sumE=epsilon.x+epsilon.y+epsilon.z;
    float traceE=sumE+logJp;
    vec3 dev=epsilon-vec3(traceE/3);
    float normDev=length(dev);
    vec3 newS;
    if (traceE>=0) {
        newS=vec3(1);
        logJp+=sumE;
    } else {
        logJp=0;
        float ys=0.816496580927726*2*params.sand.x/(3-params.sand.x);
        float dg=normDev+(3*params.material.y+2*params.material.x)/(2*params.material.x)*traceE*ys;
        vec3 H=epsilon;
        if (dg>0) H-=dg/max(normDev,1e-20)*dev;
        newS=exp(H);
    }
    F=U*mat3(newS.x,0,0, 0,newS.y,0, 0,0,newS.z)*transpose(V);
    vec3 logs=log(newS);
    vec3 pHat=(2*params.material.x*logs+params.material.y*(logs.x+logs.y+logs.z))/newS;
    PF=U*mat3(pHat.x,0,0, 0,pHat.y,0, 0,0,pHat.z)*transpose(V)*transpose(F);
}
