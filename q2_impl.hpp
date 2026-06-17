#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <algorithm>
#include <vector>
#include <string>
#include <unordered_set>
#include <climits>
#include <cstdint>
#include <cstdio>

inline void run_q2_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q2_total");
    TRACE_DECL_COUNTER(part_scanned);
    TRACE_DECL_COUNTER(part_emitted);
    TRACE_DECL_COUNTER(probe_rows);
    // Filter: r_name = 'ASIA' → get regionkey
    int32_t asia_regionkey = -1;
    for (int i = 0; i < db->n_regions; i++) {
        if (db->r_name[i] == "ASIA") { asia_regionkey = i; break; }
    }

    // Get nationkeys in ASIA
    std::unordered_set<int32_t> asia_nations;
    for (int i = 0; i < db->n_nations; i++) {
        if (db->n_regionkey[i] == asia_regionkey) {
            asia_nations.insert(i);
        }
    }

    // Get suppliers in ASIA nations: suppkey (0-based index) → nationkey
    std::vector<int32_t> asia_suppliers; // indices of suppliers in ASIA
    for (int32_t i = 0; i < db->n_supplier; i++) {
        if (asia_nations.count(db->s_nationkey[i])) {
            asia_suppliers.push_back(i);
        }
    }
    // suppkey → bool (is in asia)
    std::vector<bool> supp_in_asia(db->n_supplier, false);
    for (int32_t idx : asia_suppliers) {
        supp_in_asia[idx] = true;
    }

    // Filter parts: p_size = 8 AND p_type LIKE '%TIN'.
    // Dense bool flag per part (0-based partkey-1) acts as the build side of the
    // ps_partkey = p_partkey join (DuckDB builds on the filtered part set).
    std::vector<char> part_match(db->n_part, 0);
    {
        PROFILE_SCOPE("q2_part_scan");
        for (int32_t i = 0; i < db->n_part; i++) {
            TRACE_INC(part_scanned);
            if (db->p_size[i] == 8) {
                const auto& t = db->p_type[i];
                if (t.size() >= 3 && t.compare(t.size() - 3, 3, "TIN") == 0) {
                    TRACE_INC(part_emitted);
                    part_match[i] = 1;
                }
            }
        }
    }

    // Per-part minimum supplycost over ASIA suppliers (the correlated subquery).
    // Dense array indexed by partkey-1; only matching parts are ever touched.
    std::vector<int64_t> min_cost(db->n_part, INT64_MAX);

    const int32_t* ps_pk = db->ps_partkey.data();
    const int32_t* ps_sk = db->ps_suppkey.data();
    const int64_t* ps_cost = db->ps_supplycost.data();
    const int32_t n_ps = db->n_partsupp;
    const int32_t n_supp = db->n_supplier;

    // Single pass over partsupp: probe the matching-part build side, track the
    // per-part min supplycost over ASIA suppliers, and stash the few surviving
    // candidate rows (only ~thousands) for a cheap second filtering pass.
    struct Cand { int32_t p; int32_t s_idx; int64_t cost; };
    std::vector<Cand> cands;
    cands.reserve(16384);
    {
        PROFILE_SCOPE("q2_partsupp_min");
        for (int32_t i = 0; i < n_ps; i++) {
            int32_t p = ps_pk[i] - 1;
            if (!part_match[p]) continue;
            int32_t s_idx = ps_sk[i] - 1;
            if ((uint32_t)s_idx >= (uint32_t)n_supp || !supp_in_asia[s_idx]) continue;
            TRACE_INC(probe_rows);
            int64_t c = ps_cost[i];
            if (c < min_cost[p]) min_cost[p] = c;
            cands.push_back({p, s_idx, c});
        }
    }

    struct Result {
        int64_t s_acctbal;
        int32_t s_idx;
        int32_t n_idx;
        int32_t p_idx;   // 0-based part index (partkey - 1)
    };

    std::vector<Result> results;
    results.reserve(cands.size());

    // Emit candidates whose supplycost equals their part's minimum.
    {
        PROFILE_SCOPE("q2_join_probe");
        for (const Cand& cd : cands) {
            if (cd.cost != min_cost[cd.p]) continue;
            results.push_back({db->s_acctbal[cd.s_idx], cd.s_idx, db->s_nationkey[cd.s_idx], cd.p});
        }
    }

    TRACE_COUNT("q2_part_rows_scanned", part_scanned);
    TRACE_COUNT("q2_part_rows_emitted", part_emitted);
    TRACE_COUNT("q2_probe_rows_in", probe_rows);
    TRACE_COUNT("q2_join_rows_emitted", (uint64_t)results.size());

    // Order by: s_acctbal desc, n_name, s_name, p_partkey
    {
        PROFILE_SCOPE("q2_sort");
        std::sort(results.begin(), results.end(), [&](const Result& a, const Result& b) {
            if (a.s_acctbal != b.s_acctbal) return a.s_acctbal > b.s_acctbal;
            int cmp = db->n_name[a.n_idx].compare(db->n_name[b.n_idx]);
            if (cmp != 0) return cmp < 0;
            cmp = db->s_name[a.s_idx].compare(db->s_name[b.s_idx]);
            if (cmp != 0) return cmp < 0;
            return (a.p_idx + 1) < (b.p_idx + 1);
        });
    }
    TRACE_COUNT("q2_sort_rows_in", (uint64_t)results.size());
    TRACE_COUNT("q2_sort_rows_out", (uint64_t)results.size());

    PROFILE_SCOPE("q2_output");

    // Manual single-buffer formatting + software prefetch of the scattered
    // per-row string columns (s_*/p_mfgr live at random indices after sorting).
    std::string buf;
    buf.reserve(96 + (size_t)results.size() * 160);
    buf += "s_acctbal,s_name,n_name,p_partkey,p_mfgr,s_address,s_phone,s_comment\n";

    const std::string* __restrict s_name = db->s_name.data();
    const std::string* __restrict s_addr = db->s_address.data();
    const std::string* __restrict s_phone = db->s_phone.data();
    const std::string* __restrict s_comm = db->s_comment.data();
    const std::string* __restrict p_mfgr = db->p_mfgr.data();
    const std::string* __restrict n_name = db->n_name.data();
    const Result* __restrict rs = results.data();
    const size_t nrows = results.size();

    auto append_csv = [&buf](const std::string& s) {
        const char* d = s.data();
        size_t n = s.size();
        bool needs_quote = false;
        for (size_t k = 0; k < n; k++) {
            char c = d[k];
            if (c == ',' || c == '"' || c == '\n' || c == '\r' || c == '\\') { needs_quote = true; break; }
        }
        if (!needs_quote) { buf.append(d, n); return; }
        buf.push_back('"');
        for (size_t k = 0; k < n; k++) {
            char c = d[k];
            if (c == '"') buf.push_back('"');
            buf.push_back(c);
        }
        buf.push_back('"');
    };
    auto append_money = [&buf](int64_t cents) {
        char t[24];
        int n;
        if (cents < 0) n = snprintf(t, sizeof(t), "-%lld.%02d", -cents / 100, (int)(-cents % 100));
        else           n = snprintf(t, sizeof(t),  "%lld.%02d",  cents / 100, (int)( cents % 100));
        buf.append(t, n);
    };

    constexpr size_t PD_OBJ = 16;  // distance to prefetch the string control block
    constexpr size_t PD_DAT = 6;   // distance to prefetch the heap char data
    char numtmp[24];
    for (size_t i = 0; i < nrows; i++) {
        if (i + PD_OBJ < nrows) {
            const Result& rf = rs[i + PD_OBJ];
            __builtin_prefetch(s_name + rf.s_idx, 0, 1);
            __builtin_prefetch(s_comm + rf.s_idx, 0, 1);
            __builtin_prefetch(s_addr + rf.s_idx, 0, 1);
            __builtin_prefetch(p_mfgr + rf.p_idx, 0, 1);
        }
        if (i + PD_DAT < nrows) {
            const Result& rn = rs[i + PD_DAT];
            __builtin_prefetch(s_name[rn.s_idx].data(), 0, 1);
            __builtin_prefetch(s_comm[rn.s_idx].data(), 0, 1);
            __builtin_prefetch(s_addr[rn.s_idx].data(), 0, 1);
        }
        const Result r = rs[i];
        append_money(r.s_acctbal);     buf.push_back(',');
        append_csv(s_name[r.s_idx]);   buf.push_back(',');
        append_csv(n_name[r.n_idx]);   buf.push_back(',');
        buf.append(numtmp, snprintf(numtmp, sizeof(numtmp), "%d", r.p_idx + 1)); buf.push_back(',');
        append_csv(p_mfgr[r.p_idx]);   buf.push_back(',');
        append_csv(s_addr[r.s_idx]);   buf.push_back(',');
        append_csv(s_phone[r.s_idx]);  buf.push_back(',');
        append_csv(s_comm[r.s_idx]);   buf.push_back('\n');
    }
    out.write(buf.data(), (std::streamsize)buf.size());
    TRACE_COUNT("q2_query_output_rows", (uint64_t)results.size());
}
