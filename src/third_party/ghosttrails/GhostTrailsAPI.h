#pragma once

// This header is the DLL ABI boundary. It deliberately requires neither a BMI,
// CommonLib headers nor a shared STL/CRT. Never delete the returned interface.
#include <cstdint>

namespace GhostTrailsAPI
{
    enum class InterfaceVersion : std::uint32_t { V1 = 1 };
    using RequestID = std::uint32_t;
    using ActorHandle = std::uint32_t; // RE::ActorHandle::native_handle(), NOT FormID.

    enum class Result : std::uint32_t
    {
        Accepted = 0,
        InvalidArgument = 1,
        NotReady = 2,
        Capacity = 3,
        NotFound = 4,
        Finished = 5
    };

    enum class State : std::uint32_t
    {
        Queued = 0,
        Running = 1,
        Completed = 2,
        Partial = 3,
        Failed = 4,
        Canceled = 5
    };

    enum class Failure : std::uint32_t
    {
        None = 0,
        ActorGone = 1,
        No3D = 2,
        CloneFailed = 3,
        NoGeometry = 4,
        InvalidSkin = 5,
        Capacity = 6,
        SceneChanged = 7,
        SessionChanged = 8,
        WindowExpired = 9
    };

    struct Appearance
    {
        float red{0.2f};
        float green{0.7f};
        float blue{1.0f};
        float alpha{0.5f};
        float intensity{2.0f};
        float rimPower{2.0f};
    };

    struct SpawnOptions
    {
        Appearance appearance{};
        std::uint32_t count{1};
        float interval{0.05f}; // Scaled game seconds between attempts; no catch-up bursts.
        float lifetime{0.5f}; // Per silhouette, starts on successful attachment.
        float retryWindow{1.0f}; // Per emission, for recoverable failures, in game seconds.
        float spawnWindow{0.0f}; // 0: unlimited; otherwise stop new emissions after this many game seconds.
    };

    struct Status
    {
        State state{State::Queued};
        Failure lastFailure{Failure::None};
        std::uint32_t spawned{};
        std::uint32_t missed{};
        std::uint32_t attempts{};
        std::uint32_t requested{};
    };

    class IVGhostTrails1
    {
    public:
        // All methods are thread-safe. Accepted means admission, not successful spawn.
        // Input structures are copied before return. Output pointers must be writable.
        // Every accepted ID remains queryable until Release or a session reset.
        virtual Result Spawn(ActorHandle actor, const SpawnOptions* options, RequestID* id) = 0;
        virtual Result Prepare(ActorHandle actor, std::uint32_t slots, RequestID* id) = 0;
        virtual Result GetStatus(RequestID id, Status* status) const = 0;
        // Changes subsequent emissions of this request; existing silhouettes keep their look.
        virtual Result SetAppearance(RequestID id, const Appearance* appearance) = 0;
        // Stops future emissions and removes this request's existing silhouettes next update.
        virtual Result Cancel(RequestID id) = 0;
        // Stops and removes the request, its silhouettes and retained status next update.
        virtual Result Release(RequestID id) = 0;
        virtual Result SetDefaultAppearance(const Appearance* appearance) = 0;
        virtual Appearance GetDefaultAppearance() const = 0;

    protected:
        ~IVGhostTrails1() = default;
    };

    using RequestPluginAPIFunction = void* (*)(InterfaceVersion);
}

// Define GHOSTTRAILS_NO_WINDOWS_HELPER for engine-independent consumers/tests.
#ifndef GHOSTTRAILS_NO_WINDOWS_HELPER
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <Windows.h>

namespace GhostTrailsAPI
{
    // Ask at SKSE kPostLoad or later. A null result means absent/unsupported API.
    [[nodiscard]] inline IVGhostTrails1* RequestPluginAPI()
    {
        const auto dll = GetModuleHandleW(L"GhostTrails.dll");
        if (!dll) return nullptr;
        const auto request = reinterpret_cast<RequestPluginAPIFunction>(GetProcAddress(dll, "RequestPluginAPI"));
        return request ? static_cast<IVGhostTrails1*>(request(InterfaceVersion::V1)) : nullptr;
    }
}
#endif
