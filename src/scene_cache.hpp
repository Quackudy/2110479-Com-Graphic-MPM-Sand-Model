#pragma once
#include "simulation_config.hpp"
#include <fstream>

// One versioned file contains metadata and fixed-ID XYZ frames. No physics state.
struct CacheHeader {
    char magic[8] = {'C', 'K', 'P', 'L', 'A', 'Y', '0', '1'};
    uint32_t version = 1, particles = 0, frames = 0, fps = 0, metadataBytes = 0, reserved = 0;
};
static_assert(sizeof(CacheHeader) == 32);
class SceneCacheWriter {
  public:
    SceneCacheWriter(const std::filesystem::path &path, uint32_t particles, uint32_t frames,
                     uint32_t fps, const Json &metadata);
    ~SceneCacheWriter();
    void append(const std::vector<Particle> &particles);
    void commit();

  private:
    std::filesystem::path path, temporary;
    std::ofstream output;
    CacheHeader header;
    uint32_t written = 0;
    bool committed = false;
};
class SceneCacheReader {
  public:
    explicit SceneCacheReader(const std::filesystem::path &path);
    std::vector<float> frame(uint32_t index);
    CacheHeader header;
    Json metadata;

  private:
    std::ifstream input;
    uint64_t offset = 0;
};
