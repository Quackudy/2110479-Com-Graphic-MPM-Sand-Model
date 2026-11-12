#include "scene_cache.hpp"
#include <cmath>
#include <cstring>
#include <stdexcept>

SceneCacheWriter::SceneCacheWriter(const std::filesystem::path &path, uint32_t particles,
                                   uint32_t frames, uint32_t fps, const Json &metadata)
    : path(path), temporary(path.string() + ".tmp") {
    if (!particles || particles > 511902 || !frames || frames > 72001 || !fps || fps > 240)
        throw std::runtime_error("invalid cache dimensions");
    header.particles = particles;
    header.frames = frames;
    header.fps = fps;
    const std::string json = metadata.dump();
    if (json.size() > 1024 * 1024)
        throw std::runtime_error("cache metadata too large");
    header.metadataBytes = static_cast<uint32_t>(json.size());
    std::filesystem::create_directories(path.parent_path());
    output.open(temporary, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(reinterpret_cast<const char *>(&header), sizeof(header));
    output.write(json.data(), json.size());
}
SceneCacheWriter::~SceneCacheWriter() {
    if (!committed) {
        output.exceptions(std::ios::goodbit);
        output.close();
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
    }
}
void SceneCacheWriter::append(const std::vector<Particle> &particles) {
    if (particles.size() != header.particles || written >= header.frames)
        throw std::runtime_error("cache frame dimensions differ");
    std::vector<float> xyz;
    xyz.reserve(particles.size() * 3);
    for (const auto &p : particles)
        for (int a = 0; a < 3; ++a) {
            if (!std::isfinite(p.position[a]))
                throw std::runtime_error("non-finite cached position");
            xyz.push_back(p.position[a]);
        }
    output.write(reinterpret_cast<const char *>(xyz.data()), xyz.size() * sizeof(float));
    ++written;
}
void SceneCacheWriter::commit() {
    if (written != header.frames)
        throw std::runtime_error("cannot publish incomplete scene cache");
    output.flush();
    output.close();
    std::filesystem::rename(temporary,
                            path); // Same-directory rename keeps the previous scene until success.
    committed = true;
}
SceneCacheReader::SceneCacheReader(const std::filesystem::path &path)
    : input(path, std::ios::binary) {
    if (!input)
        throw std::runtime_error("no saved scene; run calculate first: " + path.string());
    input.read(reinterpret_cast<char *>(&header), sizeof(header));
    const CacheHeader expected;
    if (!input || std::memcmp(header.magic, expected.magic, 8) || header.version != 1 ||
        header.reserved || !header.particles || header.particles > 511902 || !header.frames ||
        header.frames > 72001 || !header.fps || header.fps > 240 ||
        header.metadataBytes > 1024 * 1024)
        throw std::runtime_error("invalid or unsupported scene cache");
    offset = sizeof(header) + header.metadataBytes;
    const uint64_t size = offset + uint64_t(header.particles) * header.frames * 3 * sizeof(float);
    input.seekg(0, std::ios::end);
    if (input.tellg() != static_cast<std::streamoff>(size))
        throw std::runtime_error("truncated or oversized scene cache");
    input.seekg(sizeof(header));
    std::string json(header.metadataBytes, '\0');
    if (!input.read(json.data(), json.size()))
        throw std::runtime_error("cannot read scene metadata");
    metadata = Json::parse(json);
}
std::vector<float> SceneCacheReader::frame(uint32_t index) {
    if (index >= header.frames)
        throw std::runtime_error("playback frame out of range");
    std::vector<float> result(size_t(header.particles) * 3);
    input.seekg(
        static_cast<std::streamoff>(offset + uint64_t(index) * result.size() * sizeof(float)));
    if (!input.read(reinterpret_cast<char *>(result.data()), result.size() * sizeof(float)))
        throw std::runtime_error("cannot read cached frame");
    for (float v : result)
        if (!std::isfinite(v))
            throw std::runtime_error("cached frame contains non-finite positions");
    return result;
}

