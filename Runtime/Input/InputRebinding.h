#pragma once
#include "InputUser.h"

namespace NLS::Input
{
enum class RebindingStatus : uint8_t { WaitingForRelease, Listening, Completed, Canceled, TimedOut };
struct RebindingOptions
{
    bool keyboard = true;
    bool mouse = true;
    bool gamepad = true;
    bool analogAxes = true;
    float axisThreshold = 0.65f;
    float axisNeutralThreshold = 0.2f;
    double axisSettleTime = 0.05;
    double timeout = 10;
    uint16_t cancelKey = 256;
    size_t partIndex = 0;
};
class NLS_INPUT_API RebindingSession final
{
public:
    RebindingSession(InputSystem& system, InputUser& user, BindingId binding, RebindingOptions options = {});
    ~RebindingSession();
    RebindingSession(const RebindingSession&) = delete;
    RebindingSession& operator=(const RebindingSession&) = delete;
    RebindingStatus Update();
    void Cancel();
    RebindingStatus GetStatus() const { return m_status; }
    const BindingOverride& GetResult() const { return m_result; }
    bool Apply();
private:
    void Complete(ControlPath path, uint32_t modifiers);
    InputSystem& m_system;
    InputUser& m_user;
    RebindingOptions m_options;
    BindingOverride m_result;
    std::vector<std::pair<DeviceId, uint16_t>> m_startControls;
    std::vector<ControlPath> m_initialAxes;
    uint64_t m_checkpoint = 0;
    double m_started = 0;
    double m_candidateTime = 0;
    ControlPath m_candidate;
    bool m_hasCandidate = false;
    ValueNature m_nature = ValueNature::State;
    RebindingStatus m_status = RebindingStatus::WaitingForRelease;
    std::shared_ptr<const CompiledInput> m_definition;
    uint64_t m_profileRevision = 0;
    bool m_applied = false;
};

struct BindingConflict
{
    BindingId binding;
    ContextId context;
    ActionId action;
};
NLS_INPUT_API std::vector<BindingConflict> FindBindingConflicts(const InputUser& user, BindingId target,
    std::span<const ControlPart> parts, const Chord& chord,
    const std::function<bool(ContextId, ContextId)>& mutuallyExclusive = {});
}
