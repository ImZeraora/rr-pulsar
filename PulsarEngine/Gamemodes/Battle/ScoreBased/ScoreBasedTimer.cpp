#include <Gamemodes/Battle/ScoreBased/ScoreBased.hpp>
#include <UI/CtrlRaceBase/CustomCtrlRaceBase.hpp>
#include <MarioKartWii/UI/Ctrl/CtrlRace/CtrlRaceTime.hpp>
#include <runtimeWrite.hpp>

namespace Pulsar {
namespace ScoreBased {

// PAL Battle HUD allocates one shared CtrlRaceTime (80857cc0). Reuse it for
// HUD slot zero and allocate the same native control for each additional view.
kmRuntimeUse(0x807f7bd0); // constructor
kmRuntimeUse(0x807f842c); // Load
kmRuntimeUse(0x807f7c6c); // InitSelf
kmRuntimeUse(0x807f7ec0); // OnUpdate
kmRuntimeUse(0x807f8768); // SetPositionAnim

static u32 CountExtraTimers() {
    if (!IsSingleCoinBattle()) return 0;
    const u8 count = Racedata::sInstance->racesScenario.localPlayerCount;
    return count > 1 && count <= 4 ? count - 1 : 0;
}

static void CreateExtraTimers(Page &page, u32 index, u32 count) {
    const char *variant = count == 1 ? "CtrlRaceTime_2" : "CtrlRaceTime_4";
    for (u32 slot = 1; slot <= count; ++slot) {
        void *storage = operator new(sizeof(CtrlRaceTime));
        CtrlRaceTime *control = reinterpret_cast<CtrlRaceTime *(*)(void *, u8, u32)>(
            kmRuntimeAddr(0x807f7bd0))(storage, 0, 0);
        page.AddControl(index + slot - 1, *control, 0);
        reinterpret_cast<void (*)(CtrlRaceTime *, const char *, u8)>(
            kmRuntimeAddr(0x807f842c))(control, variant, slot);
    }
}
static UI::CustomCtrlBuilder extraTimers(CountExtraTimers, CreateExtraTimers);

static void InitCoinTimer(CtrlRaceTime *control) {
    // Load writes the base HUD slot at 0x190; the timer also has its own slot
    // at 0x1a8, used by native InitSelf/GetPlayerId. Set it before initialization.
    if (IsSingleCoinBattle()) control->hudSlotId = control->CtrlRaceBase::hudSlotId;
    reinterpret_cast<void (*)(CtrlRaceTime *)>(kmRuntimeAddr(0x807f7c6c))(control);
}
kmWritePointer(0x808d4028, InitCoinTimer);

static void UpdateCoinTimer(CtrlRaceTime *control) {
    if (!IsSingleCoinBattle()) {
        reinterpret_cast<void (*)(CtrlRaceTime *)>(kmRuntimeAddr(0x807f7ec0))(control);
        return;
    }
    control->UpdatePausePosition();
    if (SectionMgr::sInstance->curSection->isPaused) return;
    const u32 remaining = GetCoinTimerMilliseconds();
    control->timer.minutes = remaining / 60000;
    control->timer.seconds = remaining / 1000 % 60;
    control->timer.milliseconds = remaining % 1000;
    control->timer.isActive = true;
    // Bypass the native battle one-minute warning, which otherwise replaces
    // a 20-second personal countdown with a frozen 1:00 for 180 frames.
    control->SetTimer(&control->timer);
}
kmWritePointer(0x808d402c, UpdateCoinTimer);

static void PositionCoinTimer(CtrlRaceTime *control, PositionAndScale &position, float frame) {
    reinterpret_cast<void (*)(CtrlRaceTime *, PositionAndScale &, float)>(
        kmRuntimeAddr(0x807f8768))(control, position, frame);
    if (!IsSingleCoinBattle()) return;
    const u8 count = Racedata::sInstance->racesScenario.localPlayerCount;
    if (count < 2) return;
    const u8 slot = control->CtrlRaceBase::hudSlotId;
    const float x = count == 2 ? 0.0f : (slot & 1) ? 152.0f : -152.0f;
    const bool bottom = count == 2 ? slot == 1 : slot >= 2;
    const float y = bottom ? -40.0f : 200.0f;
    // Preserve the native entrance/exit animation relative to each viewport's
    // top center. Three-player layouts use the four-player arrangement.
    position.position.x += x - control->positionAndscale[1].position.x;
    position.position.y += y - control->positionAndscale[1].position.y;
}
kmWritePointer(0x808d4030, PositionCoinTimer);

} // namespace ScoreBased
} // namespace Pulsar
