"""SPSA search-parameter tuner for Koi.

The tool tunes search-policy margins by staging perturbed copies of the
``detail/`` headers, building each variant, playing a short match, and applying
the simultaneous-perturbation-stochastic-approximation update. Nothing is edited
in ``src/``: every variant is written under a caller-provided staging root, and
adopting a tuned value remains a separate, gated change (the Mac speed gate and
the strength A/B rules in AGENTS.md still apply).

Builds and matches are delegated to a runner command supplied by the caller so
the same driver works for the Mac harness and for a local checkout. The runner
receives the staged directory, a tag, and the parameters JSON path via
``{dir}``, ``{tag}`` and ``{params}`` placeholders, and must write
``<dir>/score.json`` containing ``{"score": <side-to-move score fraction>}``.

Usage:
    python tools/measurement/spsa_search.py --emit-default-manifest spsa.json
    python tools/measurement/spsa_search.py --manifest spsa.json \
        --staging-root artifacts/training/spsa --iterations 40 \
        --runner "bash tools/measurement/spsa_runner.sh {dir} {tag} {params}"

The default manifest tunes the razor, child-futility, reverse-futility,
prob-cut and null-move margins carried by ``search_constants.hpp`` plus the
null-move and late-move shapes in ``search_policy.hpp``.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import random
import re
import subprocess
import sys
from dataclasses import asdict, dataclass

REPORT_SCHEMA = "koi-spsa-report-v1"
MANIFEST_SCHEMA = "koi-spsa-manifest-v1"


class TuningError(RuntimeError):
    """Raised for unusable manifests or staging failures."""


@dataclass(frozen=True)
class Parameter:
    """One tunable integer carried by a literal pattern with one capture group."""

    name: str
    path: str
    pattern: str
    value: int
    minimum: int
    maximum: int
    step: int


DEFAULT_PARAMETERS = (
    Parameter(
        "razor_margin_per_depth_squared",
        "src/koi/detail/search_constants.hpp",
        r"kRazorMarginPerDepthSquared = (\d+)",
        120, 60, 220, 8,
    ),
    Parameter(
        "child_futility_base_margin",
        "src/koi/detail/search_constants.hpp",
        r"kChildFutilityBaseMargin = (\d+)",
        96, 48, 180, 8,
    ),
    Parameter(
        "reverse_futility_base_margin",
        "src/koi/detail/search_constants.hpp",
        r"kReverseFutilityBaseMargin = (\d+)",
        80, 40, 160, 8,
    ),
    Parameter(
        "null_move_static_margin",
        "src/koi/detail/search_constants.hpp",
        r"kNullMoveStaticMargin = (\d+)",
        96, 48, 180, 8,
    ),
    Parameter(
        "transposition_prob_cut_margin",
        "src/koi/detail/search_constants.hpp",
        r"kTranspositionProbCutMargin = (\d+)",
        428, 300, 560, 16,
    ),
    Parameter(
        "null_move_floor_slope",
        "src/koi/detail/search_policy.hpp",
        r"- (\d+) \* depth -",
        13, 8, 20, 1,
    ),
    Parameter(
        "null_move_improving_credit",
        "src/koi/detail/search_policy.hpp",
        r"(?m)^\s+\(improving \? (\d+) : 0\);$",
        47, 20, 80, 4,
    ),
    Parameter(
        "null_move_base_reduction",
        "src/koi/detail/search_policy.hpp",
        r"(\d+) \+ depth / 3 \+ excess / 256",
        7, 4, 10, 1,
    ),
    Parameter(
        "lmr_step_one_depth",
        "src/koi/detail/search_policy.hpp",
        r"depth >= (\d+) && move_number >= 7\) \{",
        7, 5, 10, 1,
    ),
    Parameter(
        "lmr_step_two_depth",
        "src/koi/detail/search_policy.hpp",
        r"depth >= (\d+) && move_number >= 14\) \{",
        11, 8, 15, 1,
    ),
    Parameter(
        "lmr_step_three_depth",
        "src/koi/detail/search_policy.hpp",
        r"depth >= (\d+) && move_number >= 24\) \{",
        16, 12, 22, 1,
    ),
)


def default_manifest() -> dict[str, object]:
    return {
        "schema": MANIFEST_SCHEMA,
        "parameters": [asdict(parameter) for parameter in DEFAULT_PARAMETERS],
    }


def load_manifest(path: pathlib.Path) -> list[Parameter]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise TuningError(f"manifest is unreadable: {error}") from error
    if document.get("schema") != MANIFEST_SCHEMA:
        raise TuningError(f"manifest schema must be {MANIFEST_SCHEMA}")
    raw = document.get("parameters")
    if not isinstance(raw, list) or not raw:
        raise TuningError("manifest must contain a non-empty parameters list")
    parameters: list[Parameter] = []
    names: set[str] = set()
    for entry in raw:
        try:
            parameter = Parameter(
                name=str(entry["name"]),
                path=str(entry["path"]),
                pattern=str(entry["pattern"]),
                value=int(entry["value"]),
                minimum=int(entry["minimum"]),
                maximum=int(entry["maximum"]),
                step=int(entry["step"]),
            )
        except (TypeError, KeyError, ValueError) as error:
            raise TuningError(f"malformed parameter entry: {error}") from error
        if parameter.name in names:
            raise TuningError(f"duplicate parameter name: {parameter.name}")
        if parameter.minimum > parameter.maximum:
            raise TuningError(f"parameter {parameter.name} has an empty range")
        if not parameter.minimum <= parameter.value <= parameter.maximum:
            raise TuningError(f"parameter {parameter.name} starts outside its range")
        if parameter.step < 1:
            raise TuningError(f"parameter {parameter.name} needs a positive step")
        names.add(parameter.name)
        parameters.append(parameter)
    return parameters


def _compiled(parameter: Parameter) -> re.Pattern[str]:
    pattern = re.compile(parameter.pattern)
    if pattern.groups != 1:
        raise TuningError(
            f"parameter {parameter.name} must carry exactly one capture group")
    return pattern


def apply_values(text: str, parameter: Parameter, value: int) -> str:
    """Replace the single captured literal for one parameter."""
    pattern = _compiled(parameter)
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        raise TuningError(
            f"parameter {parameter.name} matched {len(matches)} times in "
            f"{parameter.path}; expected exactly one")
    match = matches[0]
    start, end = match.span(1)
    replacement = str(value)
    return text[:start] + replacement + text[end:]


def stage_variant(
    root: pathlib.Path,
    parameters: list[Parameter],
    values: dict[str, int],
    output: pathlib.Path,
) -> pathlib.Path:
    """Write one variant tree (only the touched headers) under ``output``."""
    by_path: dict[str, str] = {}
    for parameter in parameters:
        if parameter.name not in values:
            raise TuningError(f"missing value for {parameter.name}")
        value = values[parameter.name]
        if not parameter.minimum <= value <= parameter.maximum:
            raise TuningError(
                f"value {value} for {parameter.name} is outside its range")
        text = by_path.get(parameter.path)
        if text is None:
            source = root / parameter.path
            try:
                text = source.read_text(encoding="utf-8")
            except OSError as error:
                raise TuningError(f"cannot read {source}: {error}") from error
        by_path[parameter.path] = apply_values(text, parameter, value)
    for relative, text in by_path.items():
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(text, encoding="utf-8")
    return output


def spsa_step(
    values: dict[str, int],
    parameters: list[Parameter],
    delta: dict[str, int],
    score_plus: float,
    score_minus: float,
    coefficient: float,
    rng: random.Random | None = None,
) -> dict[str, int]:
    """One simultaneous-perturbation update, clamped to every parameter range.

    Integer knobs take whole steps; the fractional part becomes a stochastic
    rounding probability so small gradients still move a parameter sometimes.
    """
    rng = rng if rng is not None else random.Random(0)
    gradient = (score_plus - score_minus) / 2.0
    updated: dict[str, int] = {}
    for parameter in parameters:
        change = coefficient * gradient * delta[parameter.name]
        magnitude = abs(change)
        steps = int(magnitude)
        if rng.random() < (magnitude - steps):
            steps += 1
        if change < 0:
            steps = -steps
        candidate = values[parameter.name] + steps
        updated[parameter.name] = max(parameter.minimum,
                                      min(parameter.maximum, candidate))
    return updated


def run(
    root: pathlib.Path,
    parameters: list[Parameter],
    staging_root: pathlib.Path,
    runner: str,
    iterations: int,
    games: int,
    seed: int,
    a0: float,
    alpha: float,
    a_bar: float,
    report_path: pathlib.Path,
    dry_run: bool,
) -> dict[str, object]:
    rng = random.Random(seed)
    values = {parameter.name: parameter.value for parameter in parameters}
    history: list[dict[str, object]] = []

    for iteration in range(iterations):
        delta = {parameter.name: rng.choice((-1, 1)) for parameter in parameters}
        plus: dict[str, int] = {}
        minus: dict[str, int] = {}
        for parameter in parameters:
            sign = delta[parameter.name]
            plus[parameter.name] = max(
                parameter.minimum,
                min(parameter.maximum, values[parameter.name] + parameter.step * sign))
            minus[parameter.name] = max(
                parameter.minimum,
                min(parameter.maximum, values[parameter.name] - parameter.step * sign))
        coefficient = a0 / ((iteration + a_bar) ** alpha)

        staged: dict[str, dict[str, int]] = {}
        for tag, variant in (("plus", plus), ("minus", minus)):
            output = staging_root / f"iter{iteration:03d}-{tag}"
            stage_variant(root, parameters, variant, output)
            staged[tag] = variant
            staged[tag]["__dir"] = str(output)  # type: ignore[assignment]

        if dry_run:
            history.append({"iteration": iteration, "delta": delta,
                            "plus": plus, "minus": minus, "dry_run": True})
            continue

        scores: dict[str, float] = {}
        for tag in ("plus", "minus"):
            output = pathlib.Path(staged[tag]["__dir"])
            params_json = output / "parameters.json"
            params_json.write_text(
                json.dumps({name: value for name, value in staged[tag].items()
                            if name != "__dir"}, indent=2), encoding="utf-8")
            command = (runner
                       .replace("{dir}", str(output))
                       .replace("{tag}", tag)
                       .replace("{params}", str(params_json))
                       .replace("{games}", str(games)))
            completed = subprocess.run(command, shell=True)
            if completed.returncode != 0:
                raise TuningError(f"runner failed for {tag} at iteration {iteration}")
            score_file = output / "score.json"
            try:
                score = float(json.loads(score_file.read_text(encoding="utf-8"))["score"])
            except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError) as error:
                raise TuningError(
                    f"runner did not leave a usable score.json in {output}: {error}"
                ) from error
            scores[tag] = score

        values = spsa_step(values, parameters, delta, scores["plus"], scores["minus"],
                           coefficient, rng)
        history.append({
            "iteration": iteration,
            "delta": delta,
            "plus": staged["plus"],
            "minus": staged["minus"],
            "score_plus": scores["plus"],
            "score_minus": scores["minus"],
            "coefficient": coefficient,
            "values": dict(values),
        })
        report = {
            "schema": REPORT_SCHEMA,
            "parameters": [asdict(parameter) for parameter in parameters],
            "iterations": history,
            "final": dict(values),
        }
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

    final = {
        "schema": REPORT_SCHEMA,
        "parameters": [asdict(parameter) for parameter in parameters],
        "iterations": history,
        "final": values,
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(final, indent=2), encoding="utf-8")
    return final


def stage_final(root: pathlib.Path, parameters: list[Parameter],
                report: dict[str, object], out_dir: pathlib.Path) -> pathlib.Path:
    """Stage the report's final theta as a buildable candidate tree."""
    values = report.get("final")
    if not isinstance(values, dict):
        raise TuningError("report has no final theta to stage")
    stage_variant(root, parameters, values, out_dir)
    return out_dir


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--manifest", type=pathlib.Path,
                        help="manifest JSON; omit with --emit-default-manifest")
    parser.add_argument("--emit-default-manifest", type=pathlib.Path,
                        help="write the built-in parameter manifest and exit")
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path("."),
                        help="repository root holding the source headers")
    parser.add_argument("--staging-root", type=pathlib.Path,
                        default=pathlib.Path("artifacts/training/spsa"))
    parser.add_argument("--runner",
                        help="command template using {dir}, {tag}, {params}, {games}")
    parser.add_argument("--iterations", type=int, default=40)
    parser.add_argument("--games", type=int, default=200)
    parser.add_argument("--seed", type=int, default=20261020)
    parser.add_argument("--a0", type=float, default=2.0)
    parser.add_argument("--alpha", type=float, default=0.602)
    parser.add_argument("--a-bar", type=float, default=10.0)
    parser.add_argument("--report", type=pathlib.Path,
                        default=pathlib.Path("artifacts/training/spsa-report.json"))
    parser.add_argument("--dry-run", action="store_true",
                        help="stage one iteration of variants and stop")
    parser.add_argument("--stage-final", nargs=2, metavar=("REPORT", "DIR"),
                        help="stage the final theta from a report and exit")
    arguments = parser.parse_args(argv)

    if arguments.stage_final:
        report_path, out_dir = (pathlib.Path(item) for item in arguments.stage_final)
        try:
            report = json.loads(report_path.read_text(encoding="utf-8"))
            raw_parameters = report["parameters"]
            parameters = [Parameter(**item) for item in raw_parameters]
            stage_final(arguments.root, parameters, report, out_dir)
        except (OSError, json.JSONDecodeError, KeyError, TypeError, TuningError) as error:
            print(f"spsa stage-final failed: {error}", file=sys.stderr)
            return 1
        print(f"staged {out_dir}")
        return 0

    if arguments.emit_default_manifest:
        arguments.emit_default_manifest.parent.mkdir(parents=True, exist_ok=True)
        arguments.emit_default_manifest.write_text(
            json.dumps(default_manifest(), indent=2), encoding="utf-8")
        print(f"wrote {arguments.emit_default_manifest}")
        return 0
    if not arguments.manifest:
        parser.error("--manifest is required unless --emit-default-manifest is used")
    if not arguments.runner and not arguments.dry_run:
        parser.error("--runner is required unless --dry-run is used")

    try:
        parameters = load_manifest(arguments.manifest)
        result = run(
            root=arguments.root,
            parameters=parameters,
            staging_root=arguments.staging_root,
            runner=arguments.runner or "",
            iterations=1 if arguments.dry_run else arguments.iterations,
            games=arguments.games,
            seed=arguments.seed,
            a0=arguments.a0,
            alpha=arguments.alpha,
            a_bar=arguments.a_bar,
            report_path=arguments.report,
            dry_run=arguments.dry_run,
        )
    except TuningError as error:
        print(f"spsa search failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result.get("final"), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
