// Calls the author's unmodified material function and SVD as a GPU reference.
// The author's material header needs variant and svd declared before inclusion.
// clang-format off
#include <variant>
#include "mpm_math.cuh"
#include "mpm_material.cuh"
// clang-format on
#include <fstream>
#include <iostream>
#include <vector>

__global__ void evaluate(float *values, unsigned count, float mu, float lambda) {
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count)
        return;
    float *p = values + 48 * i;
    mpm::Matrix<float, 3, 3> F, PF;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            F[3 * r + c] = p[8 + 4 * c + r];
    float logJp = p[32];
    mpm::ComputeStress<mpm::MPMConstitutiveModel::kDruckerPragerStvkhencky>(1.0f, mu, lambda, F,
                                                                            logJp, PF);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            p[8 + 4 * c + r] = F[3 * r + c];
            p[20 + 4 * c + r] = PF[3 * r + c];
        }
    p[32] = logJp;
}
void check(cudaError_t status) {
    if (status != cudaSuccess)
        throw std::runtime_error(cudaGetErrorString(status));
}
int main(int argc, char **argv) {
    try {
        if (argc != 5)
            throw std::runtime_error(
                "usage: cuda_material_reference input.bin output.bin mu lambda");
        std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
        if (!input)
            throw std::runtime_error("cannot read reference input");
        auto bytes = input.tellg();
        if (bytes <= 0 || bytes % (48 * sizeof(float)))
            throw std::runtime_error("invalid reference input size");
        std::vector<float> values(static_cast<size_t>(bytes) / sizeof(float));
        input.seekg(0);
        if (!input.read(reinterpret_cast<char *>(values.data()), bytes))
            throw std::runtime_error("reference input truncated");
        float *device = nullptr;
        check(cudaMalloc(&device, static_cast<size_t>(bytes)));
        check(
            cudaMemcpy(device, values.data(), static_cast<size_t>(bytes), cudaMemcpyHostToDevice));
        unsigned count = values.size() / 48;
        evaluate<<<(count + 127) / 128, 128>>>(device, count, std::stof(argv[3]),
                                               std::stof(argv[4]));
        check(cudaGetLastError());
        check(cudaDeviceSynchronize());
        check(
            cudaMemcpy(values.data(), device, static_cast<size_t>(bytes), cudaMemcpyDeviceToHost));
        check(cudaFree(device));
        std::ofstream out(argv[2], std::ios::binary);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out.write(reinterpret_cast<const char *>(values.data()), bytes);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
