"""有界毫秒时长子集选择；一秒量化只用于状态分桶，保留真实毫秒比较。"""

from array import array
import math


def selectCountSubset(candidates, options):
    """固定首数的有界动态规划；有限分桶近似时长，不宣称全局最优。"""
    pool = candidates[:60]
    count = max(0, min(options["exactCount"], len(pool), 30))
    target, base = options.get("targetMs", 0), options.get("baseMs", 0)
    required = options.get("requiredFlags", 0)
    prefer = options.get("preferenceWithinTolerance", False)
    tolerance = max(60000, target * 0.1) if target else 0
    quantum, maximum_states = 1000, 256
    # 每层按曲数/证据分组，各组最多256状态；固定60候选与30曲上限控制资源。
    states = {(0, 0): {0: (0, 0, 0, 0)}}
    for index, track in enumerate(pool):
        duration = track.get("durationMs", 0)
        flags = track.get("flags", 0) & required
        for used in range(min(count - 1, index), -1, -1):
            for evidence in range(4 if required else 1):
                source = states.get((used, evidence))
                if not source:
                    continue
                destination = states.setdefault((used + 1, evidence | flags), {})
                for total, rank, support, mask in list(source.values()):
                    value = (total + duration, rank + index + 1, support + int(track.get("styleSupport") is True), mask | (1 << index))
                    bucket = (total + duration + quantum // 2) // quantum if target else 0
                    old = destination.get(bucket)
                    key = lambda row: (abs(base + row[0] - target), row[1]) if target else (row[1],)
                    if old is None or key(value) < key(old):
                        destination[bucket] = value
        remaining = sorted(t.get("durationMs", 0) for t in pool[index + 1:])
        for (used, evidence), values in list(states.items()):
            needed = count - used
            if needed > len(remaining):
                del states[(used, evidence)]
                continue
            if len(values) <= maximum_states:
                continue
            lower = sum(remaining[:needed])
            upper = sum(remaining[-needed:]) if needed else 0
            def bound(row):
                low, high = base + row[0] + lower, base + row[0] + upper
                distance = max(low - target, target - high, 0)
                return (distance, row[1], abs(base + row[0] - target), -row[2])
            states[(used, evidence)] = dict(sorted(values.items(), key=lambda item: bound(item[1]))[:maximum_states])
    def quality(item):
        evidence, row = item
        satisfied = (evidence & required).bit_count()
        error = abs(base + row[0] - target) if target else 0
        within = bool(target) and error <= tolerance
        if prefer and target:
            return (-satisfied, not within, -row[2] if within else 0, row[1] if within else error, error)
        return (-satisfied, error, row[1])
    choices = [(evidence, row) for (used, evidence), values in states.items() if used == count for row in values.values()]
    best = min(choices, key=quality)[1] if choices else (0, 0, 0, 0)
    return {
        "indices": [i for i in range(len(pool)) if best[3] & (1 << i)],
        "algorithm": "bounded_count_duration_dp",
        "quantizationMs": quantum if target else 0,
        "maximumQuantizationSpreadMs": count * quantum if target else 0,
        "selectionPolicy": "fixed_count_preference_within_tolerance" if prefer and target else "fixed_count_closest_duration" if target else "fixed_count_catalog_order",
        "toleranceMs": tolerance,
        "withinTolerance": bool(target) and abs(base + best[0] - target) <= tolerance,
        "maximumStatesPerCountAndEvidence": maximum_states,
    }


def selectDurationSubset(candidates, options=None, **kwargs):
    options = {**(options or {}), **kwargs}
    if "exactCount" in options:
        return selectCountSubset(candidates, options)
    target = options.get("targetMs", 0)
    base = options.get("baseMs", 0)
    pool = candidates[:60]
    slots = max(0, min(options.get("maximumTracks", 30), len(pool), 30))
    minimum = options.get("minimumTracks", 0)
    required = options.get("requiredFlags", 0)
    prefer = options.get("preferenceWithinTolerance", False)
    tolerance = max(60000, target * 0.1) if target else 0
    policy = "preference_within_tolerance" if prefer else "closest_duration"
    if not slots:
        return {
            "indices": [],
            "algorithm": "bounded_duration_dp",
            "quantizationMs": 1000,
            "maximumQuantizationSpreadMs": 0,
            "selectionPolicy": policy,
            "toleranceMs": tolerance,
            "withinTolerance": bool(target) and abs(base - target) <= tolerance,
        }
    if not target:
        return {
            "indices": list(range(slots)),
            "algorithm": "catalog_order",
            "quantizationMs": 0,
            "maximumQuantizationSpreadMs": 0,
            "selectionPolicy": "catalog_order",
            "toleranceMs": 0,
            "withinTolerance": False,
        }
    remaining = max(0, target - base)
    weights = [
        max(1, math.floor(t.get("durationMs", 0) / 1000 + 0.5))
        if t.get("durationMs", 0) > 0
        else 0
        for t in pool
    ]
    max_duration = max(0, *(t.get("durationMs", 0) for t in pool))
    evidence_weight = math.inf if required else 0
    for i, track in enumerate(pool):
        if track.get("flags", 0) & required == required:
            evidence_weight = min(evidence_weight, weights[i])
        if slots >= 2:
            for j in range(i + 1, len(pool)):
                if (
                    track.get("flags", 0) | pool[j].get("flags", 0)
                ) & required == required:
                    evidence_weight = min(evidence_weight, weights[i] + weights[j])
    upper = max(
        math.ceil((remaining + max_duration) / 1000) + slots,
        evidence_weight if math.isfinite(evidence_weight) else 0,
    )
    bins = min(sum(weights), upper) + 1
    flags = 4 if required else 1
    cells = (slots + 1) * flags * bins
    lanes = 2 if prefer else 1
    # 固定宽度数组避免 Python 整数对象放大 DP 的内存占用。
    exact = array("i", [-1]) * (cells * lanes)
    rank = array("i", [0]) * (cells * lanes)
    support = bytearray(cells * lanes)
    masks = array("Q", [0]) * (cells * lanes)
    exact[0] = 0
    processed = 0
    for item, track in enumerate(pool):
        weight, duration, item_flags = (
            weights[item],
            track.get("durationMs", 0),
            track.get("flags", 0) & required,
        )
        for count in range(min(slots - 1, item), -1, -1):
            for evidence in range(flags):
                offset = (count * flags + evidence) * bins
                next_offset = ((count + 1) * flags + (evidence | item_flags)) * bins
                for bucket in range(min(bins - 1 - weight, processed) + 1):
                    for lane in range(lanes):
                        source = lane * cells + offset + bucket
                        if exact[source] < 0:
                            continue
                        total, score = exact[source] + duration, rank[source] + item + 1
                        supported = support[source] + int(
                            track.get("styleSupport") is True
                        )
                        error = abs(total - remaining)
                        for destination_lane in range(lanes):
                            dest = (
                                destination_lane * cells + next_offset + bucket + weight
                            )
                            old_error = abs(exact[dest] - remaining)
                            better = (
                                (
                                    error < old_error
                                    or error == old_error
                                    and score < rank[dest]
                                )
                                if destination_lane == 0
                                else (
                                    supported > support[dest]
                                    or supported == support[dest]
                                    and (
                                        score < rank[dest]
                                        or score == rank[dest]
                                        and error < old_error
                                    )
                                )
                            )
                            if exact[dest] < 0 or better:
                                exact[dest], rank[dest], support[dest] = (
                                    total,
                                    score,
                                    supported,
                                )
                                masks[dest] = masks[source] | (1 << item)
        processed += weight
    best = best_count = best_support = 0
    best_satisfied = -1
    best_error = best_rank = math.inf
    best_within = False
    for count in range(slots + 1):
        if count < minimum:
            continue
        for evidence in range(flags):
            satisfied = int(bool(evidence & required & 1)) + int(
                bool(evidence & required & 2)
            )
            offset = (count * flags + evidence) * bins
            for bucket in range(bins):
                for lane in range(lanes):
                    index = lane * cells + offset + bucket
                    if exact[index] < 0:
                        continue
                    error = abs(base + exact[index] - target)
                    within = error <= tolerance
                    better = satisfied > best_satisfied
                    if satisfied == best_satisfied:
                        if prefer and within != best_within:
                            better = within
                        elif prefer and within:
                            support_difference = support[index] * max(
                                1, best_count
                            ) - best_support * max(1, count)
                            rank_difference = (
                                rank[index] * best_count - best_rank * count
                                if count and best_count
                                else -1
                                if count
                                else 1
                                if best_count
                                else 0
                            )
                            better = (
                                support_difference > 0
                                or support_difference == 0
                                and (
                                    rank_difference < 0
                                    or rank_difference == 0
                                    and error < best_error
                                )
                            )
                        else:
                            better = (
                                error < best_error
                                or error == best_error
                                and rank[index] < best_rank
                            )
                    if better:
                        (
                            best,
                            best_count,
                            best_satisfied,
                            best_error,
                            best_rank,
                            best_support,
                            best_within,
                        ) = (
                            index,
                            count,
                            satisfied,
                            error,
                            rank[index],
                            support[index],
                            within,
                        )
    return {
        "indices": [i for i in range(len(pool)) if masks[best] & (1 << i)],
        "algorithm": "bounded_duration_dp",
        "quantizationMs": 1000,
        "maximumQuantizationSpreadMs": slots * 1000,
        "selectionPolicy": policy,
        "toleranceMs": tolerance,
        "withinTolerance": best_within,
    }
