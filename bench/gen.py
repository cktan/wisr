#!/usr/bin/env python3
"""Write usertable.csv.gz: 10,000 users, the same every run.

Each row is id, lastname, firstname, street, city, state, zip,
phone. Lastnames are Last1 to Last1000 and states the 50 US
states, both uniform, so a lastname is about 10 rows and a
state about 200: q1.sql and q2.sql count them.
"""
import csv
import gzip
import io
import os
import random

ROWS = 10000
LASTNAMES = 1000

STATES = """AL AK AZ AR CA CO CT DE FL GA HI ID IL IN IA KS KY LA ME MD
MA MI MN MS MO MT NE NV NH NJ NM NY NC ND OH OK OR PA RI SC SD TN TX UT
VT VA WA WV WI WY""".split()


def main():
    """Generate the rows and write them, gzipped, beside this script."""
    rng = random.Random(42)
    buf = io.StringIO()
    out = csv.writer(buf, lineterminator="\n")
    for i in range(1, ROWS + 1):
        out.writerow([
            i,
            f"Last{rng.randint(1, LASTNAMES)}",
            f"First{rng.randint(1, 500)}",
            f"{rng.randint(1, 9999)} Street{rng.randint(1, 200)}",
            f"City{rng.randint(1, 300)}",
            rng.choice(STATES),
            f"{rng.randint(0, 99999):05d}",
            f"{rng.randint(200, 999)}-{rng.randint(200, 999)}-"
            f"{rng.randint(0, 9999):04d}",
        ])
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "usertable.csv.gz")
    # mtime=0, so the same rows give the same bytes.
    with open(path, "wb") as f, gzip.GzipFile(
            filename="", mode="wb", fileobj=f, mtime=0) as gz:
        gz.write(buf.getvalue().encode())


main()
