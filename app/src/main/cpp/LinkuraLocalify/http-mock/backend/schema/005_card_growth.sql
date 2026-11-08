-- Evolve stage of growable (base) cards. A missing row means stage 0.
-- Style level and limit break count stay in museum_card_state.
CREATE TABLE IF NOT EXISTS offline_card_growth (
    d_card_datas_id TEXT NOT NULL PRIMARY KEY,
    evolve_times INTEGER NOT NULL DEFAULT 0 CHECK(evolve_times >= 0)
);
