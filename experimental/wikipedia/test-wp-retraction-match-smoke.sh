#!/bin/sh
set -eu

cd "$(dirname "$0")"

case "${1:-}" in
  --help|-h)
    echo "usage: sh ./test-wp-retraction-match-smoke.sh"
    exit 0
    ;;
esac

work_dir="build/wp-retraction-match-smoke"
rm -rf "$work_dir"
mkdir -p "$work_dir"

cat > "$work_dir/retractions.csv" <<'CSV'
Record ID,Title,Subject,Institution,Journal,Publisher,Country,Author,URLS,ArticleType,RetractionDate,RetractionDOI,RetractionPubMedID,OriginalPaperDate,OriginalPaperDOI,OriginalPaperPubMedID,RetractionNature,Reason,Paywalled,Notes,
1001,Alpha Beta Gamma Findings,Biology,Test Institute,Journal of Synthetic Results,Test Publisher,US,Smith Jane,,Research Article,2020-01-02,10.9999/retract,9001,2019-05-04,10.1111/exact,123456,Retraction,Error,false,,
CSV

cat > "$work_dir/dewiki-2026-06-20-citations.tsv" <<'TSV'
wiki	snapshot	source	page_id	page_title	kind	value	raw
dewiki	2026-06-20	synthetic	1	Exact Page	doi	10.1111/exact	{{Cite journal |doi=10.1111/exact}}
dewiki	2026-06-20	synthetic	1	Exact Page	doi	https://doi.org/10.1111/exact.	{{Cite journal |doi=https://doi.org/10.1111/exact.}}
dewiki	2026-06-20	synthetic	4	PMID Page	pmid	123456	{{Cite journal |pmid=123456}}
dewiki	2026-06-20	synthetic	4	PMID Page	pmid	123456 	<ref>PMID 123456</ref>
dewiki	2026-06-20	synthetic	2	Weak Page	template		{{Cite journal |title=Alpha Beta Gamma Findings |journal=Journal of Synthetic Results |date=2019 |last=Smith}}
dewiki	2026-06-20	synthetic	3	Title Only	template		{{Cite journal |title=Alpha Beta Gamma Findings}}
TSV

./build/wp-retraction-match -q \
  -r "$work_dir/retractions.csv" \
  -c "$work_dir/dewiki-2026-06-20-citations.tsv" \
  -o "$work_dir/hard.tsv" \
  -w "$work_dir/weak.tsv" \
  --article-dedup "$work_dir/article-dedup.tsv"

hard_rows=$(sed '1d' "$work_dir/hard.tsv" | wc -l | tr -d ' ')
weak_rows=$(sed '1d' "$work_dir/weak.tsv" | wc -l | tr -d ' ')
dedup_rows=$(sed '1d' "$work_dir/article-dedup.tsv" | wc -l | tr -d ' ')

if [ "$hard_rows" != "4" ]; then
  echo "expected 4 hard DOI/PMID rows, got $hard_rows" >&2
  exit 1
fi

if [ "$weak_rows" != "1" ]; then
  echo "expected 1 weak row, got $weak_rows" >&2
  exit 1
fi

if [ "$dedup_rows" != "2" ]; then
  echo "expected 2 article-deduplicated hard rows, got $dedup_rows" >&2
  exit 1
fi

if ! grep -q '^2	doi	10.1111/exact	OriginalPaperDOI	dewiki	2026-06-20	synthetic	1	Exact Page' "$work_dir/article-dedup.tsv"; then
  echo "article dedup output did not collapse duplicate DOI evidence" >&2
  exit 1
fi

if ! grep -q '^2	pmid	123456	OriginalPaperPubMedID	dewiki	2026-06-20	synthetic	4	PMID Page' "$work_dir/article-dedup.tsv"; then
  echo "article dedup output did not collapse duplicate PMID evidence" >&2
  exit 1
fi

if ! grep -q 'weak-title	Alpha Beta Gamma Findings	title+journal+year+author' "$work_dir/weak.tsv"; then
  echo "weak match did not include expected title+journal+year+author signals" >&2
  exit 1
fi

if grep -q 'Title Only' "$work_dir/weak.tsv"; then
  echo "title-only citation should not weak-match without an extra signal" >&2
  exit 1
fi

echo "wp-retraction-match smoke test passed"
