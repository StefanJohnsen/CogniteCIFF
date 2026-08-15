#pragma once

// SceneData.h
//
// Shared v1 scene contract. Keep the field layout, order and semantics identical
// to the active pilot copies in 3DViewer and NavisworksNWDLib.

#include <cstdint>
#include <string>
#include <vector>

namespace scene
{
    enum class UpAxis : std::uint8_t
    {
        Y = 1,
        Z = 2
    };

    enum class FrontAxis : std::uint8_t
    {
        X = 0,
        Y = 1,
        Z = 2,
        NegX = 3,
        NegY = 4,
        NegZ = 5
    };

    constexpr std::uint32_t kInvalidIndex = UINT32_MAX;

    // One GPU-sized planar geometry slab. A .3d file stores these arrays as
    // independently aligned blocks so they can be read and uploaded without
    // interleaving or concatenating per-mesh arrays first.
    struct GeometryChunk
    {
        std::vector<float> positions;       // 3 * vertexCount (x,y,z,...)
        std::vector<float> normals;         // 3 * vertexCount; finite unit normals
        std::vector<std::uint32_t> indices; // local-to-form triangle indices
    };

    // A unique mesh form in local space. Ranges use element offsets, never byte
    // offsets. Index values are local to this form and are combined with
    // baseVertex by DrawIndexedInstanced.
    struct Mesh
    {
        std::uint64_t shapeHash = 0;
        std::uint32_t chunkIndex = kInvalidIndex;
        std::uint32_t baseVertex = 0;
        std::uint32_t vertexCount = 0;
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        float boundsMin[3]{};
        float boundsMax[3]{};
    };

    // One placement of a mesh in world space.
    struct Instance
    {
        std::uint32_t meshIndex = kInvalidIndex;
        std::uint32_t materialIndex = kInvalidIndex;
        std::uint32_t nodeIndex = kInvalidIndex;
        float transform[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}; // column-major 3x4
    };

    // RGBA color (0..255) + optional texture reference.
    struct Material
    {
        std::uint8_t r = 255;
        std::uint8_t g = 255;
        std::uint8_t b = 255;
        std::uint8_t a = 255;
        std::uint32_t textureIndex = kInvalidIndex; // kInvalidIndex = untextured
    };

    // Compressed image data carried from source file.
    struct Texture
    {
        std::string name;               // identifier from source
        std::string format;             // "png", "jpg", ...
        std::vector<std::uint8_t> data; // raw compressed bytes
    };

    // Hierarchy node. Children are derived from parent indices at load time.
    struct Node
    {
        std::int32_t parent = -1; // -1 = root
        std::uint32_t firstInstance = 0;
        std::uint32_t instanceCount = 0;
        std::string name; // UTF-8 / ANSI
    };

    // Top-level scene container.
    struct SceneData
    {
        UpAxis upAxis = UpAxis::Z;
        FrontAxis frontAxis = FrontAxis::Y;
        bool mirrorXAxisInWorld = false;
        bool geometryChunksGpuReady = false;

        std::vector<GeometryChunk> geometryChunks;
        std::vector<Mesh> meshes;
        std::vector<Instance> instances;
        std::vector<Material> materials;
        std::vector<Texture> textures; // empty = no textures in scene
        std::vector<Node> nodes;
        std::uint32_t rootNode = 0;
    };

} // namespace scene
