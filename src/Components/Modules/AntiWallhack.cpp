#include "AntiWallhack.hpp"

#include "Events.hpp"
#include "Scheduler.hpp"

namespace Components
{
  Dvar::Var AntiWallhack::Enabled;
  Dvar::Var AntiWallhack::Radius;
  Dvar::Var AntiWallhack::FireRadius;
  Dvar::Var AntiWallhack::Linger;
  Dvar::Var AntiWallhack::Peek;
  Dvar::Var AntiWallhack::Interval;
  Dvar::Var AntiWallhack::DebugPrint;

  std::array<unsigned int, Game::MAX_CLIENTS> AntiWallhack::AppliedMask{};
  std::array<std::array<int, Game::MAX_CLIENTS>, Game::MAX_CLIENTS> AntiWallhack::VisibleUntil{};
  std::array<int, Game::MAX_CLIENTS> AntiWallhack::LastFireTime{};
  int AntiWallhack::LastLevelTime = 0;
  int AntiWallhack::LastUpdateTime = 0;

  std::uint64_t AntiWallhack::StatTraces = 0;
  std::uint64_t AntiWallhack::StatPairs = 0;
  std::uint64_t AntiWallhack::StatHidden = 0;
  std::uint64_t AntiWallhack::StatMicroseconds = 0;
  std::uint64_t AntiWallhack::StatUpdates = 0;
  int AntiWallhack::StatLastPrint = 0;

  namespace
  {
    // G_SightTrace(int* hitNum, const float* start, const float* end, int passEntityNum, int contentMask).
    // Same call GSC sightTracePassed(start, end, false, ent) makes (0x5F0C50); hitNum == 0 means clear.
    constexpr auto G_SIGHT_TRACE = 0x4F5FE0;
    // The mask sightTracePassed uses when characters don't block: world geometry only.
    // Glass, water, foliage and smoke don't block it.
    constexpr int SIGHT_MASK_NO_CHARACTERS = 0x801803;

    constexpr int FIRE_REVEAL_MS = 2000;
    constexpr float LEAD_SECONDS = 0.1f;

    // playerState_s::otherFlags bit cleared by GSC cameraUnlink() (0x5F5090): view taken from another entity
    constexpr int POF_CAMERA_LINKED = 0x2;
    // playerState_s::pm_flags bit tested by GSC controlsUnlink() (0x5F5130): controls linked to another entity
    constexpr int PMF_CONTROLS_LINKED = 0x80000;

    bool SightTracePassed(const float* start, const float* end, const int passEntityNum)
    {
      int hitNum = 0;
      Utils::Hook::Call<void(int*, const float*, const float*, int, int)>(G_SIGHT_TRACE)(&hitNum, start, end,
        passEntityNum, SIGHT_MASK_NO_CHARACTERS);
      return hitNum == 0;
    }

    int MaxClients()
    {
      const auto count = (*Game::sv_maxclients)->current.integer;
      return std::clamp(count, 0, static_cast<int>(Game::MAX_CLIENTS));
    }

    // Connected, spawned and alive.
    bool IsAlivePlayer(const int num)
    {
      if (Game::svs_clients[num].header.state < Game::CS_ACTIVE)
      {
        return false;
      }

      const auto* ent = &Game::g_entities[num];
      if (!ent->client || ent->health <= 0)
      {
        return false;
      }

      return ent->client->sess.sessionState == Game::SESS_STATE_PLAYING;
    }

    bool AreEnemies(const int a, const int b)
    {
      const auto teamA = Game::g_entities[a].client->sess.cs.team;
      const auto teamB = Game::g_entities[b].client->sess.cs.team;

      // Free-for-all: everyone is TEAM_FREE and everyone is an enemy
      if (teamA == Game::TEAM_FREE || teamB == Game::TEAM_FREE)
      {
        return true;
      }

      return teamA != teamB;
    }

    float DistanceSquared(const float* a, const float* b)
    {
      const auto dx = a[0] - b[0];
      const auto dy = a[1] - b[1];
      const auto dz = a[2] - b[2];
      return dx * dx + dy * dy + dz * dz;
    }
  }

  bool AntiWallhack::CanSee(const int viewerNum, const int targetNum, std::uint64_t& traces)
  {
    const auto& viewer = Game::g_entities[viewerNum].client->ps;
    const auto& target = Game::g_entities[targetNum].client->ps;

    const auto peek = Peek.get<float>();
    const auto yaw = viewer.viewangles[1] * (3.14159265f / 180.0f);
    const float right[3] = { std::sin(yaw), -std::cos(yaw), 0.0f };

    float eyes[4][3];
    eyes[0][0] = viewer.origin[0];
    eyes[0][1] = viewer.origin[1];
    eyes[0][2] = viewer.origin[2] + viewer.viewHeightCurrent;
    for (auto i = 0; i < 3; ++i)
    {
      eyes[1][i] = eyes[0][i] + viewer.velocity[i] * LEAD_SECONDS; // where the viewer will be
      eyes[2][i] = eyes[0][i] + right[i] * peek;                   // a step to the right
      eyes[3][i] = eyes[0][i] - right[i] * peek;                   // a step to the left
    }

    const auto height = std::max(target.viewHeightCurrent, 11.0f);
    float points[5][3];
    for (auto i = 0; i < 2; ++i)
    {
      points[0][i] = target.origin[i];
      points[1][i] = target.origin[i];
      points[2][i] = target.origin[i];
      points[3][i] = target.origin[i] + target.velocity[i] * LEAD_SECONDS;
      points[4][i] = target.origin[i] + target.velocity[i] * LEAD_SECONDS;
    }
    points[0][2] = target.origin[2] + height + 4.0f;  // head
    points[1][2] = target.origin[2] + height * 0.6f;  // chest
    points[2][2] = target.origin[2] + 8.0f;           // feet
    points[3][2] = target.origin[2] + target.velocity[2] * LEAD_SECONDS + height + 4.0f; // head, a moment later
    points[4][2] = target.origin[2] + target.velocity[2] * LEAD_SECONDS + 8.0f;          // feet, a moment later

    for (const auto& eye : eyes)
    {
      for (const auto& point : points)
      {
        ++traces;
        if (SightTracePassed(eye, point, viewerNum))
        {
          return true;
        }
      }
    }

    return false;
  }

  // Takes away every bit this component set. Script-set bits stay.
  void AntiWallhack::RemoveAll()
  {
    for (std::size_t t = 0; t < Game::MAX_CLIENTS; ++t)
    {
      if (AppliedMask[t])
      {
        auto& mask = Game::g_entities[t].s.clientMask[0];
        mask = static_cast<int>(static_cast<unsigned int>(mask) & ~AppliedMask[t]);
        AppliedMask[t] = 0;
      }
    }
  }

  // New map or shutdown: the entities were reinitialized by the engine, only forget our state.
  void AntiWallhack::ResetState()
  {
    AppliedMask.fill(0);
    LastFireTime.fill(0);
    for (auto& row : VisibleUntil)
    {
      row.fill(0);
    }
    LastUpdateTime = 0;
  }

  void AntiWallhack::Update()
  {
    const auto now = Game::level->time;
    if (now < LastLevelTime)
    {
      ResetState(); // map restarted
      StatLastPrint = now;
    }
    LastLevelTime = now;

    if (!Enabled.get<bool>())
    {
      RemoveAll();
      return;
    }

    if (now >= LastUpdateTime && now - LastUpdateTime < Interval.get<int>())
    {
      return;
    }
    LastUpdateTime = now;

    const auto started = std::chrono::high_resolution_clock::now();
    const auto maxClients = MaxClients();
    const auto radiusSq = Radius.get<float>() * Radius.get<float>();
    const auto fireRadiusSq = FireRadius.get<float>() * FireRadius.get<float>();
    const auto linger = Linger.get<int>();

    for (auto t = 0; t < maxClients; ++t)
    {
      if (IsAlivePlayer(t) && (Game::g_entities[t].client->ps.eFlags & Game::EF_FIRING))
      {
        LastFireTime[t] = now;
      }
    }

    std::array<unsigned int, Game::MAX_CLIENTS> hide{};
    std::uint64_t traces = 0;

    for (auto v = 0; v < maxClients; ++v)
    {
      // Bots don't render anything; dead players, spectators and killcams see everything as before
      if (Game::svs_clients[v].bIsTestClient || !IsAlivePlayer(v))
      {
        continue;
      }

      const auto* viewer = Game::g_entities[v].client;

      // UAV up: the minimap shows enemies anyway. Killstreak camera: the view isn't at the body.
      if (viewer->sess.hasRadar || (viewer->ps.otherFlags & POF_CAMERA_LINKED) ||
        (viewer->ps.pm_flags & PMF_CONTROLS_LINKED))
      {
        continue;
      }

      const float eye[3] = { viewer->ps.origin[0], viewer->ps.origin[1], viewer->ps.origin[2] + viewer->ps.viewHeightCurrent };

      for (auto t = 0; t < maxClients; ++t)
      {
        if (t == v || !IsAlivePlayer(t) || !AreEnemies(v, t))
        {
          continue;
        }

        ++StatPairs;

        if (VisibleUntil[v][t] > now)
        {
          continue; // seen recently
        }

        const auto distSq = DistanceSquared(eye, Game::g_entities[t].client->ps.origin);
        if (distSq <= radiusSq)
        {
          continue; // close enough to hear
        }

        if (now - LastFireTime[t] < FIRE_REVEAL_MS && LastFireTime[t] && distSq <= fireRadiusSq)
        {
          continue; // shooting: audible and shown on the minimap
        }

        if (CanSee(v, t, traces))
        {
          VisibleUntil[v][t] = now + linger;
          continue;
        }

        hide[t] |= 1u << v;
        ++StatHidden;
      }
    }

    for (auto t = 0; t < static_cast<int>(Game::MAX_CLIENTS); ++t)
    {
      if (t >= maxClients)
      {
        hide[t] = 0;
      }

      if (hide[t] == AppliedMask[t])
      {
        continue;
      }

      auto& mask = Game::g_entities[t].s.clientMask[0];
      mask = static_cast<int>((static_cast<unsigned int>(mask) & ~AppliedMask[t]) | hide[t]);
      AppliedMask[t] = hide[t];
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - started);
    StatTraces += traces;
    StatMicroseconds += static_cast<std::uint64_t>(elapsed.count());
    ++StatUpdates;

    if (DebugPrint.get<bool>() && now - StatLastPrint >= 10000)
    {
      Logger::Print("[AntiWallhack] last 10s: {} updates, avg {} us per update, avg {} traces per update, "
        "{} of {} enemy pairs hidden\n",
        StatUpdates, StatUpdates ? StatMicroseconds / StatUpdates : 0, StatUpdates ? StatTraces / StatUpdates : 0,
        StatHidden, StatPairs);

      StatTraces = StatPairs = StatHidden = StatMicroseconds = StatUpdates = 0;
      StatLastPrint = now;
    }
    else if (!DebugPrint.get<bool>())
    {
      StatTraces = StatPairs = StatHidden = StatMicroseconds = StatUpdates = 0;
      StatLastPrint = now;
    }
  }

  AntiWallhack::AntiWallhack()
  {
    Enabled = Dvar::Register<bool>("sv_antiWallhack", false, Game::DVAR_NONE,
      "Don't send players the positions of enemies they cannot see, so wallhacks have nothing to draw");
    Radius = Dvar::Register<float>("sv_antiWallhackRadius", 800.0f, 0.0f, 10000.0f, Game::DVAR_NONE,
      "Enemies closer than this are always sent (footsteps and other close sounds)");
    FireRadius = Dvar::Register<float>("sv_antiWallhackFireRadius", 4000.0f, 0.0f, 50000.0f, Game::DVAR_NONE,
      "Enemies who fired in the last 2 seconds are sent within this distance (gunfire sound, minimap dot)");
    Linger = Dvar::Register<int>("sv_antiWallhackLinger", 500, 0, 5000, Game::DVAR_NONE,
      "Milliseconds an enemy stays sent after they were last visible");
    Peek = Dvar::Register<float>("sv_antiWallhackPeek", 40.0f, 0.0f, 200.0f, Game::DVAR_NONE,
      "Sidestep distance tested left and right of the eye, so enemies appear just before a corner is peeked");
    Interval = Dvar::Register<int>("sv_antiWallhackInterval", 100, 0, 1000, Game::DVAR_NONE,
      "Milliseconds between visibility updates (50 = every server frame at sv_fps 20). Lower reveals enemies "
      "sooner after they come into view but costs more server CPU");
    DebugPrint = Dvar::Register<bool>("sv_antiWallhackDebug", false, Game::DVAR_NONE,
      "Print anti-wallhack cost and hidden-pair counts to the console every 10 seconds");

    // Runs inside G_RunFrame, before snapshots are built for this frame
    Scheduler::Loop(Update, Scheduler::Pipeline::SERVER);

    Events::OnClientDisconnect([](const int clientNum)
    {
      if (clientNum < 0 || clientNum >= static_cast<int>(Game::MAX_CLIENTS))
      {
        return;
      }

      // The leaving player's entity: remove our bits before the slot is reused
      if (AppliedMask[clientNum])
      {
        auto& mask = Game::g_entities[clientNum].s.clientMask[0];
        mask = static_cast<int>(static_cast<unsigned int>(mask) & ~AppliedMask[clientNum]);
        AppliedMask[clientNum] = 0;
      }

      // The leaving player as a viewer: take their bit off every other player
      for (std::size_t t = 0; t < Game::MAX_CLIENTS; ++t)
      {
        const auto bit = 1u << clientNum;
        if (AppliedMask[t] & bit)
        {
          auto& mask = Game::g_entities[t].s.clientMask[0];
          mask = static_cast<int>(static_cast<unsigned int>(mask) & ~bit);
          AppliedMask[t] &= ~bit;
        }

        VisibleUntil[clientNum][t] = 0;
        VisibleUntil[t][clientNum] = 0;
      }

      LastFireTime[clientNum] = 0;
    });

    Scheduler::OnGameShutdown([]
    {
      ResetState();
    });
  }
}
