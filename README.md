# Explain: tpch_sf10_0_8723yhr82

## Pipeline summary

| Phase | Turns | API Calls | Elapsed | Cost |
|-------|-------|-----------|---------|------|
| setup        | 0 | 0 | 0 s | $0.00 |
| storage_plan | 1 | 3 | 140 s | $0.30 |
| base_impl    | 51 | 277 | 2515 s | $16.60 |
| optimization | 97 | 2082 | 27106 s | $123.60 |
| fix          | — | — | — s | $— |

The pipeline produced a full C++ TPC-H engine (loader, builder, and 22 query executors), then ran a long per-query optimization loop with git checkpoints before/after each query and before each optimization stage.

`storage_plan.txt` was generated as a speculative design document, but the shipped engine uses a much simpler fully materialized in-memory columnar layout (`std::vector`-backed columns in `builder_impl.hpp/.cpp`), plus a few derived arrays (not the full speculative index set described in that text file).

Optimization was commit-heavy and iterative: many techniques were tried per query, then selectively kept by stage boundary. Most lasting wins are branchless filtering, dense-array replacements for hash/maps, AVX2 left-pack/filter loops, and output-path reductions in string-heavy queries.

---

## Storage plan

### region

- **Partitioning/segmentation:** no partitioning; full table materialized as a single vector.
- **Sort order:** input/parquet order preserved.
- **Hot/cold columns:** hot: `r_name`; cold/not loaded: `r_regionkey`, `r_comment` are not stored explicitly.
- **Per-segment metadata:** none.
- **Derived structures:** `region_name_to_key` map in `Database`.
- **Cross-query sharing:** region key lookup reused by q2/q5/q8.
- **✅/❌ trade-off:** ✅ tiny and cache-resident; ❌ loses explicit regionkey/comment columns.

### nation

- **Partitioning/segmentation:** single materialized vectors.
- **Sort order:** parquet order.
- **Hot/cold columns:** hot: `n_name`, `n_regionkey`; cold/not loaded: `n_comment`.
- **Per-segment metadata:** none.
- **Derived structures:** `nation_name_to_key`; `nations_in_region`.
- **Cross-query sharing:** nation-key/name maps reused across nearly all joins.
- **✅/❌ trade-off:** ✅ fast key/name lookup; ❌ no compressed/encoded dimension layout.

### supplier

- **Partitioning/segmentation:** single vectors for all loaded columns.
- **Sort order:** parquet order (suppkey-dense assumption used by queries).
- **Hot/cold columns:**

| Column | Type |
|---|---|
| s_name | `std::string` |
| s_address | `std::string` |
| s_nationkey | `int32_t` |
| s_phone | `std::string` |
| s_acctbal | `int64_t` (cents) |
| s_comment | `std::string` |

- **Per-segment metadata:** none.
- **Derived structures:** none global; many queries build supplier membership bitmaps at runtime.
- **Cross-query sharing:** `s_nationkey` is heavily reused by q2/q5/q7/q8/q11/q20/q21.
- **✅/❌ trade-off:** ✅ straightforward random-access by suppkey index; ❌ no persisted nation partition lists/bitmaps.

### customer

- **Partitioning/segmentation:** single vectors.
- **Sort order:** parquet order; accessed as `custkey-1` dense index.
- **Hot/cold columns:**

| Column | Type |
|---|---|
| c_name | `std::string` |
| c_address | `std::string` |
| c_nationkey | `int32_t` |
| c_phone | `std::string` |
| c_acctbal | `int64_t` (cents) |
| c_mktsegment | `std::string` |
| c_comment | `std::string` |

- **Per-segment metadata:** none.
- **Derived structures:** none persisted; query-local bitmaps for segment/phone-prefix in q3/q22.
- **Cross-query sharing:** dense custkey addressing supports q3/q5/q8/q10/q13/q22.
- **✅/❌ trade-off:** ✅ simple O(1) key-to-row mapping; ❌ string-heavy columns remain expensive unless query-local filtering is selective.

### part

- **Partitioning/segmentation:** single vectors.
- **Sort order:** parquet order; accessed as `partkey-1`.
- **Hot/cold columns:** loaded: `p_name,p_mfgr,p_brand,p_type,p_size,p_container,p_retailprice`; not loaded: `p_comment`.
- **Per-segment metadata:** none.
- **Derived structures:** none persisted; query-local bitmaps/group maps (q8/q9/q14/q16/q17/q19/q20).
- **Cross-query sharing:** partkey-dense indexing is central to q2/q8/q9/q14/q16/q17/q19/q20.
- **✅/❌ trade-off:** ✅ good dense-key probing; ❌ no global trigram/prefix/suffix index from speculative plan.

### partsupp

- **Partitioning/segmentation:** single vectors.
- **Sort order:** effectively relies on source ordering by `ps_partkey` in q11 streaming group-by.
- **Hot/cold columns:** loaded: `ps_partkey,ps_suppkey,ps_availqty,ps_supplycost`; not loaded: `ps_comment`.
- **Per-segment metadata:** none.
- **Derived structures:** none persisted; query-local flattened/grouped arrays in q2/q9/q11/q16/q20.
- **Cross-query sharing:** reused in q2/q9/q11/q16/q20.
- **✅/❌ trade-off:** ✅ easy scans and arithmetic; ❌ no persisted perfect-hash pair index.

### orders

- **Partitioning/segmentation:** single vectors.
- **Sort order:** detected at build time (`orders_sorted_by_orderkey`); orderkey direct map built regardless.
- **Hot/cold columns:**

| Column | Type |
|---|---|
| o_orderkey | `int32_t` |
| o_custkey | `int32_t` |
| o_orderstatus | `char` |
| o_totalprice | `int64_t` |
| o_orderdate | `Date=int32_t` |
| o_orderpriority | `std::string` |
| o_shippriority | `int32_t` |
| o_comment | `std::string` |

- **Per-segment metadata:** none.
- **Derived structures:** `orderkey_to_idx` dense direct-address map; `max_orderkey`; sortedness flag.
- **Cross-query sharing:** `orderkey_to_idx` is critical across q3/q4/q5/q7/q8/q9/q10/q12/q18/q21.
- **✅/❌ trade-off:** ✅ fast random orderkey probes; ❌ no persisted zonemap/month bitmap.

### lineitem

- **Partitioning/segmentation:** single giant vectors (no microblocks in actual impl).
- **Sort order:** detected at build time (`lineitem_sorted_by_orderkey`).
- **Hot/cold columns:**

| Column | Type |
|---|---|
| l_orderkey/l_partkey/l_suppkey | `int32_t` |
| l_linenumber | `int32_t` |
| l_quantity/l_extendedprice/l_discount/l_tax | `int64_t` fixed-point |
| l_returnflag/l_linestatus | `char` |
| l_shipdate/l_commitdate/l_receiptdate | `Date=int32_t` |
| l_shipinstruct/l_shipmode | dictionary-coded (`uint8_t` + dict vector) |

- **Per-segment metadata:** none.
- **Derived structures:** optional CSR-style `orderkey_lineitem_range` when lineitem is orderkey-sorted.
- **Cross-query sharing:** primary fact scan for all 22 queries; dictionary-coded shipmode/instruct used directly by q12/q19.
- **✅/❌ trade-off:** ✅ contiguous vector scans + optional CSR acceleration; ❌ no persisted revenue vector/zonemaps/inverted indexes from speculative plan.

---

## Query plans

### q1

**SQL**: aggregate lineitem by `(l_returnflag,l_linestatus)` under shipdate cutoff and order by keys.

```text
OUTPUT result1.csv
└─ ORDER_BY [l_returnflag, l_linestatus]
   └─ PERFECT_HASH_GROUP_BY [rf, ls]
      aggregates: sum_qty, sum_base_price, sum_disc_price, sum_charge, sum_disc, count
      └─ FILTER [l_shipdate <= 1998-08-23]
         └─ SCAN lineitem [AVX2 8-lane masked arithmetic + dual accumulator banks]
```

**Execution analysis**

- Hot loop is AVX2 vector scan over shipdate/qty/price/discount/tax with masked contribution.
- Grouping uses a flat 26×26 array, not hashmap/map.
- Fixed-point arithmetic stays integer until output formatting; no float accumulation drift.

### q2

**SQL**: find ASIA suppliers for size-8 TIN parts where supplycost equals per-part minimum, then sort.

```text
OUTPUT result2.csv
└─ ORDER_BY [s_acctbal DESC, n_name, s_name, p_partkey]
   └─ FILTER [ps_supplycost = per-part min in ASIA]
      └─ HASH_JOIN [part↔partsupp↔supplier↔nation/region folded to bool arrays]
         ├─ FILTER part [p_size=8 AND p_type LIKE %TIN]
         ├─ FILTER supplier [nation in ASIA]
         └─ SCAN partsupp [single-pass min-cost tracking + candidate stash]
```

**Execution analysis**

- Region/nation/supplier predicates are collapsed to dense bool arrays.
- Partsupp scanned once to compute min-cost and collect candidates; second pass only over compact candidates.
- Output path is heavily optimized (single buffer + software prefetch of scattered strings).

### q3

**SQL**: compute revenue per order for BUILDING customers with orderdate/shipdate constraints, then sort by revenue.

```text
OUTPUT result3.csv
└─ ORDER_BY [revenue DESC, o_orderdate]
   └─ HASH_GROUP_BY [l_orderkey, o_orderdate, o_shippriority]
      sum(l_extendedprice*(1-l_discount))
      └─ HASH_JOIN [lineitem ↔ qualifying orders]
         ├─ FILTER customer [c_mktsegment='BUILDING'] → bitmap
         ├─ FILTER orders [o_orderdate < 1995-03-08] + customer bitmap → orderkey bitmap
         └─ FILTER lineitem [l_shipdate > 1995-03-08 AND orderkey bitmap]
            └─ SCAN lineitem [AVX2 decoupled hit-collection + run-length aggregation]
```

**Execution analysis**

- Build side is compressed to bitmaps (`cust_building`, `qbits`), reducing random probes.
- AVX2 select phase collects sparse hits; aggregation then runs on survivors only.
- Revenue uses integer fixed-point product (`scale 4`) then formatted once.

### q4

**SQL**: count orders by priority for a 3-month orderdate window with `EXISTS late lineitem`.

```text
OUTPUT result4.csv
└─ ORDER_BY [o_orderpriority]
   └─ HASH_GROUP_BY [o_orderpriority] count(*)
      └─ SEMI_JOIN [orders EXISTS lineitem(commit<receipt)]
         ├─ FILTER orders [1993-09-01 <= o_orderdate < 1993-12-01]
         └─ PROBE lineitem CSR by orderkey
            └─ SCAN lineitem ranges [AVX2 left-pack order-row collection + prefetched probes]
```

**Execution analysis**

- Uses `orderkey_lineitem_range` CSR when lineitem is sorted, avoiding full lineitem scan.
- Date filter is AVX2 branch-light range check, then compacted order index list.
- Two-phase gather/probe improves memory-level parallelism on random lineitem range accesses.

### q5

**SQL**: AFRICA revenue by nation across customer-orders-lineitem-supplier joins in 1997.

```text
OUTPUT result5.csv
└─ ORDER_BY [revenue DESC]
   └─ HASH_GROUP_BY [n_name] sum(revenue)
      └─ HASH_JOIN chain [orders↔customer↔lineitem↔supplier]
         ├─ FILTER nation/region [AFRICA] → nation bool table
         ├─ FILTER orders [1997 year] + customer nation membership
         └─ PROBE lineitem via orderkey CSR, then supplier nation equality
```

**Execution analysis**

- Join order is orders-driven, not full lineitem scan.
- Pipeline split into phases with explicit prefetch at each random-gather stage.
- Aggregation is dense per-nation array (`nation_revenue[64]`).

### q6

**SQL**: single SUM over lineitem with shipdate/discount/quantity filters.

```text
OUTPUT result6.csv
└─ UNGROUPED_AGGREGATE [sum(l_extendedprice*l_discount)]
   └─ FILTER [shipdate in 1993, discount in [7,9], quantity<2400]
      └─ SCAN lineitem [fully vectorized AVX2 branchless masked multiply-add]
```

**Execution analysis**

- Pure scan/aggregate kernel; no joins/grouping.
- Implements unsigned-range tricks for branchless comparisons.
- 4-lane int64 accumulation is done in registers then horizontal reduced.

### q7

**SQL**: revenue by supplier/customer nation pair (ALGERIA/BRAZIL) and ship year.

```text
OUTPUT result7.csv
└─ ORDER_BY [supp_nation, cust_nation, l_year]
   └─ HASH_GROUP_BY [supp_nation, cust_nation, l_year] sum(volume)
      └─ JOIN chain [supplier↔lineitem↔orders↔customer↔nation]
         ├─ FILTER supplier/customer nations [ALGERIA/BRAZIL codes]
         ├─ FILTER lineitem shipdate [1995-01-01..1996-12-31]
         └─ MERGE_WALK lineitem-orderkey with cached customer nation code
```

**Execution analysis**

- Two-stage prefilter compacts candidate lineitems before order join.
- When sort-order assumptions hold, uses merge-walk instead of random hash lookups.
- Final aggregation is tiny fixed 2×2 array by nation direction and year.

### q8

**SQL**: yearly market share for FRANCE under AMERICA customer region and part type filter.

```text
OUTPUT result8.csv
└─ ORDER_BY [o_year]
   └─ HASH_GROUP_BY [o_year] sum(france_volume)/sum(total_volume)
      └─ JOIN chain [part↔lineitem↔orders↔customer↔supplier↔nation/region]
         ├─ FILTER part [p_type='ECONOMY BRUSHED TIN'] → bitmap
         ├─ FILTER customer region [AMERICA] → bitmap
         ├─ FILTER supplier nation [FRANCE] → bitmap
         └─ SCAN lineitem [branchless candidate pack + prefetched order bitmap probes]
```

**Execution analysis**

- Converts multiple dimension predicates into bitmaps to keep probe structures L2/L3-resident.
- Uses bit-packed `ord_valid` and `ord_year` instead of larger per-order arrays.
- Aggregation reduced to two year buckets (1995/1996).

### q9

**SQL**: profit by nation/year for parts with name LIKE `%rosy%`.

```text
OUTPUT result9.csv
└─ ORDER_BY [nation ASC, o_year DESC]
   └─ HASH_GROUP_BY [nation, o_year] sum(amount)
      └─ JOIN chain [part↔lineitem↔partsupp↔orders↔supplier↔nation]
         ├─ FILTER part [p_name LIKE %rosy%] → bitmap + dense part ids
         ├─ BUILD compact partsupp slices per matching part
         └─ SCAN lineitem [left-pack survivors, then decoupled order/date gathers]
```

**Execution analysis**

- Matching part set is represented as bitset + dense id mapping.
- Partsupp rows for matching parts are compacted into contiguous arrays for cheap lookup.
- Aggregation uses dense `(nation,year)` array of `__int128` accumulator slots.

### q10

**SQL**: top customers by returned-item revenue for a 3-month orderdate window.

```text
OUTPUT result10.csv
└─ ORDER_BY [revenue DESC]
   └─ HASH_GROUP_BY [c_custkey (+FD columns)] sum(revenue)
      └─ JOIN [orders(date) ↔ lineitem(returnflag='R') ↔ customer ↔ nation]
         ├─ FILTER orders [1993-08-01 <= date < 1993-11-01]
         └─ PROBE lineitem via orderkey CSR, then dense customer accumulator
```

**Execution analysis**

- Orders-driven CSR probing avoids 60M-row full scan.
- Output path is two-pass: serialize fragments in custkey order, then stitch sorted fragments.
- Sorting is LSD radix on integer revenue key (descending via reverse emit).

### q11

**SQL**: parts with RUSSIA stock value above `0.01%` of total, sorted by value.

```text
OUTPUT result11.csv
└─ ORDER_BY [value DESC]
   └─ FILTER HAVING [part_value*10000 > total_value]
      └─ HASH_GROUP_BY [ps_partkey] sum(ps_supplycost*ps_availqty)
         └─ JOIN [partsupp ↔ supplier ↔ nation='RUSSIA']
            ├─ FILTER supplier [nation=RUSSIA] → membership bitmap
            └─ SCAN partsupp [streaming per-part aggregation]
```

**Execution analysis**

- Uses supplier bitmap membership test in the hot partsupp scan.
- Exploits sorted/clustered partsupp by partkey to stream groups without random scatters.
- Threshold comparison kept integer (`value*10000 > total`).

### q12

**SQL**: high/low priority line counts by shipmode (MAIL/FOB) under date and consistency predicates.

```text
OUTPUT result12.csv
└─ ORDER_BY [l_shipmode]
   └─ HASH_GROUP_BY [l_shipmode] [high_count, low_count]
      └─ JOIN [lineitem ↔ orders]
         ├─ FILTER lineitem [receipt in 1997, commit<receipt, ship<commit]
         │  └─ SCAN lineitem [AVX2 pass-1 left-pack survivors]
         └─ PROBE orders [orderkey_to_idx, then orderpriority first-char test]
```

**Execution analysis**

- Two-pass design: vectorized date/consistency filter then compacted probe loop.
- Shipmode is dictionary-coded; hot loop uses code→tag table (`MAIL/FOB/other`).
- Priority classification uses first character (`'1'/'2'` high).

### q13

**SQL**: histogram of per-customer order counts excluding comments like `%express%requests%`.

```text
OUTPUT result13.csv
└─ ORDER_BY [custdist DESC, c_count DESC]
   └─ HASH_GROUP_BY [c_count] count(*)
      └─ LEFT_JOIN_SEMANTICS [customer LEFT orders(comment NOT LIKE pattern)]
         ├─ SCAN orders [anchor-char pattern matcher + comment prefetch]
         └─ AGG customer counts [dense custkey array, then histogram]
```

**Execution analysis**

- Pattern matcher anchors on rare chars (`x`,`q`) before full token check.
- Per-customer counts use dense array (no hash join state).
- Final histogram is small `unordered_map<int32,int64>` then sorted.

### q14

**SQL**: promo revenue ratio for one-month shipdate window.

```text
OUTPUT result14.csv
└─ UNGROUPED_AGGREGATE [100*sum(promo_rev)/sum(all_rev)]
   └─ JOIN [lineitem ↔ part]
      ├─ BUILD part promo-flag byte array [p_type LIKE 'PROMO%']
      └─ FILTER lineitem [1995-05-01 <= shipdate < 1995-06-01]
         └─ SCAN lineitem [AVX2 date fast-reject + pipelined survivor gather]
```

**Execution analysis**

- All string prefix work is hoisted to part build-side (`is_promo[p]`).
- Scan kernel pipelines prefetch and deferred gather for sparse date survivors.
- Uses exact int64 sums for numerator/denominator; ratio computed once at end.

### q15

**SQL**: supplier(s) with max revenue over a 3-month lineitem window.

```text
OUTPUT result15.csv
└─ ORDER_BY [s_suppkey]
   └─ FILTER [total_revenue = global max]
      └─ HASH_GROUP_BY [l_suppkey] sum(revenue)
         └─ FILTER lineitem [1996-02-01 <= shipdate < 1996-05-01]
            └─ SCAN lineitem [AVX2 left-pack tiled filter + dense suppkey scatter]
```

**Execution analysis**

- Group-by is perfect-hash style dense array indexed by suppkey.
- Filter pass uses tiled AVX2 compaction, then survivor scatter-aggregate.
- Max reduction and supplier lookup are linear in supplier table size.

### q16

**SQL**: count distinct suppliers per `(brand,type,size)` after part/supplier exclusion predicates.

```text
OUTPUT result16.csv
└─ ORDER_BY [supplier_cnt DESC, p_brand, p_type, p_size]
   └─ HASH_GROUP_BY [p_brand,p_type,p_size] count(distinct ps_suppkey)
      └─ JOIN [part ↔ partsupp] + ANTI_JOIN [bad suppliers]
         ├─ FILTER part [brand/type/size predicates] → dense gid mapping
         ├─ FILTER supplier [comment like Customer...Complaints] → bad bitmap
         └─ SCAN partsupp [emit (gid,suppkey), counting-sort buckets, stamp dedup]
```

**Execution analysis**

- Part groups use packed integer keys instead of string concatenation hashing.
- Distinct counting done via counting-sort by gid plus per-supplier stamp array.
- Sorting uses integer surrogates (`brand_code`, `type_rank`) to avoid string compare cost.

### q17

**SQL**: average yearly extendedprice for selected brand/container with correlated quantity threshold.

```text
OUTPUT result17.csv
└─ UNGROUPED_AGGREGATE [sum(l_extendedprice)/7.0]
   └─ FILTER [l_quantity < 0.2*avg(l_quantity) per part]
      └─ JOIN [lineitem ↔ filtered part]
         ├─ FILTER part [Brand#13 AND MED BOX] → dense map + match bitmap
         └─ SCAN lineitem
            ├─ PASS1 aggregate per matching part [sum_qty,count] + materialize survivors
            └─ PASS2 threshold test on survivor buffer
```

**Execution analysis**

- Part filter uses fixed-width word compare for `Brand#13`.
- Survivor materialization in pass1 avoids rescanning all lineitem in pass2.
- Correlated predicate rewritten to integer inequality (`qty*count*5 < sum_qty`).

### q18

**SQL**: orders/customers where total lineitem quantity per order exceeds 314.

```text
OUTPUT result18.csv
└─ ORDER_BY [o_totalprice DESC, o_orderdate]
   └─ HASH_GROUP_BY [c_name,c_custkey,o_orderkey,o_orderdate,o_totalprice] sum(l_quantity)
      └─ SEMI_JOIN [orders IN big-order set]
         ├─ HASH_GROUP_BY lineitem [l_orderkey] sum(l_quantity) > 314
         └─ JOIN [orders ↔ customer]
```

**Execution analysis**

- First pass is dense order-indexed quantity aggregation (`order_qty_sum[order_idx]`).
- Big-order extraction uses AVX2 compare over dense sums.
- Final materialization is lightweight since big-order set is small.

### q19

**SQL**: sum revenue over three OR-ed brand/container/quantity bundles with ship filters.

```text
OUTPUT result19.csv
└─ UNGROUPED_AGGREGATE [sum(l_extendedprice*(1-l_discount))]
   └─ JOIN [lineitem ↔ part-group tags]
      ├─ BUILD part group tags {1,2,3,0} from brand/container/size
      └─ SCAN lineitem
         ├─ PASS1 AVX2 dict-code filter [shipinstruct='DELIVER IN PERSON' AND shipmode in AIR]
         └─ PASS2 probe part group + quantity-range check + revenue accumulate
```

**Execution analysis**

- String predicates on shipmode/instruct are dictionary-code comparisons in SIMD.
- Part predicates are precompiled into byte group tags to remove string ops from probe.
- Two-pass design shrinks expensive probes to pass1 survivors (~sparse subset).

### q20

**SQL**: French suppliers whose partsupp availqty exceeds half of 1997 shipped quantity for linen parts.

```text
OUTPUT result20.csv
└─ ORDER_BY [s_name]
   └─ FILTER supplier [nation='FRANCE' AND suppkey in qualifying set]
      └─ SEMI_JOIN [partsupp against aggregated lineitem(part,supp)]
         ├─ FILTER part [p_name LIKE 'linen%'] → bitmap
         ├─ HASH_GROUP_BY lineitem [part,supp] sum(l_quantity) with date filter
         │  └─ SCAN lineitem [AVX2 date filter + flat open-addressing agg]
         └─ FILTER partsupp [ps_availqty*200 > sum_qty]
```

**Execution analysis**

- Linen part set is a cache-resident bitmap.
- Aggregation uses flat open-addressed hash table keyed by `(part,supp)` pair.
- Partsupp probe is selective and short-circuits when no lineitem aggregate exists.

### q21

**SQL**: count per Russian supplier of orders where that supplier is the sole late supplier on an F-order with other suppliers.

```text
OUTPUT result21.csv
└─ ORDER_BY [numwait DESC, s_name]
   └─ HASH_GROUP_BY [s_name] count(*)
      └─ FILTER per-order conditions [EXISTS other supplier AND NOT EXISTS other late supplier]
         └─ SCAN lineitem grouped by orderkey
            ├─ FILTER orders [o_orderstatus='F']
            ├─ per-order min/max supplier and late-supplier reduction
            └─ FILTER supplier nation [RUSSIA bitmap]
```

**Execution analysis**

- Fast path leverages both lineitem/orders sorted by orderkey and run-aligned walk.
- AVX2 run-boundary detection reduces branchy key-change checks.
- Per-order logic is branch-light min/max reduction, then dense suppkey counters.

### q22

**SQL**: per-country-code customer counts and balances above scalar avg, excluding customers with orders.

```text
OUTPUT result22.csv
└─ ORDER_BY [cntrycode]
   └─ HASH_GROUP_BY [cntrycode] [count,sum(c_acctbal)]
      └─ FILTER customer
         ├─ FILTER phone prefix in {13,17,31,23,18,29,30} (lookup table)
         ├─ FILTER c_acctbal > scalar avg over positive target-prefix accounts
         └─ ANTI_JOIN orders [cust has no orders] via custkey bitmap
```

**Execution analysis**

- Orders anti-join is a dense bitset (`has_orders`) indexed by custkey.
- Customer scan is branchless/masked accumulation for avg and candidate collection.
- Final grouping is flat 7-slot aggregation by dense code slot (no map).

---

## Per-query optimisation paths

### q1

- **Baseline** (`optim/pre-q1`, commit `0d4b329`): scalar/map-heavy baseline from base_impl+trace instrumentation.
- **sample_plan** (`d7ef344` → `28a8ed9`): **kept** — perfect-hash flat array group-by (`q1-perfecthash`).
- **trace** (`28a8ed9` → `7413496`): **kept** — switched to int64 accumulators, removed redundant double sums.
- **expert_knowledge** (`7413496` → `8b94beb`): **kept** — AVX2 vectorized scan then 8-wide/M.L.P. enhancement.
- **human_reference** (`8b94beb` → `2ec649f`): **rolled back** — no committed source change survived.

### q2

- **Baseline** (`optim/pre-q2`, commit `e87bb08`): early optimized-but-general baseline.
- **sample_plan** (`de5d762` → `abe394f`): **kept** — dense two-pass join replacing per-key hashmap.
- **trace** (`abe394f` → `6ea5d0c`): **kept** — single-pass partsupp probe + candidate staging.
- **expert_knowledge** (`6ea5d0c` → `ce2b734`): **kept** — output path reworked to buffered/prefetched string gathers.
- **human_reference** (`ce2b734` → `dbc466d`): **rolled back** — no surviving commit.

### q3

- **Baseline** (`optim/pre-q3`, commit `e756dbb`): baseline join/agg with heavier per-row overhead.
- **sample_plan** (`cd78629` → `61307b6`): **kept** — bitmap gates, fast output path, customer bitmap.
- **trace** (`61307b6` → `fd6c45e`): **kept** — run-length aggregation and removed unused structures.
- **expert_knowledge** (`fd6c45e` → `72f87b3`): **kept** — fast segment match (`BUILDING`) via cheap checks.
- **human_reference** (`72f87b3` → `dee570d`): **kept** — AVX2 decoupled probe/aggregate pipeline.

### q4

- **Baseline** (`optim/pre-q4`, commit `b9fb580`): date filter + semijoin with less decoupling.
- **sample_plan** (`512c6ba` → `0f3ffbd`): **kept** — fused single-pass date+CSR semijoin path.
- **trace** (`0f3ffbd` → `7f3e24b`): **kept** — collect-then-probe with tuned prefetch, packed range array.
- **expert_knowledge** (`7f3e24b` → `d3c228a`): **kept** — AVX2 left-pack date filter + decoupled probe.
- **human_reference** (`d3c228a` → `cdaa409`): **rolled back** — attempted branchless variant not retained.

### q5

- **Baseline** (`optim/pre-q5`, commit `6bf5c4e`): broader-scan style join path.
- **sample_plan** (`6fefe10` → `fe9108d`): **kept** — CSR-driven orders→lineitem join order.
- **trace** (`fe9108d` → `a39e57e`): **rolled back** — no committed change in this stage.
- **expert_knowledge** (`a39e57e` → `cf099ef`): **kept** — phased AVX2 + decoupled MLP gather pipeline.
- **human_reference** (`cf099ef` → `925f6a6`): **kept** — removed scratch zero-init (`new[]` buffers).

### q6

- **Baseline** (`optim/pre-q6`, commit `9065577`): scalar filter-and-sum baseline.
- **sample_plan** (`65c14d4` → `990fd73`): **kept** — fully vectorized AVX2 branchless masked aggregation.
- **trace** (`990fd73` → `e0a4626`): **rolled back** — tested alternatives but reverted to sample-plan commit.
- **expert_knowledge** (`e0a4626` → `f0ced8a`): **rolled back** — no net source delta.
- **human_reference** (`f0ced8a` → `f921a63`): **rolled back** — no surviving change.

### q7

- **Baseline** (`optim/pre-q7`, commit `ca1fb55`): baseline multi-join with heavier random access.
- **sample_plan** (`14ba16f` → `a5c62d8`): **kept** — merge-join lineitem↔orders + array aggregation.
- **trace** (`a5c62d8` → `a5c417f`): **kept** — branchless date/supplier prefilter into compact buffer.
- **expert_knowledge** (`a5c417f` → `07322d2`): **kept** — dropped unnecessary orderkey stream in phase-1.
- **human_reference** (`07322d2` → `c31925a`): **kept** — split date/supplier passes + sparse prefetch tuning.

### q8

- **Baseline** (`optim/pre-q8`, commit `2769992`): baseline join chain with higher probe costs.
- **sample_plan** (`a4bdc64` → `893fa77`): **kept** — bit-packed part/supplier/order maps and fixed 2-bucket agg.
- **trace** (`893fa77` → `91dc3da`): **kept** — L2 customer bitmap + prefetch tuning.
- **expert_knowledge** (`91dc3da` → `7b88fb9`): **kept** — branchless orders join + cheap part type checks.
- **human_reference** (`7b88fb9` → `f4d0f84`): **kept** — decoupled 3-phase lineitem probe pipeline.

### q9

- **Baseline** (`optim/pre-q9`, commit `0de51c1`): baseline multi-join with broader scans.
- **sample_plan** (`1e27170` → `adff9d3`): **kept** — bitmap part filter + compact partsupp + dense nation/year agg.
- **trace** (`adff9d3` → `3b8fb56`): **kept** — branchless left-pack + decoupled survivor pipeline.
- **expert_knowledge** (`3b8fb56` → `fdda032`): **rolled back** — candidate changes were checked out.
- **human_reference** (`fdda032` → `07ed2df`): **rolled back** — no final commit beyond existing best.

### q10

- **Baseline** (`optim/pre-q10`, commit `264545b`): baseline customer revenue aggregation.
- **sample_plan** (`a89cf48` → `8a84409`): **kept** — orders-driven CSR + dense per-customer aggregation.
- **trace** (`8a84409` → `5a37d9a`): **kept** — multiple output-path micro-opts incl two-pass fragment serialization.
- **expert_knowledge** (`5a37d9a` → `218e659`): **kept** — LSD radix sort by revenue.
- **human_reference** (`218e659` → `2d3b7f2`): **rolled back** — no additional source change kept.

### q11

- **Baseline** (`optim/pre-q11`, commit `36b8457`): baseline grouped value computation.
- **sample_plan** (`aef7ac7` → `88c7918`): **kept** — dense supplier bitmap + dense part accumulator.
- **trace** (`88c7918` → `f2d32a4`): **kept** — int64 accumulators + prefetch on sparse streams.
- **expert_knowledge** (`f2d32a4` → `c319ffa`): **rolled back** — no committed technique survived.
- **human_reference** (`c319ffa` → `04db01f`): **kept** — streaming sorted-input group-by replacing dense scatter.

### q12

- **Baseline** (`optim/pre-q12`, commit `70f9738`): baseline join/filter with string checks in hot path.
- **sample_plan** (`dd42dc0` → `0970280`): **kept** — reordered filters and fixed MAIL/FOB accumulators.
- **trace** (`0970280` → `e6a63f6`): **kept** — two-pass branchless filter evolution, prefetch chain, AVX2 left-pack.
- **expert_knowledge** (`e6a63f6` → `4ca924b`): **kept** — first-char shipmode/priority discrimination + hint tuning.
- **human_reference** (`4ca924b` → `1802674`): **kept** — split pass-2 into monotonic gather/probe loops.

### q13

- **Baseline** (`optim/pre-q13`, commit `6d8ba70`): baseline LIKE filter + histogram.
- **sample_plan** (`6f5b674` → `9c2b054`): **kept** — rare-anchor LIKE matcher + comment prefetch.
- **trace** (`9c2b054` → `29c9d11`): **rolled back** — no committed delta (checked out to sample best).
- **expert_knowledge** (`29c9d11` → `09cd238`): **rolled back** — no surviving change.
- **human_reference** (`09cd238` → `e582063`): **rolled back** — no surviving change.

### q14

- **Baseline** (`optim/pre-q14`, commit `ee72ce8`): baseline promo-revenue join/scan.
- **sample_plan** (`e7b3b16` → `a5f87de`): **kept** — dense promo byte build-side and branchless probe-agg.
- **trace** (`a5f87de` → `7e662ef`): **kept** — AVX2 date fast-reject + pipelined gather.
- **expert_knowledge** (`7e662ef` → `b511427`): **rolled back** — no retained expert-stage commit.
- **human_reference** (`b511427` → `37384e9`): **rolled back** — no retained human-stage commit.

### q15

- **Baseline** (`optim/pre-q15`, commit `7b4e780`): baseline CTE group-by + max filter.
- **sample_plan** (`34a5ad7` → `b99015f`): **kept** — dense suppkey array group-by.
- **trace** (`b99015f` → `cafc464`): **kept** — tiled AVX2 left-pack + scatter aggregate.
- **expert_knowledge** (`cafc464` → `e0fa3d2`): **rolled back** — no expert-stage code commit.
- **human_reference** (`e0fa3d2` → `5df215c`): **rolled back** — no human-stage code commit.

### q16

- **Baseline** (`optim/pre-q16`, commit `b6b4684`): baseline distinct supplier counting by part groups.
- **sample_plan** (`392e3ee` → `f8ede3b`): **kept** — dense gid join + sort/distinct strategy.
- **trace** (`f8ede3b` → `d7c091f`): **kept** — counting-sort distinct + packed-key grouping.
- **expert_knowledge** (`d7c091f` → `bb01c78`): **kept** — prefetch part scan + integer-key sort path.
- **human_reference** (`bb01c78` → `c09a982`): **rolled back** — no net source delta.

### q17

- **Baseline** (`optim/pre-q17`, commit `b3a4ceb`): baseline correlated subquery implementation.
- **sample_plan** (`4f2b624` → `8acdcad`): **kept** — dense partkey map + survivor materialization.
- **trace** (`8acdcad` → `e682975`): **kept** — L2-resident bitmap gate for full scan.
- **expert_knowledge** (`e682975` → `f2fd083`): **rolled back** — expert candidate reverted.
- **human_reference** (`f2fd083` → `498d403`): **kept** — word-compare part filter replacing string ops.

### q18

- **Baseline** (`optim/pre-q18`, commit `a9d1b68`): baseline big-order detection then joins.
- **sample_plan** (`2ed3fef` → `dbd90c0`): **kept** — dense order-indexed quantity aggregation.
- **trace** (`dbd90c0` → `90f8e0d`): **kept** — AVX2-gated big-order scan.
- **expert_knowledge** (`90f8e0d` → `9ef94eb`): **rolled back** — no source commit retained.
- **human_reference** (`9ef94eb` → `1e8ef4b`): **rolled back** — no source commit retained.

### q19

- **Baseline** (`optim/pre-q19`, commit `4cf6e06`): baseline OR-heavy predicate evaluation.
- **sample_plan** (`7d6322a` → `d8923c5`): **kept** — dictionary-encoded lineitem string columns.
- **trace** (`d8923c5` → `938e4e3`): **kept** — query-time part-group build + branchless probe.
- **expert_knowledge** (`938e4e3` → `a1687b6`): **kept** — word-compare part build + AVX2 pass-1 dictionary filter.
- **human_reference** (`a1687b6` → `28d85ff`): **rolled back** — no retained human-stage commit.

### q20

- **Baseline** (`optim/pre-q20`, commit `a0d9583`): baseline nested subquery style with heavier lookups.
- **sample_plan** (`9a207a2` → `195be15`): **kept** — linen-part bitmap instead of hash-set probes.
- **trace** (`195be15` → `91b4d83`): **kept** — AVX2 date filter + flat open-addressing aggregation.
- **expert_knowledge** (`91b4d83` → `e63cd84`): **rolled back** — no expert-stage commit survived.
- **human_reference** (`e63cd84` → `ec4af44`): **rolled back** — no human-stage commit survived.

### q21

- **Baseline** (`optim/pre-q21`, commit `e6712e0`): baseline EXISTS/NOT EXISTS with heavier per-order state.
- **sample_plan** (`bca1985` → `4626ff4`): **kept** — dense per-order arrays replacing hash-map.
- **trace** (`4626ff4` → `40018bc`): **kept** — orderkey merge-walk + branchless min/max reduction.
- **expert_knowledge** (`40018bc` → `c78a31c`): **kept** — AVX2 run-boundary scan + counted reduction.
- **human_reference** (`c78a31c` → `7c9f533`): **kept** — dropped o_orderkey read via run↔order alignment.

### q22

- **Baseline** (`optim/pre-q22`, commit `fec15ae`): baseline anti-join/group-by for country-code query.
- **sample_plan** (`a06e825` → `08a6f69`): **kept** — flat custkey anti-join + branchless prefix filter.
- **trace** (`08a6f69` → `79f477c`): **kept** — L2 orders bitmap, flat 7-slot group-by, pointer hoisting.
- **expert_knowledge** (`79f477c` → `f3de6dc`): **kept** — branchless customer scan with masked accumulation.
- **human_reference** (`f3de6dc` → `c9b61d3`): **rolled back** — no surviving human-stage source change.

---

## Techniques that paid off

1. **Dense array/bitmap replacements for hash/map state** (`q1_impl.hpp`, `q3_impl.hpp`, `q8_impl.hpp`, `q11_impl.hpp`, `q16_impl.hpp`, `q22_impl.hpp`): cut branch/hash overhead and improved cache residency.
2. **AVX2 left-pack + masked scan kernels** (`q1_impl.hpp`, `q4_impl.hpp`, `q6_impl.hpp`, `q12_impl.hpp`, `q14_impl.hpp`, `q15_impl.hpp`, `q19_impl.hpp`, `q21_impl.hpp`): made selective filters fast without branch mispredict penalties.
3. **CSR/orderkey direct addressing for orders↔lineitem joins** (`builder_impl.cpp` `orderkey_lineitem_range`, used in q4/q5/q10): avoided repeated full lineitem scans.
4. **Decoupled multi-phase probe pipelines with software prefetch** (`q4_impl.hpp`, `q5_impl.hpp`, `q8_impl.hpp`, `q9_impl.hpp`, `q10_impl.hpp`, `q12_impl.hpp`): increased memory-level parallelism for random gathers.
5. **Output-path engineering in string-heavy queries** (`q2_impl.hpp`, `q10_impl.hpp`, `q16_impl.hpp`): single-buffer writes, custom quoting, and sorted-fragment stitching reduced tail latency.
6. **Sort-path specialization** (`q10_impl.hpp` radix sort, `q16_impl.hpp` integer-key sort): replaced expensive comparison-based/string sort paths.
7. **Predicate hoisting/build-side tagging** (`q14_impl.hpp`, `q19_impl.hpp`): moved string comparisons off the fact-table hot loops.
8. **Streaming group-by on sorted inputs** (`q11_impl.hpp`, `q21_impl.hpp`): exploited physical order to avoid random-scatter aggregators.

---

## Techniques that were rolled back

- **q1 human_reference:** exploratory post-AVX2 alternatives were not committed; no source changes were committed.
- **q2 human_reference:** no surviving source change.
- **q4 human_reference:** attempted alternative after AVX2 path, reverted via checkout; no committed delta.
- **q5 trace:** stage produced no committed source changes.
- **q6 trace/expert_knowledge/human_reference:** all post-sample attempts were checked out; no net committed change.
- **q9 expert_knowledge + human_reference:** stage candidates were reverted to trace-best commit; no retained commit.
- **q10 human_reference:** no retained source commit.
- **q11 expert_knowledge:** no retained source commit.
- **q13 trace/expert_knowledge/human_reference:** no stage commit survived beyond sample_plan anchor-scan.
- **q14 expert_knowledge + human_reference:** no retained commit after trace AVX2 path.
- **q15 expert_knowledge + human_reference:** no stage commit retained.
- **q16 human_reference:** no retained source commit.
- **q17 expert_knowledge:** candidate reverted; no net source change.
- **q18 expert_knowledge + human_reference:** no retained source commit.
- **q19 human_reference:** no retained source commit.
- **q20 expert_knowledge + human_reference:** no retained source commit.
- **q22 human_reference:** no retained source commit.

---

## Lessons & ideas for the next run

1. **Add build-time persisted auxiliary structures actually used by final code** (e.g., prebuilt bitmaps for common predicates, compact surrogate columns) to cut repeated query-local setup cost.
2. **Introduce selective parallelism (intra-query chunk parallel scan) with deterministic merge** for the few scan-dominant queries still fully single-thread bound.
3. **Add auto-selection between CSR-driven and bitmap-driven join paths per query/cardinality** instead of static per-query heuristics.
4. **Unify date/predicate SIMD kernels into shared templates** to reduce duplicated AVX2 code and ease AVX-512 upgrades.
5. **Track stage-level perf deltas in metadata** (not just commit history) so rolled-back rationale is explicit and explain pass can attribute failed attempts quantitatively.
