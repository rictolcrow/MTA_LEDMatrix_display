#!/usr/bin/env python3
"""
Fetch + decode MTA GTFS-Realtime (NYCT Subway) feed and extract arrivals for:
  Route: A
  Stop:  A12N  (145 St, uptown/northbound platform)

Notes:
- GTFS-RT is protobuf; we decode using gtfs-realtime-bindings.
- Stop IDs: 145 St station is A12 (parent); platforms include A12N / A12S. :contentReference[oaicite:3]{index=3}
- If the feed requires a key, set env var: MTA_API_KEY=...
"""

import argparse
import json
import os
import sys
import time
from datetime import datetime, timezone
from typing import Optional, List, Dict, Any

import requests
from google.transit import gtfs_realtime_pb2
from google.protobuf.json_format import MessageToDict


FEED_URL_DEFAULT = "https://api-endpoint.mta.info/Dataservice/mtagtfsfeeds/nyct%2Fgtfs-ace"


def fetch_feed(url: str, api_key: Optional[str] = None, timeout_s: int = 30) -> bytes:
    headers = {
        "Accept": "application/x-protobuf",
        "User-Agent": "gtfsrt-nyct-decoder/1.0",
    }
    if api_key:
        headers["x-api-key"] = api_key

    resp = requests.get(url, headers=headers, timeout=timeout_s)
    if resp.status_code in (401, 403) and not api_key:
        raise RuntimeError(
            f"HTTP {resp.status_code}: this endpoint appears to require an API key. "
            f"Set MTA_API_KEY and retry."
        )
    resp.raise_for_status()
    return resp.content


def parse_feed(raw: bytes) -> gtfs_realtime_pb2.FeedMessage:
    feed = gtfs_realtime_pb2.FeedMessage()
    feed.ParseFromString(raw)
    return feed


def unix_to_local_str(ts: int, tz: timezone = timezone.utc) -> str:
    # Using local timezone conversion via datetime.fromtimestamp without specifying tz
    # will use the machine's local tz; that’s usually what you want for quick viewing.
    return datetime.fromtimestamp(ts).astimezone().strftime("%Y-%m-%d %H:%M:%S %Z")


def minutes_away(arrival_ts: int, now_ts: Optional[int] = None) -> float:
    if now_ts is None:
        now_ts = int(time.time())
    return (arrival_ts - now_ts) / 60.0


def extract_arrivals(
    feed: gtfs_realtime_pb2.FeedMessage,
    route_id: str = "A",
    stop_id: str = "A12N",
    max_results: int = 4,
    
) -> List[Dict[str, Any]]:
    """
    Return a list of predicted arrivals at (route_id, stop_id).
    We look for TripUpdate entities and then StopTimeUpdate entries matching stop_id.
    """
    now_ts = int(time.time())
    results: List[Dict[str, Any]] = []

    for ent in feed.entity:
        if not ent.HasField("trip_update"):
            continue

        tu = ent.trip_update
        trip = tu.trip

        # Filter to desired route (A). Some feeds may omit route_id in certain cases;
        # if so, you can relax this check.
        if trip.route_id and trip.route_id != route_id:
            continue

        # Search for our stop_id within stop_time_update list
        for stu in tu.stop_time_update:
            if stu.stop_id != stop_id:
                continue

            # GTFS-RT: arrival/departure may be present; we prefer arrival.time.
            arr_ts = stu.arrival.time if stu.HasField("arrival") and stu.arrival.time else 0
            dep_ts = stu.departure.time if stu.HasField("departure") and stu.departure.time else 0
            best_ts = arr_ts or dep_ts
            if not best_ts:
                continue

            results.append(
                {
                    "entity_id": ent.id,
                    "route_id": trip.route_id or route_id,
                    "trip_id": trip.trip_id,
                    "start_date": trip.start_date,
                    "stop_id": stop_id,
                    "arrival_ts": arr_ts or None,
                    "departure_ts": dep_ts or None,
                    "best_ts": best_ts,
                    "best_time_local": unix_to_local_str(best_ts),
                    "minutes_away": minutes_away(best_ts, now_ts),
                }
            )

    # Sort by soonest arrival
    results.sort(key=lambda r: r["best_ts"])
    return results[:max_results]


def dump_json(feed: gtfs_realtime_pb2.FeedMessage, path: str) -> None:
    d = MessageToDict(feed, preserving_proto_field_name=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(d, f, indent=2, ensure_ascii=False)


def dump_text(feed: gtfs_realtime_pb2.FeedMessage, path: str) -> None:
    # Protobuf text format (very readable, often large)
    with open(path, "w", encoding="utf-8") as f:
        f.write(str(feed))


def main() -> int:
    ap = argparse.ArgumentParser(description="Decode MTA GTFS-RT and extract A train arrivals at 145 St (uptown).")
    ap.add_argument("--url", default=FEED_URL_DEFAULT, help="GTFS-RT feed URL")
    ap.add_argument("--route", default="A", help="Route ID to filter (default: A)")
    ap.add_argument("--stop", default="A12N", help="Stop ID to filter (default: A12N = 145 St uptown)")
    ap.add_argument("--limit", type=int, default=10, help="Max arrivals to print")
    ap.add_argument("--dump-json", default=None, help="Write the ENTIRE decoded feed to JSON at this path")
    ap.add_argument("--dump-text", default=None, help="Write the ENTIRE decoded feed to protobuf-text at this path")
    args = ap.parse_args()

    api_key = os.getenv("MTA_API_KEY")

    try:
        raw = fetch_feed(args.url, api_key=api_key)
        feed = parse_feed(raw)
    except Exception as e:
        print(f"❌ Error fetching/parsing feed: {e}", file=sys.stderr)
        return 1

    # Optional: dump entire feed (this is what you want if you mean “entirety of the response”)
    if args.dump_json:
        dump_json(feed, args.dump_json)
        print(f"✅ Wrote full feed JSON to: {args.dump_json}")
    if args.dump_text:
        dump_text(feed, args.dump_text)
        print(f"✅ Wrote full feed protobuf-text to: {args.dump_text}")

    arrivals = extract_arrivals(feed, route_id=args.route, stop_id=args.stop, max_results=args.limit)

    print("---- QUERY ----")
    print(f"Route: {args.route}")
    print(f"Stop:  {args.stop}")
    print()

    if not arrivals:
        print("No matching predicted arrivals found in this feed snapshot.")
        print("Tips:")
        print("- Try again in ~30 seconds; realtime feeds can fluctuate.")
        print("- If you meant '145 St on the A line' but get nothing, verify stop_id (A12N) in your static GTFS.")
        return 0

    print("---- NEXT ARRIVALS ----")
    for r in arrivals:
        mins = r["minutes_away"]
        when = r["best_time_local"]
        trip_id = r["trip_id"] or "(no trip_id)"
        # Only show future arrivals in a nice way, but still print past if any
        flag = "⏳" if mins >= 0 else "⌛(past)"
        print(f"{flag} in {mins:6.2f} min | {when} | trip_id={trip_id}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())




