#pragma once
// query_utils.hpp — CSV output helpers for query results.
//
// Use csv_quote() to escape any string value, and write_csv_row() / write_csv_header()
// to emit complete CSV lines.  The quoting convention matches the validator's CSV reader:
//   escapechar='\\', quotechar='"', doublequote=true
//
// If a field contains a comma, double-quote, newline, or backslash it is wrapped in
// double-quotes.  Internal double-quotes are doubled ("").  This is standard RFC 4180
// CSV with the additional convention that backslashes are treated as an escape character
// by the reader, so literal backslashes are also quoted.

#include <ostream>
#include <string>
#include <vector>
#include <initializer_list>
#include "trace.hpp"

// ---------------------------------------------------------------------------
// csv_quote — escape a single field value for CSV output
// ---------------------------------------------------------------------------

inline std::string csv_quote(const std::string& s) {
    bool needs_quote = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r' || c == '\\') {
            needs_quote = true;
            break;
        }
    }
    if (!needs_quote) return s;

    std::string out;
    out.reserve(s.size() + 4);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out.push_back('"');   // double the quote
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// Overload for numeric types — no quoting needed.
inline std::string csv_quote(int v)                { return std::to_string(v); }
inline std::string csv_quote(long v)               { return std::to_string(v); }
inline std::string csv_quote(long long v)          { return std::to_string(v); }
inline std::string csv_quote(unsigned v)           { return std::to_string(v); }
inline std::string csv_quote(unsigned long v)      { return std::to_string(v); }
inline std::string csv_quote(unsigned long long v) { return std::to_string(v); }

// ---------------------------------------------------------------------------
// Decimal formatting helpers
// ---------------------------------------------------------------------------

#include <iomanip>
#include <sstream>

/// Format a double with a fixed number of decimal places (default 2).
inline std::string fmt_decimal(double v, int precision = 2) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << v;
    return oss.str();
}

/// Format a fixed-point integer stored as value * 10^scale.
/// E.g. fmt_money(12345, 2) → "123.45"
inline std::string fmt_money(long long cents, int scale = 2) {
    bool neg = cents < 0;
    if (neg) cents = -cents;
    long long whole = cents;
    long long frac = 0;
    long long divisor = 1;
    for (int i = 0; i < scale; ++i) divisor *= 10;
    whole = cents / divisor;
    frac = cents % divisor;
    std::ostringstream oss;
    if (neg) oss << '-';
    oss << whole << '.' << std::setfill('0') << std::setw(scale) << frac;
    return oss.str();
}

// ---------------------------------------------------------------------------
// write_csv_header / write_csv_row — emit a full CSV line
// ---------------------------------------------------------------------------

/// Write a CSV header line.  Column names are not quoted (they should
/// be plain identifiers).
inline void write_csv_header(std::ostream& out,
                             std::initializer_list<const char*> cols) {
    bool first = true;
    for (auto c : cols) {
        if (!first) out << ',';
        out << c;
        first = false;
    }
    out << '\n';
}

/// Write one CSV data row.  Each value must already be formatted as a
/// string — use csv_quote() for string fields and std::to_string() /
/// fmt_decimal() for numerics.
inline void write_csv_row(std::ostream& out,
                          std::initializer_list<std::string> vals) {
    bool first = true;
    for (auto& v : vals) {
        if (!first) out << ',';
        out << v;
        first = false;
    }
    out << '\n';
}
