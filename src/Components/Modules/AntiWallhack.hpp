#pragma once

namespace Components
{
  // Server-side anti-wallhack ("server culling").
  //
  // A wallhack can only draw players the client was told about. This component stops the server
  // from sending a player the position of an enemy they cannot see, so a wallhack has nothing to
  // draw. It uses the engine's own per-client entity mask (entityState_s::clientMask, the same mask
  // GSC hide()/showToPlayer() write): the snapshot writer skips an entity for every client whose
  // bit is set (checked in the packet-entities writer, 0x629490).
  //
  // An enemy is still sent to a viewer when any of these holds:
  //   - the viewer can see a point on them (head, chest, feet - also where both will be a moment
  //     later) from their eye or from a short sidestep left/right, so peeking a corner works
  //   - they are within sv_antiWallhackRadius (footsteps and close sounds)
  //   - they fired in the last 2 seconds and are within sv_antiWallhackFireRadius (gunfire sound,
  //     red dot on the minimap)
  //   - they were visible during the last sv_antiWallhackLinger milliseconds
  //   - the viewer has a UAV up, is dead, spectating, in a killcam or looking through a killstreak
  //     camera (predator missile)
  // Teammates are always sent. Bots are never treated as viewers (they don't render anything).
  //
  // Off by default: sv_antiWallhack 0.
  class AntiWallhack : public Component
  {
  public:
    AntiWallhack();

  private:
    static Dvar::Var Enabled;
    static Dvar::Var Radius;
    static Dvar::Var FireRadius;
    static Dvar::Var Linger;
    static Dvar::Var Peek;
    static Dvar::Var Interval;
    static Dvar::Var DebugPrint;

    // Bits this component set on each player entity's clientMask, so bits set by scripts
    // (hide()/showToPlayer()) are left alone.
    static std::array<unsigned int, Game::MAX_CLIENTS> AppliedMask;
    // Level time until which target [t] stays visible to viewer [v].
    static std::array<std::array<int, Game::MAX_CLIENTS>, Game::MAX_CLIENTS> VisibleUntil;
    static std::array<int, Game::MAX_CLIENTS> LastFireTime;
    static int LastLevelTime;
    static int LastUpdateTime;

    // sv_antiWallhackDebug statistics, printed every 10 seconds
    static std::uint64_t StatTraces;
    static std::uint64_t StatPairs;
    static std::uint64_t StatHidden;
    static std::uint64_t StatMicroseconds;
    static std::uint64_t StatUpdates;
    static int StatLastPrint;

    static void Update();
    static void RemoveAll();
    static void ResetState();
    static bool CanSee(int viewerNum, int targetNum, std::uint64_t& traces);
  };
}
