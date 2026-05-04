#ifndef MPC_CONTROLLER_HPP
#define MPC_CONTROLLER_HPP

// Lightweight MPC pacing controller for UDP video streaming
// Enumerative MPC — evaluates 7 discrete candidates over N=3 horizon
// No heap allocation, no STL, no external dependencies
// Target: ARM Cortex-A7 @ 800MHz, ~1-2ms per update (VFPv4 FPU)

class MPCPacingController {
public:
    static const int NUM_CANDIDATES = 7;
    static const int HORIZON         = 3;    // N=3 steps = 6 seconds at 2s interval
    static const int MAX_PAYLOAD     = 1396; // bytes per fragment payload

    // Cost weights
    float w_throughput;   // reward per Kbps of predicted goodput
    float w_loss;         // penalty per %^2 of predicted loss
    float w_smooth;       // penalty per unit^2 of relative rate change

    // Observable state (for debug/log display at call site)
    float bw_est_kbps;        // estimated available bottleneck bandwidth
    float enc_est_kbps;       // estimated encoder output capacity
    float current_loss;       // latest smoothed loss rate
    int   current_rate_kbps;  // current effective send rate
    float last_cost;          // cost of the chosen control action
    float model_gain;         // adaptive sensitivity of loss-vs-rate [0.5, 3.0]
    int   iteration;          // monotonic counter

    MPCPacingController()
        : w_throughput(1.0f)
        , w_loss(0.12f)
        , w_smooth(100.0f)
        , bw_est_kbps(500.0f)
        , enc_est_kbps(400.0f)
        , current_loss(0.0f)
        , current_rate_kbps(3723)    // 11168000 / 3000 (default initial pacing)
        , last_cost(0.0f)
        , model_gain(1.0f)
        , iteration(0)
        , loss_ema(0.0f)
    {}

    // Main entry: called every 2s after checkFeedback()
    //   measured_loss_percent : from receiver LOSS:x.x feedback (0..100)
    //   actual_throughput_kbps: computed from (bytes_sent * 8) / interval_ms
    //   current_pacing_us     : current pacing_delay_us
    // Returns: optimal pacing_delay_us, clamped to [1000, 100000]
    int update(float measured_loss_percent,
               int actual_throughput_kbps,
               int current_pacing_us);

    // Encoder capacity estimation — only meaningful when pacing=1000
    void update_encoder_est(int actual_tput_kbps);

    // Cold-start / stale protection
    int effective_rate_kbps(int measured_kbps) const {
        if (measured_kbps < 50) measured_kbps = 50;
        if (current_loss <= 0.5f && (float)measured_kbps < enc_est_kbps * 0.8f) {
            return (int)enc_est_kbps;
        }
        return measured_kbps;
    }

    // Conversion utilities (static, integer arithmetic)
    static int pacing_to_rate_kbps(int pacing_us) {
        return (MAX_PAYLOAD * 8 * 1000) / pacing_us;
    }
    static int rate_to_pacing_us(int rate_kbps) {
        if (rate_kbps <= 0) return 100000;
        return (MAX_PAYLOAD * 8 * 1000) / rate_kbps;
    }
    static int clamp_pacing(int pacing_us) {
        return (pacing_us < 1000) ? 1000 : (pacing_us > 100000) ? 100000 : pacing_us;
    }

private:
    // Candidate rate multipliers applied to current_rate_kbps
    static const float rate_multipliers[NUM_CANDIDATES];

    // Internal state (smoothed for system identification)
    float loss_ema;        // EMA of measured loss

    // Online system identification
    void identify(float measured_loss, int actual_throughput_kbps);

    // Predict loss rate for a candidate send-rate
    float predict_loss(int candidate_rate_kbps) const;

    // Full cost function over the N-step horizon for a candidate
    float evaluate_cost(int candidate_rate_kbps, int current_rate_kbps) const;
};

// Candidate rate multipliers — asymmetric (more slowdowns than speedups)
// because the encoder is typically the bottleneck, not the network
const float MPCPacingController::rate_multipliers[NUM_CANDIDATES] = {
    0.50f,   // strong slowdown
    0.67f,   // moderate slowdown
    0.80f,   // mild slowdown
    0.90f,   // slight slowdown
    1.00f,   // hold
    1.11f,   // slight speedup
    1.25f    // moderate speedup
};

inline void MPCPacingController::identify(float measured_loss, int actual_tput_kbps) {
    // Step 1: Update loss EMA
    loss_ema = 0.7f * loss_ema + 0.3f * measured_loss;

    // Step 2: Bandwidth estimation
    if (measured_loss <= 0.5f) {
        bw_est_kbps = 0.85f * bw_est_kbps + 0.15f * (float)actual_tput_kbps;
        model_gain = 0.95f * model_gain + 0.05f * 0.5f;
    } else if (measured_loss <= 3.0f) {
        bw_est_kbps = 0.90f * bw_est_kbps + 0.10f * (float)actual_tput_kbps;
        model_gain = 0.90f * model_gain + 0.10f * 1.0f;
    } else {
        float adjusted = (float)actual_tput_kbps * (1.0f - measured_loss / 100.0f);
        bw_est_kbps = 0.80f * bw_est_kbps + 0.20f * adjusted;
        model_gain = 0.85f * model_gain + 0.15f * 2.0f;
    }

    // Step 4: Clamp
    if (bw_est_kbps < 100.0f)  bw_est_kbps = 100.0f;
    if (bw_est_kbps > 12000.0f) bw_est_kbps = 12000.0f;
    if (enc_est_kbps < 50.0f)  enc_est_kbps = 50.0f;
    if (enc_est_kbps > 12000.0f) enc_est_kbps = 12000.0f;
    if (model_gain < 0.5f)  model_gain = 0.5f;
    if (model_gain > 3.0f)  model_gain = 3.0f;
}

// Separate encoder capacity estimation — only called when pacing is at minimum
// (i.e. we're not artificially throttling, so measured throughput is genuine)
inline void MPCPacingController::update_encoder_est(int actual_tput_kbps) {
    if (current_loss <= 1.0f) {
        enc_est_kbps = 0.90f * enc_est_kbps + 0.10f * (float)actual_tput_kbps;
    }
}

inline float MPCPacingController::predict_loss(int candidate_rate_kbps) const {
    if (candidate_rate_kbps <= (int)bw_est_kbps) {
        return 0.0f;
    }
    // Loss proportional to how much we overshoot bandwidth estimate
    float overshoot = (float)(candidate_rate_kbps - (int)bw_est_kbps);
    float loss = 100.0f * model_gain * overshoot / (float)candidate_rate_kbps;
    if (loss > 100.0f) loss = 100.0f;
    return loss;
}

inline float MPCPacingController::evaluate_cost(int candidate_rate, int current_rate) const {
    float total = 0.0f;
    float sim_loss = loss_ema;   // start from current smoothed loss state

    for (int step = 0; step < HORIZON; step++) {
        // Predicted throughput capped by encoder capacity
        float pred_thru = (candidate_rate < (int)enc_est_kbps)
                          ? (float)candidate_rate : enc_est_kbps;

        float pred_loss_raw = predict_loss(candidate_rate);

        // First-order loss dynamics (prevents instantaneous jumps in prediction)
        sim_loss = 0.6f * sim_loss + 0.4f * pred_loss_raw;

        float pred_goodput = pred_thru * (1.0f - sim_loss / 100.0f);

        // Throughput reward (negative cost for higher goodput)
        total += -w_throughput * pred_goodput;
        // Loss penalty
        total += w_loss * sim_loss * sim_loss;
    }

    // Smoothness penalty (applied once, not per step)
    float delta = (float)(candidate_rate - current_rate) / (float)current_rate;
    total += w_smooth * delta * delta;

    return total;
}

inline int MPCPacingController::update(float measured_loss_percent,
                                       int actual_throughput_kbps,
                                       int current_pacing_us) {
    iteration++;

    // Online identification (bandwidth + model_gain, but NOT encoder est)
    identify(measured_loss_percent, actual_throughput_kbps);
    current_loss = measured_loss_percent;

    // Encoder capacity estimation: only updated when pacing=1000
    // (not artificially throttled), so the measurement is genuine.
    if (current_pacing_us == 1000) {
        update_encoder_est(actual_throughput_kbps);
    }

    // Use effective rate as base: normally actual throughput, but during
    // startup (very low throughput + 0 loss), use encoder estimate instead.
    // This prevents MPC from locking onto a cold-start trough.
    current_rate_kbps = effective_rate_kbps(actual_throughput_kbps);

    // Evaluate all 7 candidates centered on effective rate
    int best_rate = current_rate_kbps;
    float best_cost = 1e9f;

    int min_rate = 50;
    int max_rate = 12000;

    for (int i = 0; i < NUM_CANDIDATES; i++) {
        int candidate_rate = (int)((float)current_rate_kbps * rate_multipliers[i]);
        if (candidate_rate < min_rate) candidate_rate = min_rate;
        if (candidate_rate > max_rate) candidate_rate = max_rate;

        float cost = evaluate_cost(candidate_rate, current_rate_kbps);
        if (cost < best_cost) {
            best_cost = cost;
            best_rate = candidate_rate;
        }
    }

    last_cost = best_cost;

    // Encoder-aware pacing mapping:
    // If MPC wants rate near/above encoder capacity, use minimum pacing
    // (encoder is the bottleneck — no point in pacing slower).
    // Only apply pacing throttle when MPC deliberately slows for congestion.
    if ((float)best_rate >= enc_est_kbps * 0.9f) {
        return 1000;
    }
    return clamp_pacing(rate_to_pacing_us(best_rate));
}

#endif // MPC_CONTROLLER_HPP
