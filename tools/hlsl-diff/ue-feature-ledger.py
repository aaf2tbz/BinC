#!/usr/bin/env python3
"""Build a machine-readable UE first-error feature ledger from an audit report.

Usage:
    ue-feature-ledger.py <audit.md> [out.json] [provenance.json]

The ledger deliberately keeps every per-file row.  A bucket is only marked
closed by a later tool/user action; this generator records the audit facts and
leaves evidence, fix, and gate fields explicit rather than inferring them.
"""
import hashlib
import json
import os
import re
import sys
from datetime import datetime, timezone


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def file_identity(path):
    path = os.path.abspath(path)
    st = os.stat(path)
    return {
        "path": path,
        "size": st.st_size,
        "mtime_ns": st.st_mtime_ns,
        "sha256": sha256_file(path),
    }


def slug(value):
    value = re.sub(r"[^A-Za-z0-9]+", "-", value).strip("-").lower()
    return value or "unclassified"


def parse_results(lines):
    for i, line in enumerate(lines):
        if line.strip() != "## Results":
            continue
        results = {}
        for row in lines[i + 1:]:
            if row.startswith("## "):
                break
            m = re.match(r"^\|\s*([^|]+?)\s*\|\s*(\d+)\s*\|\s*$", row)
            if m:
                results[m.group(1)] = int(m.group(2))
        return results
    return {}


def parse_buckets(lines):
    for i, line in enumerate(lines):
        if line.strip() != "## Feature-gap buckets (first error)":
            continue
        buckets = []
        for row in lines[i + 1:]:
            if row.startswith("## "):
                break
            # Gap text may contain a literal '|'.  Greedy matching keeps the
            # final numeric column as the count and the final column as files.
            m = re.match(r"^\|\s*(.*)\s*\|\s*(\d+)\s*\|\s*(.*)\s*\|\s*$", row)
            if not m:
                continue
            name = m.group(1).strip()
            if name == "gap":
                continue
            buckets.append({
                "id": slug(name),
                "name": name,
                "count": int(m.group(2)),
                "sample_files": [x.strip() for x in m.group(3).split(",") if x.strip()],
                "status": "open",
                "evidence": [],
                "fix": None,
                "native_gate": None,
            })
        return buckets
    return []


def parse_rows(lines):
    for i, line in enumerate(lines):
        if line.strip() != "## Per-file rows":
            continue
        rows = []
        for row in lines[i + 1:]:
            if row.startswith("## "):
                break
            if not row.startswith("|") or row.startswith("|---") or row.startswith("| file "):
                continue
            fields = row.split(" | ", 3)
            if len(fields) != 4:
                continue
            rows.append({
                "file": fields[0].lstrip("| "),
                "profile": fields[1],
                "entry": fields[2],
                "result": fields[3].rstrip(" |").strip(),
            })
        return rows
    return []


def main():
    if len(sys.argv) not in (2, 3, 4):
        raise SystemExit("usage: ue-feature-ledger.py <audit.md> [out.json] [provenance.json]")
    report = os.path.abspath(sys.argv[1])
    out = os.path.abspath(sys.argv[2]) if len(sys.argv) >= 3 else os.path.splitext(report)[0] + ".ledger.json"
    provenance_path = (os.path.abspath(sys.argv[3]) if len(sys.argv) == 4 else
                       os.path.splitext(report)[0] + ".provenance.json")
    lines = open(report, encoding="utf-8", errors="replace").read().splitlines()
    results = parse_results(lines)
    buckets = parse_buckets(lines)
    rows = parse_rows(lines)
    by_bucket = {}
    for row in rows:
        result = row["result"]
        if not result.startswith("GAP:"):
            continue
        name = result[4:]
        by_bucket.setdefault(name, []).append(row)
    for bucket in buckets:
        bucket["rows"] = by_bucket.get(bucket["name"], [])
        bucket["row_count"] = len(bucket["rows"])
    provenance = None
    if os.path.exists(provenance_path):
        provenance = json.load(open(provenance_path, encoding="utf-8"))
    ledger = {
        "schema": 1,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "report": file_identity(report),
        "provenance_path": provenance_path if provenance is not None else None,
        "provenance": provenance,
        "summary": {
            "results": results,
            "queued": len(rows),
            "metallib_producing": results.get("COMPILES", 0),
            "gap": results.get("GAP", 0),
            "crashes": results.get("CRASH", 0),
            "hangs": results.get("HANG", 0),
            "bucket_count": len(buckets),
        },
        "buckets": buckets,
        "rows": rows,
    }
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(ledger, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"ue-feature-ledger: {len(rows)} rows, {len(buckets)} buckets -> {out}")
    print(f"  report sha256: {ledger['report']['sha256']}")
    print(f"  compiles: {ledger['summary']['metallib_producing']}")
    print(f"  gaps: {ledger['summary']['gap']}")
    print(f"  crashes: {ledger['summary']['crashes']}")
    print(f"  hangs: {ledger['summary']['hangs']}")


if __name__ == "__main__":
    main()
