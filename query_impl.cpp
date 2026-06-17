#include "query_impl.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <functional>

#include "args_parser.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp"
#include "q2_impl.hpp"

// Forward declarations for each query
static void run_q1(Database* db, const std::string& args, std::ostream& out);
static void run_q2(Database* db, const std::string& args, std::ostream& out);
static void run_q3(Database* db, const std::string& args, std::ostream& out);
static void run_q4(Database* db, const std::string& args, std::ostream& out);
static void run_q5(Database* db, const std::string& args, std::ostream& out);
static void run_q6(Database* db, const std::string& args, std::ostream& out);
static void run_q7(Database* db, const std::string& args, std::ostream& out);
static void run_q8(Database* db, const std::string& args, std::ostream& out);
static void run_q9(Database* db, const std::string& args, std::ostream& out);
static void run_q10(Database* db, const std::string& args, std::ostream& out);
static void run_q11(Database* db, const std::string& args, std::ostream& out);
static void run_q12(Database* db, const std::string& args, std::ostream& out);
static void run_q13(Database* db, const std::string& args, std::ostream& out);
static void run_q14(Database* db, const std::string& args, std::ostream& out);
static void run_q15(Database* db, const std::string& args, std::ostream& out);
static void run_q16(Database* db, const std::string& args, std::ostream& out);
static void run_q17(Database* db, const std::string& args, std::ostream& out);
static void run_q18(Database* db, const std::string& args, std::ostream& out);
static void run_q19(Database* db, const std::string& args, std::ostream& out);
static void run_q20(Database* db, const std::string& args, std::ostream& out);
static void run_q21(Database* db, const std::string& args, std::ostream& out);
static void run_q22(Database* db, const std::string& args, std::ostream& out);

using QueryFunc = void(*)(Database*, const std::string&, std::ostream&);

static QueryFunc get_query_func(const std::string& id) {
    if (id == "q1" || id == "1") return run_q1;
    if (id == "q2" || id == "2") return run_q2;
    if (id == "q3" || id == "3") return run_q3;
    if (id == "q4" || id == "4") return run_q4;
    if (id == "q5" || id == "5") return run_q5;
    if (id == "q6" || id == "6") return run_q6;
    if (id == "q7" || id == "7") return run_q7;
    if (id == "q8" || id == "8") return run_q8;
    if (id == "q9" || id == "9") return run_q9;
    if (id == "q10" || id == "10") return run_q10;
    if (id == "q11" || id == "11") return run_q11;
    if (id == "q12" || id == "12") return run_q12;
    if (id == "q13" || id == "13") return run_q13;
    if (id == "q14" || id == "14") return run_q14;
    if (id == "q15" || id == "15") return run_q15;
    if (id == "q16" || id == "16") return run_q16;
    if (id == "q17" || id == "17") return run_q17;
    if (id == "q18" || id == "18") return run_q18;
    if (id == "q19" || id == "19") return run_q19;
    if (id == "q20" || id == "20") return run_q20;
    if (id == "q21" || id == "21") return run_q21;
    if (id == "q22" || id == "22") return run_q22;
    return nullptr;
}

void query(Database* db) {
    std::vector<QueryRequest> requests;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) break;
        std::istringstream iss(line);
        std::string query_id;
        iss >> query_id;
        if (!iss) continue;
        requests.push_back(QueryRequest{query_id, line});
    }

    int run_nr = 0;
    for (auto& req : requests) {
        run_nr++;
        std::string filename = "result" + std::to_string(run_nr) + ".csv";
        std::ofstream fout(filename);

        // Print separator for validation
        std::cout << "--- query " << req.id << " ---" << std::endl;

        auto func = get_query_func(req.id);
        if (func) {
            // Write to file
            func(db, req.line, fout);
            fout.close();

            // Also output to stdout
            std::ifstream fin(filename);
            std::cout << fin.rdbuf();
        } else {
            fout.close();
            std::cout << "Unknown query: " << req.id << std::endl;
        }
    }
}

// ===== QUERY STUBS =====
// Each writes an empty CSV (header only) for now

static void run_q1(Database* db, const std::string& args, std::ostream& out) {
    run_q1_impl(db, out);
}

static void run_q2(Database* db, const std::string& args, std::ostream& out) {
    run_q2_impl(db, out);
}

static void run_q3(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"l_orderkey","revenue","o_orderdate","o_shippriority"});
}

static void run_q4(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"o_orderpriority","order_count"});
}

static void run_q5(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"n_name","revenue"});
}

static void run_q6(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"revenue"});
}

static void run_q7(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"supp_nation","cust_nation","l_year","revenue"});
}

static void run_q8(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"o_year","mkt_share"});
}

static void run_q9(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"nation","o_year","sum_profit"});
}

static void run_q10(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"c_custkey","c_name","revenue","c_acctbal","n_name","c_address","c_phone","c_comment"});
}

static void run_q11(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"ps_partkey","value"});
}

static void run_q12(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"l_shipmode","high_line_count","low_line_count"});
}

static void run_q13(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"c_count","custdist"});
}

static void run_q14(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"promo_revenue"});
}

static void run_q15(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"s_suppkey","s_name","s_address","s_phone","total_revenue"});
}

static void run_q16(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"p_brand","p_type","p_size","supplier_cnt"});
}

static void run_q17(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"avg_yearly"});
}

static void run_q18(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"c_name","c_custkey","o_orderkey","o_orderdate","o_totalprice","sum(l_quantity)"});
}

static void run_q19(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"revenue"});
}

static void run_q20(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"s_name","s_address"});
}

static void run_q21(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"s_name","numwait"});
}

static void run_q22(Database* db, const std::string& args, std::ostream& out) {
    write_csv_header(out, {"cntrycode","numcust","totacctbal"});
}