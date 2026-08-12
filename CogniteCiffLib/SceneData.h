#pragma once

// SceneData.h
//
// Shared scene contract between every Convert*-pipeline (FBX/RVM/CIFF/OBJ/NWD/.3d)
// and the 3DViewer. Header-only. No DirectX, no Windows, no I/O. Pure data.
//
// Designed so that vertex/index arrays can be read directly from disk
// (one ReadExact per array) and uploaded directly to GPU buffers (memcpy).
//
// NOTE: This contract is intentionally duplicated across:
//         3DViewer/Scene/SceneData.h
//         CADViewer/Scene/SceneData.h
//         AutodeskFBX/AutodeskFBXLib/SceneData.h
//         AvevaRvmDebug/AvevaRvmLib/SceneData.h
//         CogniteCIFF/CogniteCiffLib/SceneData.h
//         Falcon3D/Falcon3DLib/SceneData.h
//         FalconOBJ/FalconOBJLib/SceneData.h
//         NavisworksNWD/NavisworksNWDLib/SceneData.h
//       These copies define the v1 contract and MUST keep identical field order and semantics.

#include <cstdint>
#include <string>
#include <vector>

namespace scene
{
    enum class UpAxis : std::uint8_t { Y = 1, Z = 2 };

    enum class FrontAxis : std::uint8_t { X = 0, Y = 1, Z = 2, NegX = 3, NegY = 4, NegZ = 5 };

    constexpr std::uint32_t kInvalidIndex = UINT32_MAX;

    // A unique mesh form in local space. Many instances can share one mesh.
    // Flat float arrays - same layout as .3d on disk and as GPU vertex buffers.
    struct Mesh
    {
        std::vector<float>         positions;   // 3 * pointCount (x,y,z,...)
        std::vector<float>         normals;     // 3 * pointCount; one finite unit normal per point (empty iff pointCount == 0)
        std::vector<std::uint32_t> indices;     // 3 * triangleCount
    };

    // One placement of a mesh in world space.
    struct Instance
    {
        std::uint32_t meshIndex     = kInvalidIndex;
        std::uint32_t materialIndex = kInvalidIndex;
        std::uint32_t nodeIndex     = kInvalidIndex;
        float         transform[12] = { 1, 0, 0,  0, 1, 0,  0, 0, 1,  0, 0, 0 }; // column-major 3x4
    };

    // RGBA color (0..255) + optional texture reference.
    struct Material
    {
        std::uint8_t  r = 255;
        std::uint8_t  g = 255;
        std::uint8_t  b = 255;
        std::uint8_t  a = 255;
        std::uint32_t textureIndex = kInvalidIndex; // kInvalidIndex = untextured
    };

    // Compressed image data carried from source file.
    struct Texture
    {
        std::string               name;     // identifier from source
        std::string               format;   // "png", "jpg", ...
        std::vector<std::uint8_t> data;     // raw compressed bytes
    };

    // Hierarchy node. Children are derived from parent indices at load time.
    struct Node
    {
        std::int32_t  parent        = -1;     // -1 = root
        std::uint32_t firstInstance = 0;
        std::uint32_t instanceCount = 0;
        std::string   name;                   // UTF-8 / ANSI
    };

    // Top-level scene container.
    struct SceneData
    {
        UpAxis    upAxis             = UpAxis::Z;
        FrontAxis frontAxis          = FrontAxis::Y;
        bool      mirrorXAxisInWorld = false;

        std::vector<Mesh>     meshes;
        std::vector<Instance> instances;
        std::vector<Material> materials;
        std::vector<Texture>  textures;     // empty = no textures in scene
        std::vector<Node>     nodes;
        std::uint32_t         rootNode = 0;
    };

} // namespace scene
