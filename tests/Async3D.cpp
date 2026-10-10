#include "../Convert3D.h"
#include "../GeometryPipelineCIFF.h"
#include "Async3DFixture.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <iterator>
#include <unordered_set>

namespace
{
    using async3d_fixture::Bytes;
    using Pipeline = ciff::process::GeometryBuildPipeline;

    void Require(const bool condition, const std::string& message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    template<class Callback> void Rejects(Callback&& callback, const std::string& expected)
    {
        try { callback(); }
        catch (const std::exception& error)
        {
            Require(std::string(error.what()).find(expected) != std::string::npos,
                    "Unexpected worker error: " + std::string(error.what()));
            return;
        }
        throw std::runtime_error("Expected worker error: " + expected);
    }

    Bytes ReadBytes(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        Require(input.good(), "Missing output: " + path.string());
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    template<class T> T Value(const Bytes& bytes, const std::size_t offset)
    {
        Require(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset, "Truncated .3d field");
        T result{};
        std::memcpy(&result, bytes.data() + offset, sizeof(T));
        return result;
    }

    std::vector<std::size_t> WorkIndices(const ciff::Read& data)
    {
        std::vector<std::size_t> result;
        for (const auto& node : data.nodes)
            for (const auto index : node.geometries) result.push_back(index);
        return result;
    }

    Bytes MeshSnapshot(const ciff::Read& data)
    {
        Bytes result;
        for (const auto& mesh : data.meshes)
        {
            async3d_fixture::Append(result, mesh.color);
            async3d_fixture::Append(result, mesh.vertices.size());
            for (const auto point : mesh.vertices) async3d_fixture::Append(result, point);
            for (const auto index : mesh.indices) async3d_fixture::Append(result, index);
        }
        for (const auto color : data.colors) async3d_fixture::Append(result, color.packed());
        return result;
    }

    void AddRepeatedReference(ciff::Read& data)
    {
        // A library caller can share the same Geometry record across nodes.
        // Exercise occurrence ordering, even though CIFF wire records are inline.
        data.nodes.back().geometries.push_back(data.nodes[2].geometries.back());
        // A degenerate form never enters the writer cache. A later occurrence
        // must remain omitted without blocking the ordered hash gate.
        data.nodes.back().geometries.push_back(data.nodes[3].geometries.back());
    }

    void ValidateWorkerPipeline(const std::filesystem::path& source)
    {
        ciff::Read data(source.string(), "unused.3d"); data.load(); AddRepeatedReference(data);
        const auto indices = WorkIndices(data);
        Require(indices.size() == 15, "Fixture lost mixed or repeated geometry occurrences");
        const auto before = MeshSnapshot(data);
        Pipeline pipeline(data, {.workerCount = 3, .reservationBudgetBytes = 64 * 1024 * 1024, .maxOutstandingWork = 8});
        Require(pipeline.enabled() && pipeline.workerCount() == 3, "Forced worker pipeline was disabled");
        Require(pipeline.workCount() == indices.size(), "Pipeline lost geometry occurrences");
        std::unordered_set<std::uint64_t> forms;
        std::size_t normalBuilds = 0;
        for (const auto index : indices)
        {
            const auto prepared = pipeline.Claim(index);
            const auto& geometry = data.getGeometry(index);
            auto expectedForm = ciff::primitive_instance::Make(data, geometry);
            ciff::normal_processing::RenderGeometry expectedMesh;
            bool sourceEmpty = false;
            if (!expectedForm.hash || !forms.contains(expectedForm.hash))
            {
                auto local = ciff::primitive_instance::Tessellate(data, geometry, expectedForm);
                sourceEmpty = local.empty();
                if (!sourceEmpty && (!expectedForm.hash || !forms.contains(expectedForm.hash)))
                {
                    ++normalBuilds;
                    expectedMesh = ciff::normal_processing::FinalizeMeshNormals(local);
                    if (!expectedMesh.empty() && expectedForm.hash) forms.insert(expectedForm.hash);
                }
            }
            Require(prepared.form.hash == expectedForm.hash && prepared.form.transform == expectedForm.transform,
                    "Worker changed canonical hash or instance transform");
            Require(prepared.mesh.positions == expectedMesh.positions && prepared.mesh.normals == expectedMesh.normals &&
                    prepared.mesh.indices == expectedMesh.indices && prepared.sourceEmpty == sourceEmpty,
                    "Worker changed final normals or first-occurrence form ownership");
        }
        Require(forms.size() == 7, "Fixture did not share canonical native and baked mesh forms");
        Require(pipeline.claimedCount() == indices.size(), "Pipeline did not return every geometry occurrence");
        Require(pipeline.participatingWorkerCount() >= 2, "Independent geometry did not use multiple workers");
        Require(pipeline.normalBuildCount() == normalBuilds, "Async rebuilt normals for shared forms");
        Require(pipeline.peakOutstandingWork() <= 8 && pipeline.peakReservedBytes() <= 64 * 1024 * 1024,
                "Pipeline exceeded configured work or memory bounds");
        Require(MeshSnapshot(data) == before, "Workers modified the source meshes or palette");

        Pipeline bounded(data, {.workerCount = 3, .reservationBudgetBytes = 64 * 1024 * 1024, .maxOutstandingWork = 2});
        for (const auto index : indices) static_cast<void>(bounded.Claim(index));
        Require(bounded.peakOutstandingWork() <= 2, "Pipeline ignored outstanding work bound");
        Pipeline oversized(data, {.workerCount = 3, .reservationBudgetBytes = 1, .maxOutstandingWork = 2});
        for (const auto index : indices) static_cast<void>(oversized.Claim(index));
        Require(oversized.peakOutstandingWork() == 1, "Oversized geometry did not progress alone");
    }

    void ValidateBinary(const Bytes& bytes)
    {
        Require(Value<std::uint32_t>(bytes, 0) == 0x46443343 && Value<std::uint32_t>(bytes, 4) == 1,
                "Output is not Falcon3D v1");
        const auto forms = Value<std::uint64_t>(bytes, 16), instances = Value<std::uint64_t>(bytes, 24);
        const auto nodes = Value<std::uint64_t>(bytes, 32), materials = Value<std::uint64_t>(bytes, 40);
        const auto chunks = Value<std::uint64_t>(bytes, 48), chunkOffset = Value<std::uint64_t>(bytes, 56);
        const auto instanceOffset = Value<std::uint64_t>(bytes, 72), nodeOffset = Value<std::uint64_t>(bytes, 80);
        const auto materialOffset = Value<std::uint64_t>(bytes, 88);
        Require(forms == 7 && instances == 13 && nodes == 5 && materials == 4 && chunks >= 1,
                "Scene lost shared forms, material variants or hierarchy");
        Require(Value<std::uint64_t>(bytes, 104) == bytes.size(), "Incorrect Falcon3D file length");
        for (std::uint64_t i = 0; i < chunks; ++i)
        {
            const auto chunk = chunkOffset + i * 64, normalOffset = Value<std::uint64_t>(bytes, chunk + 16);
            const auto count = Value<std::uint32_t>(bytes, chunk + 48);
            for (std::uint32_t vertex = 0; vertex < count; ++vertex)
            {
                double norm = 0;
                for (unsigned axis = 0; axis < 3; ++axis)
                {
                    const auto value = Value<float>(bytes, normalOffset + vertex * 12 + axis * 4);
                    Require(std::isfinite(value), "Non-finite final normal"); norm += value * value;
                }
                Require(std::abs(norm - 1) < 1e-4, "Missing or non-unit final normal");
            }
        }
        for (const auto index : {6U, 7U})
        {
            Require(Value<std::uint32_t>(bytes, instanceOffset + index * 56) == Value<std::uint32_t>(bytes, instanceOffset),
                    "Translated/rotated boxes did not share their form");
            Require(Value<std::uint32_t>(bytes, instanceOffset + index * 56 + 4) != Value<std::uint32_t>(bytes, instanceOffset + 4),
                    "Shared native geometry lost independent material");
            Require(Value<float>(bytes, instanceOffset + index * 56 + 44) == (index == 6 ? 25.f : 50.f),
                    "Native instance translation changed");
        }
        for (unsigned index = 9; index < 13; ++index)
            Require(Value<std::uint32_t>(bytes, instanceOffset + index * 56) == Value<std::uint32_t>(bytes, instanceOffset + 8 * 56),
                    "Translated, permuted or repeated baked mesh lost form sharing");
        const std::array<std::int32_t, 5> parents{-1, 0, 1, 0, 0};
        auto cursor = nodeOffset;
        for (const auto parent : parents)
        {
            Require(Value<std::int32_t>(bytes, cursor) == parent, "Node parent changed");
            const auto first = Value<std::uint32_t>(bytes, cursor + 4);
            const auto count = Value<std::uint64_t>(bytes, cursor + 8);
            Require(first + count <= instances, "Node refers to missing instances");
            cursor += 21 + Value<std::uint32_t>(bytes, cursor + 17);
        }
        Require(cursor == materialOffset, "Node table overlaps materials");
    }

    void ValidateExport(const std::filesystem::path& source)
    {
        const auto original = ReadBytes(source);
        Bytes serial;
        std::string serialStats;
        for (unsigned pass = 0; pass < 4; ++pass)
        {
            const auto target = source.parent_path() / ("scene-pass" + std::to_string(pass) + ".3d");
            ciff::Read data(source.string(), target.string()); data.load(); AddRepeatedReference(data);
            const auto before = MeshSnapshot(data);
            ciff::cmd::async = pass > 0;
            ciff::f3d::Convert converter(data, {.workerCount = 3});
            Require(converter.run(), "Serial/async .3d conversion failed");
            if (pass == 0)
            {
                Require(!converter.geometryPipeline, "Serial export started workers");
                serial = ReadBytes(target); ValidateBinary(serial);
                serialStats = converter.catalog.primitiveStats.Format("");
            }
            else
            {
                Require(converter.geometryPipeline && converter.geometryPipeline->participatingWorkerCount() >= 2,
                        "Async export did not use multiple workers");
                Require(ReadBytes(target) == serial, "Async .3d differs byte-for-byte from serial .3d");
                Require(converter.catalog.primitiveStats.Format("") == serialStats, "Async changed primitive sharing statistics");
            }
            Require(MeshSnapshot(data) == before, "Export modified source meshes or palette");
        }
        Require(ReadBytes(source) == original, "Export changed source CIFF bytes");
    }

    void ValidateFallbackAndErrors(const std::filesystem::path& directory)
    {
        ciff::cmd::async = true;
        ciff::Read tiny((directory / "tiny.ciff").string(), (directory / "tiny.3d").string()); tiny.load();
        ciff::f3d::Convert tinyConverter(tiny);
        Require(tinyConverter.run() && !tinyConverter.geometryPipeline, "Tiny scene did not use serial fallback");

        ciff::Read repeated((directory / "scene.ciff").string(), (directory / "shared-boxes.3d").string()); repeated.load();
        repeated.nodes.resize(1); repeated.nodes.front().childCount = 0;
        repeated.nodes.front().geometries.assign(1000, 0);
        ciff::f3d::Convert repeatedConverter(repeated);
        Require(repeatedConverter.run() && !repeatedConverter.geometryPipeline,
                "Many instances of one cheap native form did not use serial fallback");
        Require(repeatedConverter.catalog.emittedFormCount == 1 && repeatedConverter.catalog.emittedInstanceCount == 1000,
                "Shared native fallback lost form sharing");

        ciff::Read sharedTorus((directory / "scene.ciff").string(), (directory / "shared-toruses.3d").string()); sharedTorus.load();
        sharedTorus.nodes.resize(1); sharedTorus.nodes.front().childCount = 0;
        sharedTorus.nodes.front().geometries.assign(1000, 2);
        ciff::f3d::Convert sharedTorusConverter(sharedTorus);
        Require(sharedTorusConverter.run() && !sharedTorusConverter.geometryPipeline,
                "One heavy native form with many instances did not use serial fallback");
        Require(sharedTorusConverter.catalog.emittedFormCount == 1 && sharedTorusConverter.catalog.emittedInstanceCount == 1000,
                "Shared heavy native fallback lost form sharing");

        ciff::Read invalid((directory / "invalid.ciff").string(), "unused.3d"); invalid.load();
        Pipeline failed(invalid, {.workerCount = 3});
        Rejects([&] { for (const auto index : WorkIndices(invalid)) static_cast<void>(failed.Claim(index)); },
                "index is outside its position buffer");
        ciff::Read valid((directory / "scene.ciff").string(), "unused.3d"); valid.load();
        Pipeline cancelled(valid, {.workerCount = 3, .maxOutstandingWork = 2});
        Rejects([&] { static_cast<void>(cancelled.Claim(WorkIndices(valid).front(), [] { return false; })); }, "cancel");

        for (const auto name : {"empty", "invalid"})
        {
            const auto target = directory / (std::string(name) + ".3d");
            { std::ofstream previous(target, std::ios::binary); previous << "PREVIOUS SUCCESSFUL OUTPUT"; }
            const auto existing = ReadBytes(target);
            {
                ciff::Read data((directory / (std::string(name) + ".ciff")).string(), target.string()); data.load();
                const auto before = MeshSnapshot(data);
                ciff::f3d::Convert converter(data, {.workerCount = 3});
                Require(!converter.run(), "Invalid/empty geometry export succeeded");
                Require(MeshSnapshot(data) == before, "Failed export modified source meshes or palette");
            }
            Require(ReadBytes(target) == existing, "Failed export replaced existing output");
        }
#if defined(_WIN32)
        const auto lockedTarget = directory / "locked.3d";
        { std::ofstream previous(lockedTarget, std::ios::binary); previous << "LOCKED EXISTING OUTPUT"; }
        const auto existing = ReadBytes(lockedTarget);
        {
            const auto handle = CreateFileW(lockedTarget.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            Require(handle != INVALID_HANDLE_VALUE, "Could not lock publication fixture");
            struct CloseHandleOnExit { HANDLE handle; ~CloseHandleOnExit() { CloseHandle(handle); } } guard{handle};
            ciff::Read data((directory / "scene.ciff").string(), lockedTarget.string()); data.load();
            ciff::f3d::Convert converter(data, {.workerCount = 3});
            Require(!converter.run(), "Publishing over a locked output unexpectedly succeeded");
            Require(converter.geometryPipeline && converter.geometryPipeline->claimedCount() == 13,
                    "Locked output failed before all worker results were claimed");
        }
        Require(ReadBytes(lockedTarget) == existing, "Failed publication replaced locked output");
#endif
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            Require(entry.path().filename().string().rfind(".conversion-", 0) != 0,
                    "Failed export left a temporary workspace");
    }
}

int main(const int argc, char* argv[])
{
    try
    {
        if (argc == 3 && std::string(argv[1]) == "--fixtures")
        {
            async3d_fixture::WriteAll(argv[2]); return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--primitive-stress")
        {
            async3d_fixture::WritePrimitiveStress(argv[2]); return 0;
        }
        Require(argc == 2, "Usage: CiffAsync3D <output-directory> | --fixtures <directory> | --primitive-stress <file.ciff>");
        const auto directory = std::filesystem::path(argv[1]);
        async3d_fixture::WriteAll(directory); ciff::bar::idle();
        ValidateWorkerPipeline(directory / "scene.ciff");
        ValidateExport(directory / "scene.ciff");
        ValidateFallbackAndErrors(directory);
        std::cout << "CIFF Async .3d: exact binary equality, shared forms, worker participation, bounded work and atomic failures: OK\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n'; return 1;
    }
}
