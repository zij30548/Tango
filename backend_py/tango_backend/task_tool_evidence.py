"""合并已校验的整条候选记录，不跨响应拼接互相冲突的字段。"""

from datetime import datetime
from .compat import digest, utf16_len, utf16_slice


def _date(value):
    return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()


def mergeToolEvidence(entries, maximumTracks=40):
    completed = [
        e
        for e in entries
        if e.get("status") == "completed"
        and e.get("result")
        and not e["result"].get("error")
    ]
    if not completed:
        return None
    pool, sources = {}, {}

    def quality(track, records):
        supports = any(
            s.get("id") == track.get("sourceId")
            and s.get("name") == track.get("sourceName")
            and (not s.get("sourceKind") or s["sourceKind"] == track.get("sourceKind"))
            for s in records
        )
        return (
            int(track.get("durationMs", 0) > 0) * 4
            + int(supports) * 2
            + int((track.get("styleEvidence") or {}).get("status") == "source_matched")
        )

    for entry in completed:
        result = entry["result"]
        for source in result["sources"]:
            key = f"{source.get('sourceKind') or 'unknown'}:{source.get('id') or source.get('sourceId') or 0}:{source.get('name')}"
            if key not in sources or _date(source["checkedAt"]) >= _date(
                sources[key]["checkedAt"]
            ):
                sources[key] = source
        for track in result["tracks"]:
            prior = pool.get(track["id"])
            if not prior or quality(track, result["sources"]) > quality(
                prior[0], prior[1]
            ):
                pool[track["id"]] = (track, result["sources"])
    recent = [t["id"] for t in completed[-1]["result"]["tracks"]]
    earlier = list(pool)
    order = []
    for i in range(max(len(recent), len(earlier))):
        for sequence in (recent, earlier):
            if i < len(sequence) and sequence[i] not in order:
                order.append(sequence[i])
    tracks = [pool[i][0] for i in order[:maximumTracks]]
    return {
        **completed[-1]["result"],
        "tracks": tracks,
        "sources": list(sources.values()),
        "checkedAt": min((e["result"]["checkedAt"] for e in completed), key=_date),
        "evidence": {
            "attempts": len(completed),
            "resultHashes": [
                e.get("resultHash") or digest(e["result"]) for e in completed
            ],
            "checkedAtByAttempt": [
                {
                    "stepId": e.get("stepId"),
                    "revision": e.get("revision"),
                    "checkedAt": e["result"]["checkedAt"],
                }
                for e in completed
            ],
            "uniqueCandidates": len(pool),
            "retainedCandidates": len(tracks),
            "truncatedCandidates": max(0, len(pool) - len(tracks)),
        },
    }


def observerAssessment(value):
    if (
        not isinstance(value, dict)
        or value.get("decision") not in ("done", "retry")
        or not isinstance(value.get("queries"), list)
        or len(value["queries"]) > 2
    ):
        return None
    if (
        any(
            not isinstance(q, str) or not q.strip() or utf16_len(q) > 80
            for q in value["queries"]
        )
        or "note" in value
        and not isinstance(value["note"], str)
    ):
        return None
    return {
        "decision": value["decision"],
        "queries": value["queries"],
        "note": utf16_slice(value.get("note", ""), 160),
    }
