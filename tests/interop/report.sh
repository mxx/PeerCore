#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARTIFACT_ROOT="${1:-$ROOT_DIR/tests/interop/artifacts}"
REPORT_FILE="${2:-$ARTIFACT_ROOT/report.md}"

mkdir -p "$ARTIFACT_ROOT"

extract_field() {
  local file="$1"
  local key="$2"
  sed -n "s/^${key}: //p" "$file" | tail -n1
}

{
  echo "# Interop Report"
  echo
  echo "| Case | Classification | Stage | Detail |"
  echo "| --- | --- | --- | --- |"

  found_any=0
  for summary in "$ARTIFACT_ROOT"/*/summary.txt; do
    [[ -f "$summary" ]] || continue
    found_any=1
    case_name="$(extract_field "$summary" "case")"
    classification="$(extract_field "$summary" "classification")"
    stage="$(extract_field "$summary" "stage")"
    detail="$(extract_field "$summary" "detail")"
    detail="${detail//|/\\|}"
    echo "| ${case_name} | ${classification} | ${stage} | ${detail} |"
  done

  if [[ "$found_any" -eq 0 ]]; then
    echo "| none | N/A | N/A | no summary files found |"
  fi

  echo
  echo "## Per-case Flags"
  echo

  for summary in "$ARTIFACT_ROOT"/*/summary.txt; do
    [[ -f "$summary" ]] || continue
    case_name="$(extract_field "$summary" "case")"
    echo "### ${case_name}"
    while IFS= read -r line; do
      case "$line" in
        case:*|classification:*|stage:*|detail:*)
          ;;
        *)
          key="${line%%:*}"
          value="${line#*: }"
          [[ -n "$key" ]] && echo "- \`${key}\`: ${value}"
          ;;
      esac
    done <"$summary"
    echo
  done
} >"$REPORT_FILE"

echo "wrote ${REPORT_FILE}"
