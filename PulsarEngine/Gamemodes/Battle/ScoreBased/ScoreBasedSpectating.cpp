#include <Gamemodes/Battle/ScoreBased/ScoreBased.hpp>
#include <MarioKartWii/3D/Camera/CameraMgr.hpp>
#include <MarioKartWii/Race/RaceInfo/RaceInfo.hpp>

namespace Pulsar {
namespace ScoreBased {

void UpdateSpectatorCameras() {
    if (!IsCoinBattle() || HasFinished()) return;
    RaceCameraMgr *cameras = RaceCameraMgr::sInstance;
    const Raceinfo *race = Raceinfo::sInstance;
    if (!cameras || !cameras->cameras || !race || !race->playerIdInEachPosition) return;
    const RacedataScenario &scenario = Racedata::sInstance->racesScenario;
    u8 target = 0xff;
    for (u8 pos = 0; pos < scenario.playerCount && pos < 12; ++pos) {
        const u8 id = race->playerIdInEachPosition[pos];
        if (id < scenario.playerCount && !IsEliminated(id)) {
            target = id;
            break;
        }
    }
    if (target == 0xff) return;
    // PAL 805a21d0 fetches the camera's kart through playerId every frame.
    // Change only eliminated local views; other split-screen players keep driving.
    for (u8 local = 0; local < scenario.localPlayerCount && local < cameras->cameraCount; ++local) {
        const u8 id = Racedata::sInstance->GetPlayerIdOfLocalPlayer(local);
        RaceCamera *camera = cameras->cameras[local];
        if (IsEliminated(id) && camera) camera->playerId = target;
    }
}

} // namespace ScoreBased
} // namespace Pulsar
