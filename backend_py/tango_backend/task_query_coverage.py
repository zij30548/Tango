"""按已确认条件生成至多两条有界召回提示，并记录覆盖与遗漏原因。"""

import re
from .compat import utf16_len
from .task_schema import clean
from .task_language import catalogLanguageQueryPrefix
from .task_style_evidence import adjustmentStyles, styleQueryAlternatives


def sceneQueryPlan(task, proposed=None, options=None, **kwargs):
    proposed = proposed or []
    options = {**(options or {}), **kwargs}
    attempt, history = options.get("attempt", 0), options.get("history", [])
    brief = task["brief"]
    styles = [clean(s, 40) for s in brief.get("styles", [])]
    excluded = lambda s: bool(
        re.match(
            r"^(?:不要|不听|不喜欢|不想(?:要|听)?|不需要|排除|避免|拒绝|非|without\s+|exclude\s+|no\s+|not\s+)(.+)$",
            s,
            re.I,
        )
    )
    fields = [
        ("scene", clean(brief.get("scene"), 160), True),
        ("requiredArtist", clean(brief.get("requiredArtist"), 100), True),
        (
            "catalogLanguage",
            catalogLanguageQueryPrefix(brief.get("catalogLanguage")),
            True,
        ),
        *[(f"style:{i}", s, not excluded(s)) for i, s in enumerate(styles)],
    ]
    fields = [f for f in fields if f[1]]
    query_fields = [f for f in fields if f[2]]
    focused = [
        clean(s, 40)
        for s in adjustmentStyles(brief, task.get("previousBrief"))
        if clean(s, 40) and not excluded(clean(s, 40))
    ]
    language = catalogLanguageQueryPrefix(brief.get("catalogLanguage"))
    artist = clean(brief.get("requiredArtist"), 100)
    positive = [value for field, value, _ in query_fields if field.startswith("style:")]
    completed = [
        e
        for e in history
        if e.get("status") == "completed"
        and e.get("skill") == "scene.research"
        and e.get("result")
    ]
    prior_queries = [
        q for e in completed for q in e.get("input", {}).get("queries", [])
    ]
    broadening = attempt > 0 and bool(completed)
    targets = focused or positive
    buckets = [[], []]

    def append(index, value):
        if (
            not value
            or utf16_len(value) > 80
            or any(value.lower() in term.lower() for term in buckets[index])
        ):
            return False
        reduced = [term for term in buckets[index] if term.lower() not in value.lower()]
        if utf16_len(" ".join([*reduced, value])) > 80:
            return False
        buckets[index] = [*reduced, value]
        return True

    for value in [
        "" if broadening else clean(brief.get("scene"), 160),
        language,
        *(targets if broadening else positive),
    ]:
        append(0, value)
    for value in [
        artist,
        language,
        *[(styleQueryAlternatives(s) or [s])[0] if broadening else s for s in targets],
    ]:
        append(1, value)
    if not buckets[1]:
        append(1, clean(brief.get("scene"), 160))
    for _, value, _ in query_fields:
        if not any(
            any(value.lower() in term.lower() for term in terms) for terms in buckets
        ) and not (
            broadening and any(value.lower() in q.lower() for q in prior_queries)
        ):
            if not append(0, value):
                append(1, value)
    queries = list(
        dict.fromkeys(
            " ".join(terms)
            for terms in buckets
            if terms and (not broadening or " ".join(terms) not in prior_queries)
        )
    )
    if not queries and not broadening:
        queries = ["音乐"]
    queries = queries[:2]
    coverage = []
    for field, value, queryable in fields:
        covered = queryable and any(value.lower() in q.lower() for q in queries)
        prior = queryable and any(value.lower() in q.lower() for q in prior_queries)
        item = {
            "field": field,
            "value": value,
            "status": "query_covered" if covered else "structured_only",
            "queryIndices": [
                i for i, q in enumerate(queries) if value.lower() in q.lower()
            ]
            if covered
            else [],
        }
        if prior:
            item.update(
                previouslyCovered=True,
                priorStepIds=[
                    e.get("stepId")
                    for e in completed
                    if any(
                        value.lower() in q.lower()
                        for q in e.get("input", {}).get("queries", [])
                    )
                ],
            )
        if not covered:
            item["reason"] = (
                "negative_style_not_positive_query"
                if not queryable
                else "covered_in_completed_attempt"
                if prior
                else "bounded_query_length"
            )
        coverage.append(item)
    return {
        "queries": queries,
        "strategy": "confirmed_condition_history_union"
        if broadening
        else "confirmed_condition_union",
        "exhausted": not queries,
        "proposalPolicy": "unconfirmed_proposal_terms_ignored",
        "ignoredProposalCount": len(proposed),
        "coverage": coverage,
    }
