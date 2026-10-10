#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "MeshNormals.h"
#include "PrimitiveInstanceCIFF.h"
#include "ReadCIFF.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ciff::process
{
    // Sources are immutable after Read::load. Workers canonicalize/hash meshes
    // and build normals, while the coordinator alone owns output and catalogs.
    // A short ordered hash gate assigns each form to its first occurrence;
    // repeated baked meshes therefore keep the serial writer's normal reuse.
    // Reservations cover estimated in-flight copied mesh/normal payloads, not
    // the loaded source scene, coordinator mesh, catalogs or thread stacks.
    class GeometryBuildPipeline final
    {
      public:
        using CancellationProbe = std::function<bool()>;
        static constexpr size_t kReservationBudgetBytes = 64ULL * 1024ULL * 1024ULL;
        static constexpr size_t kMaxOutstandingWork = 64U;
        static constexpr size_t kMinimumParallelWorkBytes = 1024ULL * 1024ULL;

        struct Options final
        {
            size_t workerCount = 0U;
            size_t reservationBudgetBytes = kReservationBudgetBytes;
            size_t maxOutstandingWork = kMaxOutstandingWork;
            size_t minimumParallelWorkBytes = kMinimumParallelWorkBytes;
        };

        struct PreparedGeometry final
        {
            primitive_instance::FormInstance form;
            normal_processing::RenderGeometry mesh;
            bool sourceEmpty = false;
        };

        explicit GeometryBuildPipeline(const Read& data)
            : GeometryBuildPipeline(data, Options{})
        {
        }

        GeometryBuildPipeline(const Read& data, const Options& options)
            : data_(data), options_(options), work_(BuildWorkList(data))
        {
            if (options_.reservationBudgetBytes == 0U || options_.maxOutstandingWork == 0U)
                throw std::invalid_argument("CIFF geometry pipeline budget and window must be positive");
            if (work_.size() < 2U ||
                (options_.workerCount == 0U && !HasParallelWork(data, work_, options_.minimumParallelWorkBytes)))
                return;
            workerCount_ = options_.workerCount == 0U ? SelectWorkerCount(work_.size())
                                                      : std::min(options_.workerCount, work_.size());
            if (workerCount_ < 2U)
            {
                workerCount_ = 0U;
                return;
            }
            resultSlots_.resize(std::min(options_.maxOutstandingWork, work_.size()));
            initialAssignments_.resize(workerCount_);
            workerParticipated_.assign(workerCount_, false);
            workers_.reserve(workerCount_);
            for (size_t index = 0U; index < workerCount_ && CanAssignNextWork(); ++index)
                initialAssignments_[index] = ReserveNextWork();
            try
            {
                for (size_t index = 0U; index < workerCount_; ++index)
                    workers_.emplace_back(&GeometryBuildPipeline::RunWorker, this, index);
            }
            catch (...)
            {
                StopAndJoin();
                throw;
            }
        }

        ~GeometryBuildPipeline() noexcept { StopAndJoin(); }
        GeometryBuildPipeline(const GeometryBuildPipeline&) = delete;
        GeometryBuildPipeline& operator=(const GeometryBuildPipeline&) = delete;

        [[nodiscard]] bool enabled() const noexcept { return workerCount_ != 0U; }
        [[nodiscard]] size_t workerCount() const noexcept { return workerCount_; }
        [[nodiscard]] size_t workCount() const noexcept { return work_.size(); }
        [[nodiscard]] size_t claimedCount() const noexcept
        {
            std::lock_guard lock(mutex_);
            return nextClaimOrdinal_;
        }
        [[nodiscard]] size_t participatingWorkerCount() const noexcept
        {
            std::lock_guard lock(mutex_);
            return participatingWorkerCount_;
        }
        [[nodiscard]] size_t normalBuildCount() const noexcept
        {
            std::lock_guard lock(mutex_);
            return normalBuildCount_;
        }
        [[nodiscard]] size_t peakReservedBytes() const noexcept
        {
            std::lock_guard lock(mutex_);
            return peakReservedBytes_;
        }
        [[nodiscard]] size_t peakOutstandingWork() const noexcept
        {
            std::lock_guard lock(mutex_);
            return peakOutstandingWork_;
        }

        [[nodiscard]] PreparedGeometry Claim(const size_t expectedGeometryIndex,
                                              const CancellationProbe& cancellationProbe = {})
        {
            if (cancellationProbe)
            {
                try
                {
                    if (!cancellationProbe())
                        throw std::runtime_error("Loading was cancelled by the user.");
                }
                catch (...)
                {
                    StopAndJoin();
                    throw;
                }
            }
            std::unique_lock lock(mutex_);
            if (!enabled() || nextClaimOrdinal_ >= work_.size() ||
                work_[nextClaimOrdinal_].geometryIndex != expectedGeometryIndex)
            {
                lock.unlock();
                StopAndJoin();
                throw std::logic_error("CIFF geometry pipeline claim does not match source occurrence order");
            }
            const auto ordinal = nextClaimOrdinal_;
            const auto ready = [this, ordinal]() noexcept
            {
                return stopping_ || workerFailure_ != nullptr || FindCompletedSlot(ordinal) != nullptr;
            };
            if (!cancellationProbe)
                resultReady_.wait(lock, ready);
            else
            {
                while (!resultReady_.wait_for(lock, std::chrono::milliseconds{8}, ready))
                {
                    lock.unlock();
                    try
                    {
                        if (!cancellationProbe())
                            throw std::runtime_error("Loading was cancelled by the user.");
                    }
                    catch (...)
                    {
                        StopAndJoin();
                        throw;
                    }
                    lock.lock();
                }
            }
            if (workerFailure_ != nullptr)
            {
                const auto failure = workerFailure_;
                lock.unlock();
                StopAndJoin();
                std::rethrow_exception(failure);
            }
            if (stopping_)
            {
                lock.unlock();
                StopAndJoin();
                throw std::runtime_error("CIFF geometry pipeline was stopped");
            }
            auto* slot = FindCompletedSlot(ordinal);
            if (slot == nullptr)
                throw std::logic_error("CIFF geometry pipeline lost its expected result");
            const auto failure = slot->failure;
            auto prepared = std::move(slot->prepared);
            reservedBytes_ -= slot->reservedBytes;
            slot->reservedBytes = 0U;
            slot->failure = nullptr;
            slot->state = ResultState::Empty;
            ++nextClaimOrdinal_;
            lock.unlock();
            workAvailable_.notify_all();
            if (failure != nullptr)
            {
                StopAndJoin();
                std::rethrow_exception(failure);
            }
            return prepared;
        }

        void StopAndJoin() noexcept
        {
            {
                std::lock_guard lock(mutex_);
                stopping_ = true;
            }
            NotifyAll();
            for (auto& worker : workers_)
                if (worker.joinable())
                    worker.join();
        }

      private:
        struct WorkItem final
        {
            size_t geometryIndex = 0U;
            size_t reservationBytes = 0U;
        };
        struct WorkAssignment final
        {
            size_t ordinal = 0U;
            size_t geometryIndex = 0U;
        };
        enum class ResultState : unsigned char { Empty, Building, Ready, Failed };
        struct ResultSlot final
        {
            size_t ordinal = 0U;
            size_t reservedBytes = 0U;
            PreparedGeometry prepared;
            std::exception_ptr failure;
            ResultState state = ResultState::Empty;
        };
        struct HashState final
        {
            bool complete = false;
            bool hasMesh = false;
        };
        static constexpr size_t kNoFailureOrdinal = std::numeric_limits<size_t>::max();
        static constexpr size_t kMaximumWorkerCount = 16U;

        void NotifyAll() noexcept
        {
            workAvailable_.notify_all();
            resultReady_.notify_all();
            hashReady_.notify_all();
        }

        [[nodiscard]] static size_t SaturatingAdd(const size_t left, const size_t right) noexcept
        {
            return right > std::numeric_limits<size_t>::max() - left
                       ? std::numeric_limits<size_t>::max() : left + right;
        }
        [[nodiscard]] static size_t SaturatingMultiply(const size_t left, const size_t right) noexcept
        {
            return left != 0U && right > std::numeric_limits<size_t>::max() / left
                       ? std::numeric_limits<size_t>::max() : left * right;
        }

        [[nodiscard]] static size_t PhysicalCoreCount() noexcept
        {
#if defined(_WIN32)
            try
            {
                DWORD bytes = 0U;
                if (::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes) == FALSE &&
                    ::GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes != 0U)
                {
                    for (unsigned int attempt = 0U; attempt < 3U; ++attempt)
                    {
                        const auto elements = static_cast<size_t>(bytes) /
                                                  sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) + 1U;
                        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX> records(elements);
                        if (::GetLogicalProcessorInformationEx(RelationProcessorCore, records.data(), &bytes) != FALSE)
                        {
                            auto offset = size_t{};
                            auto cores = size_t{};
                            const auto* storage = reinterpret_cast<const unsigned char*>(records.data());
                            if (static_cast<size_t>(bytes) > records.size() * sizeof(records.front()))
                                return 0U;
                            while (offset < bytes)
                            {
                                constexpr auto headerBytes = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor);
                                if (bytes - offset < headerBytes)
                                    return 0U;
                                const auto* record = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(storage + offset);
                                if (record->Size < headerBytes || record->Size > bytes - offset)
                                    return 0U;
                                if (record->Relationship == RelationProcessorCore)
                                    ++cores;
                                offset += record->Size;
                            }
                            return cores;
                        }
                        if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER)
                            break;
                    }
                }
            }
            catch (...)
            {
            }
#endif
            const auto logical = static_cast<size_t>(std::thread::hardware_concurrency());
            return logical > 1U ? logical / 2U : 1U;
        }
        [[nodiscard]] static size_t SelectWorkerCount(const size_t workCount) noexcept
        {
            const auto cores = PhysicalCoreCount();
            return std::min(workCount, std::min(cores > 1U ? cores - 1U : 1U, kMaximumWorkerCount));
        }

        [[nodiscard]] static size_t EstimateReservation(const Read& data, const size_t geometryIndex)
        {
            const auto& geometry = data.getGeometry(geometryIndex);
            if (geometry.primitive == Type::Mesh)
            {
                const auto& mesh = data.getMesh(geometry.mesh);
                auto bytes = SaturatingMultiply(mesh.vertices.size(), sizeof(Point) + sizeof(UV2) + sizeof(RGBA4f));
                // Copy, canonical hash tables and worst-case split normal mesh.
                return SaturatingAdd(bytes, SaturatingMultiply(mesh.indices.size(), 384U));
            }
            // The local tessellators cap arcs at 64 segments. Torus is the
            // largest native primitive: <= 65*64 vertices and 8320 triangles.
            auto corners = size_t{8320U * 3U};
            switch (geometry.primitive)
            {
                case Type::Box: corners = 36U; break;
                case Type::Cylinder: corners = 768U; break;
                case Type::Sphere: corners = 2160U; break;
                case Type::SphericalDish: corners = 1656U; break;
                case Type::GeneralCylinder: corners = 800U; break;
                default: break;
            }
            return SaturatingMultiply(corners, 384U);
        }
        [[nodiscard]] static std::vector<WorkItem> BuildWorkList(const Read& data)
        {
            std::vector<WorkItem> work;
            for (const auto& node : data.nodes)
                for (const auto geometryIndex : node.geometries)
                    work.push_back({geometryIndex, EstimateReservation(data, geometryIndex)});
            return work;
        }

        [[nodiscard]] static bool HasParallelWork(const Read& data, const std::vector<WorkItem>& work,
                                                  const size_t minimumBytes)
        {
            auto estimatedBytes = size_t{};
            auto independentWork = size_t{};
            std::unordered_set<uint64_t> nativeHashes;
            for (const auto& item : work)
            {
                const auto& geometry = data.getGeometry(item.geometryIndex);
                if (geometry.primitive != Type::Mesh)
                {
                    const auto hash = primitive_instance::Make(data, geometry).hash;
                    if (hash != 0U && !nativeHashes.insert(hash).second)
                        continue;
                }
                estimatedBytes = SaturatingAdd(estimatedBytes, item.reservationBytes);
                ++independentWork;
                if (independentWork >= 2U && estimatedBytes >= minimumBytes)
                    return true;
            }
            return false;
        }

        [[nodiscard]] ResultSlot* FindCompletedSlot(const size_t ordinal) noexcept
        {
            auto& slot = resultSlots_[ordinal % resultSlots_.size()];
            return slot.ordinal == ordinal &&
                           (slot.state == ResultState::Ready || slot.state == ResultState::Failed)
                       ? &slot : nullptr;
        }
        [[nodiscard]] bool CanAssignNextWork() const noexcept
        {
            if (nextWorkOrdinal_ >= work_.size() || firstFailedOrdinal_ != kNoFailureOrdinal)
                return false;
            const auto outstanding = nextWorkOrdinal_ - nextClaimOrdinal_;
            if (outstanding >= resultSlots_.size())
                return false;
            const auto reservation = work_[nextWorkOrdinal_].reservationBytes;
            if (reservedBytes_ <= options_.reservationBudgetBytes &&
                reservation <= options_.reservationBudgetBytes - reservedBytes_)
                return true;
            return outstanding == 0U && reservedBytes_ == 0U;
        }
        [[nodiscard]] WorkAssignment ReserveNextWork()
        {
            const auto ordinal = nextWorkOrdinal_;
            const auto& item = work_[ordinal];
            auto& slot = resultSlots_[ordinal % resultSlots_.size()];
            if (slot.state != ResultState::Empty)
                throw std::logic_error("CIFF geometry pipeline reused an occupied result slot");
            slot.ordinal = ordinal;
            slot.reservedBytes = item.reservationBytes;
            slot.state = ResultState::Building;
            reservedBytes_ += item.reservationBytes;
            ++nextWorkOrdinal_;
            peakReservedBytes_ = std::max(peakReservedBytes_, reservedBytes_);
            peakOutstandingWork_ = std::max(peakOutstandingWork_, nextWorkOrdinal_ - nextClaimOrdinal_);
            return {ordinal, item.geometryIndex};
        }

        std::shared_ptr<HashState> ReserveHash(const WorkAssignment& assignment,
                                              const uint64_t hash, bool& build)
        {
            std::shared_ptr<HashState> state;
            {
                std::unique_lock lock(mutex_);
                hashReady_.wait(lock, [this, &assignment]() noexcept
                {
                    return stopping_ || firstFailedOrdinal_ < assignment.ordinal ||
                           nextHashOrdinal_ == assignment.ordinal;
                });
                if (stopping_ || firstFailedOrdinal_ < assignment.ordinal)
                    throw std::runtime_error("CIFF geometry pipeline was stopped");
                if (build && hash != 0U)
                {
                    const auto found = hashStates_.find(hash);
                    if (found != hashStates_.end())
                    {
                        state = found->second;
                        hashReady_.wait(lock, [this, &state, &assignment]() noexcept
                        {
                            return stopping_ || firstFailedOrdinal_ < assignment.ordinal || state->complete;
                        });
                        if (stopping_ || firstFailedOrdinal_ < assignment.ordinal)
                            throw std::runtime_error("CIFF geometry pipeline was stopped");
                        build = !state->hasMesh;
                        if (build)
                            state->complete = false;
                    }
                    else
                    {
                        state = std::make_shared<HashState>();
                        hashStates_.emplace(hash, state);
                    }
                }
                ++nextHashOrdinal_;
            }
            hashReady_.notify_all();
            return state;
        }

        PreparedGeometry Prepare(const WorkAssignment& assignment)
        {
            const auto& geometry = data_.getGeometry(assignment.geometryIndex);
            PreparedGeometry prepared;
            prepared.form = primitive_instance::Make(data_, geometry);
            const auto preHash = prepared.form.hash;
            auto build = true;
            std::shared_ptr<HashState> state;
            // Known primitive hashes retain the serial cache point before
            // tessellation, which is essential for highly instanced scenes.
            if (preHash != 0U)
            {
                state = ReserveHash(assignment, preHash, build);
                if (!build)
                    return prepared;
            }
            auto localMesh = primitive_instance::Tessellate(data_, geometry, prepared.form);
            prepared.sourceEmpty = localMesh.empty();
            if (preHash == 0U)
            {
                build = !prepared.sourceEmpty;
                state = ReserveHash(assignment, prepared.form.hash, build);
            }
            if (build && !prepared.sourceEmpty)
            {
                {
                    std::lock_guard lock(mutex_);
                    ++normalBuildCount_;
                }
                prepared.mesh = normal_processing::FinalizeMeshNormals(std::move(localMesh));
            }
            if (state && build)
            {
                {
                    std::lock_guard lock(mutex_);
                    state->hasMesh = !prepared.mesh.empty();
                    state->complete = true;
                }
                hashReady_.notify_all();
            }
            return prepared;
        }

        void RunWorker(const size_t workerIndex) noexcept
        {
            try
            {
                auto initial = initialAssignments_[workerIndex];
                while (true)
                {
                    WorkAssignment assignment;
                    {
                        std::unique_lock lock(mutex_);
                        if (!initial)
                            workAvailable_.wait(lock, [this]() noexcept
                            {
                                return stopping_ || firstFailedOrdinal_ != kNoFailureOrdinal ||
                                       nextWorkOrdinal_ >= work_.size() || CanAssignNextWork();
                            });
                        if (stopping_ || (!initial && firstFailedOrdinal_ != kNoFailureOrdinal) ||
                            (initial && initial->ordinal > firstFailedOrdinal_))
                            return;
                        if (initial)
                        {
                            assignment = *initial;
                            initial.reset();
                        }
                        else
                        {
                            if (nextWorkOrdinal_ >= work_.size())
                                return;
                            assignment = ReserveNextWork();
                        }
                        if (!workerParticipated_[workerIndex])
                        {
                            workerParticipated_[workerIndex] = true;
                            ++participatingWorkerCount_;
                        }
                    }
                    try
                    {
                        auto prepared = Prepare(assignment);
                        {
                            std::lock_guard lock(mutex_);
                            if (stopping_)
                                return;
                            auto& slot = resultSlots_[assignment.ordinal % resultSlots_.size()];
                            slot.prepared = std::move(prepared);
                            slot.state = ResultState::Ready;
                        }
                    }
                    catch (...)
                    {
                        const auto failure = std::current_exception();
                        {
                            std::lock_guard lock(mutex_);
                            if (stopping_)
                                return;
                            auto& slot = resultSlots_[assignment.ordinal % resultSlots_.size()];
                            slot.failure = failure;
                            slot.state = ResultState::Failed;
                            firstFailedOrdinal_ = std::min(firstFailedOrdinal_, assignment.ordinal);
                        }
                    }
                    NotifyAll();
                }
            }
            catch (...)
            {
                {
                    std::lock_guard lock(mutex_);
                    if (workerFailure_ == nullptr)
                        workerFailure_ = std::current_exception();
                    stopping_ = true;
                }
                NotifyAll();
            }
        }

        const Read& data_;
        const Options options_;
        const std::vector<WorkItem> work_;
        std::vector<ResultSlot> resultSlots_;
        std::vector<std::optional<WorkAssignment>> initialAssignments_;
        std::vector<bool> workerParticipated_;
        std::vector<std::thread> workers_;
        std::unordered_map<uint64_t, std::shared_ptr<HashState>> hashStates_;
        mutable std::mutex mutex_;
        std::condition_variable workAvailable_;
        std::condition_variable resultReady_;
        std::condition_variable hashReady_;
        std::exception_ptr workerFailure_;
        size_t workerCount_ = 0U;
        size_t nextWorkOrdinal_ = 0U;
        size_t nextClaimOrdinal_ = 0U;
        size_t nextHashOrdinal_ = 0U;
        size_t firstFailedOrdinal_ = kNoFailureOrdinal;
        size_t reservedBytes_ = 0U;
        size_t participatingWorkerCount_ = 0U;
        size_t normalBuildCount_ = 0U;
        size_t peakReservedBytes_ = 0U;
        size_t peakOutstandingWork_ = 0U;
        bool stopping_ = false;
    };
}
