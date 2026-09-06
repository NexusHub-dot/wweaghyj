#include "replay.hpp"
#include <iostream>
#include <stdexcept>
using namespace pulse;

void check(bool value, char const* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    bool threw = false;
    try { action(); } catch (std::exception const&) { threw = true; }
    check(threw, "Expected invalid clock data to be rejected");
}

// An independent input-driven simulation: variable update sizes change its
// gravity integration. Recording and replay must reach identical trajectories.
struct Model {
    float x[2]{}, y[2]{}, velocity[2]{};
    std::array<bool, 6> held{};
    void input(Input i) {
        const int p = i.player1 ? 0 : 1;
        if (i.button == 1 && i.down && !held[key(i)] && y[p] == 0) velocity[p] = 14;
        held[key(i)] = i.down;
    }
    Step sample() const { return {float(FixedUpdateClock::seconds), x[0], y[0], x[1], y[1]}; }
    void advance() {
        const float dt = float(FixedUpdateClock::seconds);
        for (int p = 0; p < 2; ++p) {
            x[p] += dt * (3 + 2 * int(held[p * 3 + 2]) - 2 * int(held[p * 3 + 1]));
            velocity[p] -= 30 * dt;
            y[p] = std::max(0.f, y[p] + velocity[p] * dt);
            if (y[p] == 0) velocity[p] = 0;
        }
    }
};

template<class StepFn> void drive(int fps, std::vector<double> const& speeds, StepFn step) {
    FixedUpdateClock clock;
    std::size_t tick = 0;
    for (int frame = 0; frame < 1'000'000 && tick < 720; ++frame) {
        // Change speed during the run as well as between recording and replay.
        const auto speed = speeds[(frame / 37) % speeds.size()];
        clock.add(static_cast<float>(speed / fps));
        while (tick < 720 && clock.take()) step(tick++);
    }
    check(tick == 720, "Simulation did not finish");
}

int main() {
    try {
        Recorder recorder;
        recorder.tape.fixedUpdates = true;
        recorder.tape.levelName = "Speed-change simulation";
        Model recorded;
        drive(144, {.25}, [&](std::size_t tick) {
            if (tick == 30 || tick == 320) recorder.queue({1, true, true});
            if (tick == 50 || tick == 340) recorder.queue({1, false, true});
            if (tick == 100) { recorder.queue({3, true, false}); recorder.queue({1, true, false}); }
            if (tick == 180) { recorder.queue({3, false, false}); recorder.queue({1, false, false}); }
            if (tick == 500) recorder.queue({2, true, true});
            if (tick == 600) recorder.queue({2, false, true});
            recorder.step(recorded.sample(), [&](Input i) { recorded.input(i); });
            recorded.advance();
        });
        std::stringstream persisted;
        write(persisted, recorder.tape);
        const auto tape = read(persisted);
        check(tape.fixedUpdates, "Saved macro lost its fixed timing mode");
        for (int fps : {30, 60, 120, 144, 240, 360, 480, 1000}) {
            for (auto const& speeds : std::vector<std::vector<double>>{{.1}, {.25}, {.5}, {1}, {2}, {4}, {.1, 2, 0, .5, 4, 1}}) {
                Player player;
                Model replayed;
                std::size_t inputCount = 0;
                drive(fps, speeds, [&](std::size_t) {
                    check(player.step(tape, replayed.sample(), .000001f, [&](Input i) {
                        ++inputCount;
                        replayed.input(i);
                    }), "Replay stopped prematurely");
                    replayed.advance();
                });
                check(inputCount == tape.events.size(), "Speed change lost or duplicated inputs");
                check(samePosition(recorded.sample(), replayed.sample(), 0), "Final trajectory changed with speed");
                check(recorded.held == replayed.held, "Final held-input state changed with speed");
            }
        }

        FixedUpdateClock clock;
        clock.add(0);
        check(!clock.take(), "Zero game time advanced an update");
        clock.add(FixedUpdateClock::seconds / 4);
        check(!clock.take(), "Fractional update ran early");
        clock.add(FixedUpdateClock::seconds * 3 / 4);
        check(clock.take() && !clock.take(), "Fractional game time was lost or duplicated");
        clock.add(.5);
        auto revision = clock.revision();
        clock.reset();
        check(clock.revision() != revision && !clock.take(), "Restart kept old accumulated time");
        clock.add(.5);
        int count = 0;
        while (clock.take()) ++count;
        check(count == 120, "Catch-up lost game time");
        rejects([&] { clock.add(-1); });
        rejects([&] { clock.add(std::numeric_limits<double>::quiet_NaN()); });
        rejects([&] { clock.add(std::numeric_limits<double>::infinity()); });
        rejects([&] { clock.add(11); });
        clock.add(6);
        rejects([&] { clock.add(5); });

        std::istringstream legacy("PULSE_MACRO 1\n1 0 0 0\n\"legacy\"\n1 0\n0.0041666669 0 0 0 0\n");
        auto old = read(legacy);
        check(!old.fixedUpdates && old.steps.size() == 1, "Legacy recording changed timing modes");
        std::stringstream legacyRoundtrip;
        write(legacyRoundtrip, old);
        check(!read(legacyRoundtrip).fixedUpdates, "Resaving legacy macro enabled fixed updates");
        rejects([] { std::istringstream s("PULSE_MACRO 2\n1 0 0 0 2\n"); read(s); });
        rejects([] { std::istringstream s("PULSE_MACRO 3\n"); read(s); });
        std::cout << "PASS: slow recording, 56 speed/FPS replay combinations, mid-run speed changes, zero speed, clock reset/catch-up, invalid clocks, v1/v2 persistence\n";
        return 0;
    } catch (std::exception const& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
