#pragma once

// ConvertToScene - public API of CogniteCiffLib.
//
// Reads a Cognite CIFF file from disk and fills a renderer-agnostic
// scene::SceneData. Intended to be linked into 3DViewer (and any other
// in-process consumer) via CogniteCiffLib.lib.
//
// IMPORTANT: scene::SceneData is intentionally forward-declared here so
// that 3DViewer can keep its own copy of SceneData.h without an ODR clash.
// The copies of SceneData.h must remain struct-identical.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

namespace scene { struct SceneData; }

namespace ciff
{
    // CPU wall-clock timings for the eager CIFF -> SceneData producer. The
    // three detailed SceneData stages are non-overlapping; projection and total
    // are rollups. These values remain separate from Viewer-wide parse/upload.
    struct ConvertToSceneTimings
    {
        // CIFF parsing and source-table loading.
        double dataLoadMs = 0.0;
        // Geometry tessellation followed by render-normal finalization.
        double geometryFinalizeMs = 0.0;
        // Mesh hash/bounds/range creation and descriptor-table commit.
        double meshDescriptorMs = 0.0;
        // Remaining SceneData work: material setup, source traversal,
        // instance/hierarchy projection, validation, and final handoff.
        double instanceHierarchyMaterialMs = 0.0;
        // Complete post-load CIFF -> SceneData projection.
        double sceneProjectionMs = 0.0;
        // Complete ConvertToScene call, including dataLoadMs.
        double totalMs = 0.0;
    };

    struct ConvertToSceneResult
    {
        bool         success = false;
        std::wstring message;
        ConvertToSceneTimings timings;
    };

    using ConvertProgressCallback = std::function<bool(std::size_t total, std::size_t current)>;

    ConvertToSceneResult ConvertToScene(
        const std::filesystem::path&    ciffPath,
        scene::SceneData&               out,
        const ConvertProgressCallback&  progressCallback = {});

}
