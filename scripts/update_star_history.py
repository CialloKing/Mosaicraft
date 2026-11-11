#!/usr/bin/env python3
"""Record GitHub Star counts and render the chart for the data branch."""

import argparse
from collections import Counter
import csv
from datetime import date, datetime, timedelta, timezone
from html import escape
from itertools import count
import json
import os
from pathlib import Path
from urllib.request import Request, urlopen


CSV_HEADER = ["date", "stars", "source", "observed_at"]
BEIJING = timezone(timedelta(hours=8))
DAY = timedelta(days=1)


def fetch_json(path, media_type="application/vnd.github+json"):
    headers = {"Accept": media_type, "X-GitHub-Api-Version": "2026-03-10"}
    if token := os.environ.get("GITHUB_TOKEN"):
        headers["Authorization"] = f"Bearer {token}"
    request = Request(f"https://api.github.com{path}", headers=headers)
    with urlopen(request, timeout=20) as response:
        return json.load(response)


def current_stargazer_days(repository):
    starred = {}
    for page in count(1):
        entries = fetch_json(f"/repos/{repository}/stargazers?per_page=100&page={page}",
                             "application/vnd.github.star+json")
        if not isinstance(entries, list):
            raise ValueError("Unexpected GitHub stargazers response")
        for entry in entries:
            timestamp = datetime.fromisoformat(entry["starred_at"])
            starred[entry["user"]["id"]] = timestamp.astimezone(BEIJING).date()
        if len(entries) < 100:
            return Counter(starred.values())


def reconstruct_history(stargazer_days, today):
    # Only stars that still exist are visible to the API. Today's real snapshot replaces the estimate.
    rows = []
    stars = 0
    day = min(stargazer_days, default=today)
    while day < today:
        stars += stargazer_days[day]
        rows.append((day, stars, "reconstructed", ""))
        day += DAY
    return rows


def read_history(csv_path):
    with csv_path.open(encoding="utf-8", newline="") as stream:
        reader = csv.reader(stream)
        if next(reader, None) != CSV_HEADER:
            raise ValueError("Invalid Star history CSV header")
        rows = []
        for day, stars, source, observed_at in reader:
            row = (date.fromisoformat(day), int(stars), source, observed_at)
            if (row[1] < 0 or source not in ("observed", "reconstructed") or
                    (rows and row[0] <= rows[-1][0])):
                raise ValueError(f"Invalid Star history record for {day}")
            rows.append(row)
        return rows


def render_svg(rows, repository):
    first_day, last_day = rows[0][0], rows[-1][0]
    latest = rows[-1][1]
    span = (last_day - first_day).days
    left, right, top, bottom = 70, 690, 64, 302
    peak = max(stars for _, stars, _, _ in rows)
    step = max(1, (peak + 4) // 5)
    ceiling = max(step, ((peak + step - 1) // step) * step)

    def x(day_offset):
        return left + (right - left) * (day_offset / span if span else 0.5)

    def y(stars):
        return bottom - (bottom - top) * (stars / ceiling)

    title = escape(f"{repository} Star History")
    chart = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="960" height="480" viewBox="0 0 720 360" role="img" aria-labelledby="title desc">',
        f'<title id="title">{title}</title>',
        f'<desc id="desc">{latest} Stars as of {last_day}.</desc>',
        '<rect x="0.5" y="0.5" width="719" height="359" rx="8" fill="#ffffff" stroke="#d0d7de"/>',
        '<g font-family="system-ui, sans-serif" font-size="18" fill="#57606a">',
        f'<text x="{left}" y="38" font-size="24" font-weight="600" fill="#24292f">{title}</text>',
        f'<text x="{right}" y="38" text-anchor="end" font-size="20" font-weight="600" fill="#24292f">{latest} Stars</text>',
    ]
    for stars in range(0, ceiling + 1, step):
        chart.append(f'<path d="M{left} {y(stars):.2f} H{right}" stroke="#d8dee4"/>')
        chart.append(f'<text x="{left - 12}" y="{y(stars) + 6:.2f}" text-anchor="end">{stars}</text>')

    ticks = min(3, span)
    offsets = [round(index * span / ticks) for index in range(ticks + 1)] if ticks else [0]
    for offset in offsets:
        anchor = "middle"
        if span and offset in (0, span):
            anchor = "start" if offset == 0 else "end"
        day = first_day + DAY * offset
        chart.append(f'<text x="{x(offset):.2f}" y="338" text-anchor="{anchor}">{day}</text>')
    chart.append('</g>')

    if len(rows) > 1:
        points = " ".join(f"{x((day - first_day).days):.2f},{y(stars):.2f}" for day, stars, _, _ in rows)
        chart.append(f'<polyline points="{points}" fill="none" stroke="#0969da" stroke-width="3" stroke-linejoin="round"/>')
    chart.append(f'<circle cx="{x(span):.2f}" cy="{y(latest):.2f}" r="4.5" fill="#0969da" stroke="#fff" stroke-width="2"/>')
    chart.append('</svg>')
    return "\n".join(chart) + "\n"


def update_history(data_dir, repository, now=None):
    csv_path = data_dir / "stars.csv"
    rows = read_history(csv_path) if csv_path.is_file() else None
    stargazer_days = current_stargazer_days(repository) if rows is None else None
    stars = fetch_json(f"/repos/{repository}")["stargazers_count"]
    if type(stars) is not int or stars < 0:
        raise ValueError("Invalid GitHub Star count")

    now = now or datetime.now(timezone.utc)
    today = now.astimezone(BEIJING).date()
    if rows is None:
        rows = reconstruct_history(stargazer_days, today)
    if rows and rows[-1][0] > today:
        raise ValueError("Observation date precedes the latest Star record")
    observed_at = now.isoformat(timespec="seconds")
    if rows and rows[-1][0] == today:
        # Repeated runs with the same result should leave the timestamp and Git tree unchanged.
        previous = rows.pop()
        if previous[1:3] == (stars, "observed"):
            observed_at = previous[3]
    rows.append((today, stars, "observed", observed_at))

    svg = render_svg(rows, repository)
    with csv_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(CSV_HEADER)
        writer.writerows(rows)
    (data_dir / "star-history.svg").write_text(svg, encoding="utf-8", newline="\n")
    (data_dir / "README.md").write_text(
        f"# {repository} Star history\n\n"
        "GitHub Actions records one Star count per Asia/Shanghai calendar day.\n\n"
        "- `stars.csv` stores the counts. `reconstructed` rows are based on current stargazer timestamps. "
        "`observed` rows are snapshots of the repository count.\n"
        "- `star-history.svg` shows the recorded history.\n",
        encoding="utf-8", newline="\n",
    )
    return today, stars


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("data_dir", type=Path)
    args = parser.parse_args()
    day, stars = update_history(args.data_dir, os.environ["GITHUB_REPOSITORY"])
    print(f"{day}: {stars} Stars")
