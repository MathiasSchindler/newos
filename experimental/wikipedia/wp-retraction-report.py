#!/usr/bin/env python3
"""Generate a static HTML review page for Wikipedia retraction matches."""

from __future__ import annotations

import argparse
import csv
import html
import re
from pathlib import Path
from urllib.parse import quote


CSS = """\
:root {
  color-scheme: light dark;
  --bg: #f8fafc;
  --fg: #0f172a;
  --muted: #475569;
  --panel: #ffffff;
  --border: #cbd5e1;
  --accent: #2563eb;
  --weak: #b45309;
  --hard: #047857;
}

* { box-sizing: border-box; }
body {
  margin: 0;
  background: var(--bg);
  color: var(--fg);
  font: 14px/1.45 system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
}
a { color: var(--accent); }
header, main { max-width: 1600px; margin: 0 auto; padding: 1rem; }
header { padding-top: 1.5rem; }
h1 { margin: 0 0 .25rem; font-size: 1.8rem; }
h2 { margin-top: 2rem; }
.meta { color: var(--muted); margin: .25rem 0; }
.controls {
  position: sticky;
  top: 0;
  z-index: 3;
  background: color-mix(in srgb, var(--bg) 92%, transparent);
  backdrop-filter: blur(6px);
  border-block: 1px solid var(--border);
  padding: .75rem 1rem;
}
.controls label { display: inline-flex; gap: .5rem; align-items: center; margin-right: 1rem; }
input[type="search"], select {
  min-width: 18rem;
  padding: .45rem .6rem;
  border: 1px solid var(--border);
  border-radius: .4rem;
  background: var(--panel);
  color: var(--fg);
}
.summary {
  display: flex;
  flex-wrap: wrap;
  gap: .75rem;
  margin: 1rem 0;
}
.card {
  background: var(--panel);
  border: 1px solid var(--border);
  border-radius: .7rem;
  padding: .75rem 1rem;
  min-width: 12rem;
}
.card strong { display: block; font-size: 1.3rem; }
.table-wrap {
  max-height: 75vh;
  overflow: auto;
  border: 1px solid var(--border);
  border-radius: .7rem;
  background: var(--panel);
}
table { border-collapse: collapse; width: 100%; min-width: 1200px; }
th, td {
  border-bottom: 1px solid var(--border);
  padding: .45rem .55rem;
  text-align: left;
  vertical-align: top;
}
th {
  position: sticky;
  top: 0;
  z-index: 2;
  background: var(--panel);
  white-space: nowrap;
  cursor: pointer;
}
th::after { content: " ↕"; color: var(--muted); font-weight: normal; }
th.sorted-asc::after { content: " ↑"; color: var(--accent); }
th.sorted-desc::after { content: " ↓"; color: var(--accent); }
tr[hidden] { display: none; }
.badge {
  display: inline-block;
  border-radius: 999px;
  padding: .1rem .45rem;
  font-size: .8rem;
  font-weight: 650;
  white-space: nowrap;
}
.badge-hard { background: color-mix(in srgb, var(--hard) 16%, transparent); color: var(--hard); }
.badge-weak { background: color-mix(in srgb, var(--weak) 18%, transparent); color: var(--weak); }
.raw {
  max-width: 40rem;
  max-height: 7rem;
  overflow: auto;
  white-space: pre-wrap;
  overflow-wrap: anywhere;
  color: var(--muted);
}
.nowrap { white-space: nowrap; }
.num { text-align: right; font-variant-numeric: tabular-nums; }
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #020617;
    --fg: #e2e8f0;
    --muted: #94a3b8;
    --panel: #0f172a;
    --border: #334155;
    --accent: #60a5fa;
    --weak: #fbbf24;
    --hard: #34d399;
  }
}
"""


JS = """\
(function () {
  function normalize(text) {
    return (text || "").toLocaleLowerCase();
  }

  function rowMatches(row, query, mode) {
    if (mode !== "all" && row.dataset.matchType !== mode) return false;
    if (!query) return true;
    return normalize(row.dataset.search || row.textContent).indexOf(query) !== -1;
  }

  function applyFilters() {
    const query = normalize(document.getElementById("filter").value.trim());
    const mode = document.getElementById("matchType").value;
    for (const row of document.querySelectorAll("tbody tr")) {
      row.hidden = !rowMatches(row, query, mode);
    }
    for (const table of document.querySelectorAll("table")) {
      const visible = Array.from(table.tBodies[0].rows).filter((row) => !row.hidden).length;
      const counter = document.querySelector(`[data-counter-for="${table.id}"]`);
      if (counter) counter.textContent = String(visible);
    }
  }

  function cellValue(row, index) {
    const cell = row.cells[index];
    if (!cell) return "";
    const sort = cell.dataset.sort;
    if (sort !== undefined) {
      const number = Number(sort);
      return Number.isNaN(number) ? sort : number;
    }
    const text = cell.textContent.trim();
    const number = Number(text);
    return text !== "" && !Number.isNaN(number) ? number : normalize(text);
  }

  function sortTable(table, index, th) {
    const body = table.tBodies[0];
    const direction = th.classList.contains("sorted-asc") ? -1 : 1;
    for (const other of table.querySelectorAll("th")) {
      other.classList.remove("sorted-asc", "sorted-desc");
    }
    th.classList.add(direction === 1 ? "sorted-asc" : "sorted-desc");
    const rows = Array.from(body.rows);
    rows.sort((a, b) => {
      const av = cellValue(a, index);
      const bv = cellValue(b, index);
      if (av < bv) return -direction;
      if (av > bv) return direction;
      return 0;
    });
    for (const row of rows) body.appendChild(row);
  }

  for (const th of document.querySelectorAll("th")) {
    th.addEventListener("click", () => sortTable(th.closest("table"), th.cellIndex, th));
  }
  document.getElementById("filter").addEventListener("input", applyFilters);
  document.getElementById("matchType").addEventListener("change", applyFilters);
  applyFilters();
})();
"""


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8", errors="replace") as handle:
        return list(csv.DictReader(handle, delimiter="\t"))


def discover_latest(data_dir: Path, suffix: str) -> Path:
    candidates = sorted(data_dir.glob(f"*wiki-????-??-??-{suffix}"))
    if not candidates:
        raise FileNotFoundError(f"no *wiki-YYYY-MM-DD-{suffix} in {data_dir}")
    return candidates[-1]


def h(text: str | None) -> str:
    return html.escape(text or "", quote=True)


def wiki_language(wiki: str) -> str:
    return wiki[:-4] if wiki.endswith("wiki") and len(wiki) > 4 else wiki


def wikipedia_url(row: dict[str, str]) -> str:
    lang = wiki_language(row.get("wiki", ""))
    page_id = row.get("page_id", "")
    if lang and page_id.isdigit():
        return f"https://{quote(lang)}.wikipedia.org/?curid={quote(page_id)}"
    title = row.get("page_title", "").replace(" ", "_")
    return f"https://{quote(lang or 'www')}.wikipedia.org/wiki/{quote(title)}"


def doi_url(doi: str) -> str:
    return f"https://doi.org/{quote(doi, safe='/:;().-_%')}"


def pmid_url(pmid: str) -> str:
    return f"https://pubmed.ncbi.nlm.nih.gov/{quote(pmid)}/"


def article_link(row: dict[str, str]) -> str:
    doi = (row.get("rw_original_doi") or "").strip()
    pmid = (row.get("rw_original_pmid") or "").strip()
    if doi:
        return f'<a href="{h(doi_url(doi))}">DOI {h(doi)}</a>'
    if pmid and pmid != "0":
        return f'<a href="{h(pmid_url(pmid))}">PMID {h(pmid)}</a>'
    return ""


def page_link(row: dict[str, str]) -> str:
    title = row.get("page_title") or row.get("page_id") or "Wikipedia page"
    return f'<a href="{h(wikipedia_url(row))}">{h(title)}</a>'


def signal_count(signals: str) -> int:
    return 0 if not signals else len([part for part in signals.split("+") if part])


def raw_snippet(row: dict[str, str]) -> str:
    raw = row.get("raw", "")
    return h(raw[:900] + ("..." if len(raw) > 900 else ""))


def search_text(row: dict[str, str], extra: str = "") -> str:
    keys = [
        "page_title",
        "match_kind",
        "match_value",
        "match_signals",
        "citation_kind",
        "citation_value",
        "rw_record_id",
        "rw_title",
        "rw_journal",
        "rw_author",
        "rw_publisher",
        "rw_reason",
        "rw_retraction_nature",
        "raw",
    ]
    return h(" ".join([extra] + [row.get(key, "") for key in keys]))


def exact_row(row: dict[str, str]) -> str:
    return f"""\
<tr data-match-type="exact" data-search="{search_text(row, 'exact')}">
  <td><span class="badge badge-hard">exact {h(row.get('match_kind'))}</span></td>
  <td>{page_link(row)}<br><span class="meta">page {h(row.get('page_id'))}</span></td>
  <td>{h(row.get('match_value'))}<br>{article_link(row)}</td>
  <td>{h(row.get('rw_title'))}</td>
  <td>{h(row.get('rw_journal'))}</td>
  <td>{h(row.get('rw_retraction_nature'))}</td>
  <td class="nowrap">{h(row.get('rw_retraction_date'))}</td>
  <td>{h(row.get('rw_reason'))}</td>
  <td>{h(row.get('citation_kind'))}</td>
  <td><div class="raw">{raw_snippet(row)}</div></td>
</tr>"""


def weak_row(row: dict[str, str]) -> str:
    signals = row.get("match_signals", "")
    count = signal_count(signals)
    return f"""\
<tr data-match-type="weak" data-search="{search_text(row, 'weak')}">
  <td><span class="badge badge-weak">weak</span></td>
  <td>{page_link(row)}<br><span class="meta">page {h(row.get('page_id'))}</span></td>
  <td data-sort="{count}" class="num">{count}</td>
  <td>{h(signals)}</td>
  <td>{article_link(row)}</td>
  <td>{h(row.get('rw_title'))}</td>
  <td>{h(row.get('rw_journal'))}</td>
  <td>{h(row.get('rw_author'))}</td>
  <td>{h(row.get('rw_retraction_nature'))}</td>
  <td>{h(row.get('rw_reason'))}</td>
  <td><div class="raw">{raw_snippet(row)}</div></td>
</tr>"""


def write_report(exact_path: Path, weak_path: Path, output_dir: Path) -> None:
    exact_rows = read_tsv(exact_path)
    weak_rows = read_tsv(weak_path)
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "retraction.css").write_text(CSS, encoding="utf-8")
    (output_dir / "retraction.js").write_text(JS, encoding="utf-8")

    html_text = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Wikipedia retraction matches</title>
  <link rel="stylesheet" href="retraction.css">
  <script defer src="retraction.js"></script>
</head>
<body>
<header>
  <h1>Wikipedia retraction matches</h1>
  <p class="meta">Exact source: <code>{h(str(exact_path))}</code></p>
  <p class="meta">Weak source: <code>{h(str(weak_path))}</code></p>
  <p class="meta">Weak rows have signal combinations, not a calibrated confidence score.</p>
</header>
<section class="controls">
  <label>Filter <input id="filter" type="search" placeholder="article, DOI, author, journal, reason..."></label>
  <label>Type
    <select id="matchType">
      <option value="all">all matches</option>
      <option value="exact">exact DOI/PMID</option>
      <option value="weak">weak signals</option>
    </select>
  </label>
</section>
<main>
  <section class="summary">
    <div class="card"><strong>{len(exact_rows)}</strong> exact DOI/PMID rows</div>
    <div class="card"><strong>{len(weak_rows)}</strong> weak rows</div>
    <div class="card"><strong data-counter-for="exact-table">{len(exact_rows)}</strong> visible exact rows</div>
    <div class="card"><strong data-counter-for="weak-table">{len(weak_rows)}</strong> visible weak rows</div>
  </section>

  <h2>Exact DOI/PMID matches</h2>
  <div class="table-wrap">
    <table id="exact-table">
      <thead><tr>
        <th>Type</th><th>Wikipedia</th><th>Published article</th><th>Retraction Watch title</th>
        <th>Journal</th><th>Nature</th><th>Retraction date</th><th>Reason</th><th>Citation kind</th><th>Raw evidence</th>
      </tr></thead>
      <tbody>
        {"".join(exact_row(row) for row in exact_rows)}
      </tbody>
    </table>
  </div>

  <h2>Weak title/context matches</h2>
  <div class="table-wrap">
    <table id="weak-table">
      <thead><tr>
        <th>Type</th><th>Wikipedia</th><th>Signal count</th><th>Signals</th><th>Published article</th>
        <th>Retraction Watch title</th><th>Journal</th><th>Author</th><th>Nature</th><th>Reason</th><th>Raw evidence</th>
      </tr></thead>
      <tbody>
        {"".join(weak_row(row) for row in weak_rows)}
      </tbody>
    </table>
  </div>
</main>
</body>
</html>
"""
    (output_dir / "index.html").write_text(html_text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", default="experimental/wikipedia/data", type=Path)
    parser.add_argument("--exact", type=Path)
    parser.add_argument("--weak", type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()

    data_dir = args.data_dir
    exact_path = args.exact or discover_latest(data_dir, "retraction-matches.tsv")
    weak_path = args.weak or discover_latest(data_dir, "weak-retraction-matches.tsv")
    output_dir = args.output_dir or data_dir / "retraction"
    write_report(exact_path, weak_path, output_dir)
    print(output_dir / "index.html")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
