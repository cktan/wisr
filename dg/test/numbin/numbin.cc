// numbin -- bind numerics as a driver does, in binary and typed 1700, and
// fetch them in binary (DG_DECIMAL_PLAN.md section 3.5), over libpq.
//
//   numbin CONNINFO
//
// Prints each step's result, which test/server.sh compares with what
// PostgreSQL's numeric gives; the exit status is 1 if it cannot connect.
#include <libpq-fe.h>

#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// numeric's oid, which a driver binds and a client is told.
const Oid numeric_oid = 1700;

// PostgreSQL's binary numeric: its digits in base 10000, most significant
// first, the weight of the first, the sign and the display scale.
std::string numeric_bytes(const std::vector<uint16_t> &digits, int weight,
                          bool neg, int dscale) {
  std::vector<uint16_t> w = {htons((uint16_t)digits.size()),
                             htons((uint16_t)weight),
                             htons(neg ? 0x4000 : 0), htons((uint16_t)dscale)};
  for (uint16_t d : digits)
    w.push_back(htons(d));
  return std::string((const char *)w.data(), 2 * w.size());
}

// A binary numeric's fields as text: digits, weight, sign and scale.
std::string numeric_text(const char *p, int len) {
  auto at = [&](int i) { return ntohs(*(const uint16_t *)(p + 2 * i)); };
  std::string s = "nd=" + std::to_string(at(0)) +
                  " w=" + std::to_string((int16_t)at(1)) +
                  (at(2) == 0x4000 ? " neg" : " pos") +
                  " dscale=" + std::to_string(at(3)) + " digits=";
  for (int i = 0; i < at(0) && 8 + 2 * i < len; i++)
    s += (i ? "," : "") + std::to_string(at(4 + i));
  return s;
}

// Run sql with params, each binary and typed 1700; print its status, or
// its error's message.
void bind(PGconn *c, const char *sql, const std::vector<std::string> &params) {
  std::vector<const char *> vals;
  std::vector<int> lens, fmts;
  std::vector<Oid> types;
  for (const std::string &p : params) {
    vals.push_back(p.data());
    lens.push_back((int)p.size());
    fmts.push_back(1);
    types.push_back(numeric_oid);
  }
  PGresult *r = PQexecParams(c, sql, (int)params.size(), types.data(),
                             vals.data(), lens.data(), fmts.data(), 0);
  printf("%s: %s\n", sql,
         PQresultStatus(r) == PGRES_COMMAND_OK
             ? PQcmdStatus(r)
             : PQresultErrorField(r, PG_DIAG_MESSAGE_PRIMARY));
  PQclear(r);
}

// Fetch sql's rows in binary; print each column's oid and binary numeric.
void fetch(PGconn *c, const char *sql) {
  PGresult *r = PQexecParams(c, sql, 0, NULL, NULL, NULL, NULL, 1);
  printf("%s:\n", sql);
  if (PQresultStatus(r) != PGRES_TUPLES_OK)
    printf("  %s", PQresultErrorMessage(r));
  for (int row = 0; row < PQntuples(r); row++)
    for (int i = 0; i < PQnfields(r); i++)
      printf("  %s: oid %u %s\n", PQfname(r, i), PQftype(r, i),
             PQgetisnull(r, row, i)
                 ? "null"
                 : numeric_text(PQgetvalue(r, row, i), PQgetlength(r, row, i))
                       .c_str());
  PQclear(r);
}

} // namespace

// Bind and fetch numerics on the server CONNINFO names.
int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: numbin CONNINFO\n");
    return 1;
  }
  PGconn *c = PQconnectdb(argv[1]);
  if (PQstatus(c) != CONNECTION_OK) {
    fprintf(stderr, "%s", PQerrorMessage(c));
    return 1;
  }
  PQclear(PQexec(c, "create table nb (a numeric(10,2), b numeric(30,5))"));
  // 12345.678: 1 2345 6780, weight 1, scale 3.
  std::string v = numeric_bytes({1, 2345, 6780}, 1, false, 3);
  // -12345678901234567890.5: 1234 5678 9012 3456 7890 5000, weight 4.
  std::string wide = numeric_bytes({1234, 5678, 9012, 3456, 7890, 5000}, 4,
                                   true, 1);
  // 1. A value of 8 digits into a decimal64 and a decimal128 column, each
  //    rounded to its scale.
  bind(c, "insert into nb values ($1, $2)", {v, v});
  // 2. One of 21 digits fits the decimal128 column, not the decimal64.
  bind(c, "insert into nb (b) values ($1)", {wide});
  bind(c, "insert into nb (a) values ($1)", {wide});
  // 3. A bound numeric compares with a column's, and computes.
  bind(c, "delete from nb where b = $1 * 2 - $1", {wide});
  // 4. Fetched in binary: oid 1700 and numeric's format for both decimals
  //    and for what is computed of them.
  fetch(c, "select a, b, a * b, sum(a) over (), -a from nb");
  PQclear(PQexec(c, "drop table nb"));
  PQfinish(c);
  return 0;
}
