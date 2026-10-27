#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace jitnodes {

// Tuning of the state copies the simulation nodes (particles, boids, springs, reactionDiffusion, gasField, fluid, grow, solver) keep so that
// scrubbing back to an earlier time does not simulate from the start. Process-wide; the defaults suit the editor.
struct SimulationCache {
    static inline int64_t checkpointInterval = 16;                 // steps between copies at first; doubles when a node's copies outgrow the budget
    static inline double replayBudgetMs = 100.0;                   // steps slower than a sixteenth of this shorten the interval; 0 keeps it fixed
    static inline size_t budgetBytes = size_t(64) << 20;           // per node; 0 keeps no copies
    static inline std::atomic<size_t> restores{0};                 // times a node resumed from a copy instead of restarting (for tests)
};

}  // namespace jitnodes
