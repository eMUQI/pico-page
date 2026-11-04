# Compare diagnostic reports

## Sub-features

Compare complete run conditions, workload, build provenance, and sample composition. Keep incomplete evidence incomparable. Preserve legitimate cross-version comparisons and the documented diagnostic-flag normalization.

## How to get to it (user POV)

A developer compares two workbench `run.json` files using `tools/device_workbench.py --compare`. The command must not suggest a performance comparison when required conditions are missing from both files.

## Driving it with the CLI

Run `python3 tools/device_workbench.py --help`, then `python3 -m unittest discover -s tests -p test_device_workbench.py -v`. Run the real `--compare` command with the two task input paths. For a regression without device captures, prepare synthetic JSON using the complete fields in `test_compare_conditions`; label the files synthetic and retain them with the output. Invoke the same CLI before and after the fix, and check the literal expected `comparable` value and reason. Keep a complete positive control alongside the failing input.

## Gotchas

The process can exit successfully while its JSON says the reports are incomparable. Check both. A synthetic report proves comparator behavior only, not a device performance change. Full requirements are in `docs/development/diagnostic-workbench.md` under the comparison command.
