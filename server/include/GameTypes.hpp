#pragma once

#include <string>

namespace mom {

enum class GameState {
    Lobby,
    RoleAssignment,
    CrimeWriting,
    AIJudging,
    Investigation,
    Result,
    NextRound,
    GameOver
};

enum class Role {
    None,
    Criminal,
    Detective
};

inline std::string to_string(GameState state)
{
    switch (state) {
        case GameState::Lobby: return "Lobby";
        case GameState::RoleAssignment: return "RoleAssignment";
        case GameState::CrimeWriting: return "CrimeWriting";
        case GameState::AIJudging: return "AIJudging";
        case GameState::Investigation: return "Investigation";
        case GameState::Result: return "Result";
        case GameState::NextRound: return "NextRound";
        case GameState::GameOver: return "GameOver";
    }
    return "Unknown";
}

inline std::string to_string(Role role)
{
    switch (role) {
        case Role::None: return "none";
        case Role::Criminal: return "criminal";
        case Role::Detective: return "detective";
    }
    return "none";
}

}
