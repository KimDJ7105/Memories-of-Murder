find_package(Boost REQUIRED COMPONENTS system)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(game_server
    src/main.cpp
    src/GameData.cpp
    src/GameRoom.cpp
    src/GameServer.cpp
    src/Session.cpp
    src/Listener.cpp
    src/MockCrimeEvaluator.cpp
    src/MockGuessJudge.cpp
)

target_include_directories(game_server PRIVATE include)

target_link_libraries(game_server PRIVATE
    Boost::system
    nlohmann_json::nlohmann_json
)

# Map/weapon definitions live as JSON under server/data/ instead of being
# compiled in, so new content is a new file, not a rebuild.
target_compile_definitions(game_server PRIVATE MOM_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

if(WIN32)
    target_compile_definitions(game_server PRIVATE _WIN32_WINNT=0x0A00)
    target_compile_options(game_server PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
endif()
