#include "query_impl.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <numeric>
#include <cstring>

#include "args_parser.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp"
#include "q2_impl.hpp"
#include "q3_impl.hpp"
#include "q4_impl.hpp"
#include "q5_impl.hpp"
#include "q6_impl.hpp"
#include "q7_impl.hpp"
#include "q8_impl.hpp"
#include "q9_impl.hpp"
#include "q10_impl.hpp"
#include "q11_impl.hpp"
#include "q12_impl.hpp"
#include "q13_impl.hpp"
#include "q14_impl.hpp"
#include "q15_impl.hpp"
#include "q16_impl.hpp"
#include "q17_impl.hpp"
#include "q18_impl.hpp"
#include "q19_impl.hpp"
#include "q20_impl.hpp"
#include "q21_impl.hpp"
#include "q22_impl.hpp"

// Forward declarations for each query
// q1 implemented in q1_impl.hpp
// q2 implemented in q2_impl.hpp
// q3 implemented in q3_impl.hpp
// q4 implemented in q4_impl.hpp
// q5 implemented in q5_impl.hpp
// q6 implemented in q6_impl.hpp
// q7 implemented in q7_impl.hpp
// q8 implemented in q8_impl.hpp
// q9 implemented in q9_impl.hpp
// q10 implemented in q10_impl.hpp
// q11 implemented in q11_impl.hpp
// q12 implemented in q12_impl.hpp
// q13 implemented in q13_impl.hpp
// q14 implemented in q14_impl.hpp
// q15 implemented in q15_impl.hpp
// q16 implemented in q16_impl.hpp
// q17 implemented in q17_impl.hpp
// q18 implemented in q18_impl.hpp
// q19 implemented in q19_impl.hpp
// q20 implemented in q20_impl.hpp
// q21 implemented in q21_impl.hpp
// q22 implemented in q22_impl.hpp

void query(Database* db) {
    std::vector<QueryRequest> requests;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            break;
        }
        std::istringstream iss(line);
        std::string query_id = "0";
        iss >> query_id;
        if (!iss) {
            continue;
        }
        requests.push_back(QueryRequest{query_id, line});
    }

    int run_nr = 0;
    for (auto& req : requests) {
        run_nr++;
        std::string rn = std::to_string(run_nr);
        std::cout << "--- query " << req.id << " ---" << std::endl;

        if (req.id == "q1") run_q1(db, rn);
        else if (req.id == "q2") run_q2(db, rn);
        else if (req.id == "q3") run_q3(db, rn);
        else if (req.id == "q4") run_q4(db, rn);
        else if (req.id == "q5") run_q5(db, rn);
        else if (req.id == "q6") run_q6(db, rn);
        else if (req.id == "q7") run_q7(db, rn);
        else if (req.id == "q8") run_q8(db, rn);
        else if (req.id == "q9") run_q9(db, rn);
        else if (req.id == "q10") run_q10(db, rn);
        else if (req.id == "q11") run_q11(db, rn);
        else if (req.id == "q12") run_q12(db, rn);
        else if (req.id == "q13") run_q13(db, rn);
        else if (req.id == "q14") run_q14(db, rn);
        else if (req.id == "q15") run_q15(db, rn);
        else if (req.id == "q16") run_q16(db, rn);
        else if (req.id == "q17") run_q17(db, rn);
        else if (req.id == "q18") run_q18(db, rn);
        else if (req.id == "q19") run_q19(db, rn);
        else if (req.id == "q20") run_q20(db, rn);
        else if (req.id == "q21") run_q21(db, rn);
        else if (req.id == "q22") run_q22(db, rn);
        else {
            std::cerr << "Unknown query: " << req.id << std::endl;
        }
    }
}

// Helper: write content to both file and stdout
static void output_result(const std::string& run_nr, const std::string& content) {
    std::string filename = "result" + run_nr + ".csv";
    std::ofstream out(filename);
    out << content;
    out.close();
    std::cout << content;
}

// ==================== STUB IMPLEMENTATIONS ====================

// All queries implemented in separate headers